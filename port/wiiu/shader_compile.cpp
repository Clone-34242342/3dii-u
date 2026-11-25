// Shaders the shipped set lacks are compiled on the console itself, on a thread of their own, with CafeGLSL's library for the Wii U
// (third_party/cafeglsl_ppc: version 1.0, where the PC build runs 0.2, and its programs differ: latte_program.h). A miss draws with its
// fallback meanwhile and takes the real shader once the GPU thread installs it (gfx_pump). Validated native programs are restored on later
// starts; the GLSL journal remains the recovery path when a native entry is missing, corrupt, or from another compiler/generator.
#include <cafeglsl/CafeGLSLCompiler.h>
#include <cafeglsl/gfd.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gfd.h>
#include <malloc.h>
#include <sys/stat.h>

#include <cctype>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "latte_program.h"
#include "shader_compile.h"
#include "shader_native_cache.h"
#include "shadergen.h"
#include "wiiu.h"



namespace gpu {
namespace {
struct Job {
    bool vs, cached;
    u64 key;
    u32 inputs;
    std::string glsl;
    u32 tries = 0;  // compiles that came back hollow (compile_thread)
};
OSMutex mutex;
std::deque<Job> jobs;
std::vector<CompileDone> done;
std::unordered_set<u64> known;  // asked for once: the cache and a live miss may name the same shader
constexpr u32 STACK = 4 << 20;  // the compiler recurses
OSThread *compiler;
volatile OSTime busy_since;  // when the compile in progress began, 0 between compiles
volatile u32 watched;        // reports written about the compile in progress
uint64_t native_disk_bytes;  // compiler thread only, initialized while scanning the cache at boot

// How many of a generated vertex shader's inputs (v0 to v15) its code reads: named again after being declared
u32 inputs_read(const std::string &glsl) {
    u32 n = 0;
    for (u32 r = 0; r < 16; r++) {
        const std::string name = "v" + std::to_string(r);
        u32 seen = 0;
        for (size_t at = glsl.find(name); at != std::string::npos && seen < 2; at = glsl.find(name, at + 1)) {
            const bool starts = at == 0 || !(isalnum((unsigned char)glsl[at - 1]) || glsl[at - 1] == '_');
            const bool ends = at + name.size() >= glsl.size() || !(isalnum((unsigned char)glsl[at + name.size()]) || glsl[at + name.size()] == '_');
            seen += starts && ends;
        }
        n += seen >= 2;
    }
    return n;
}

// One cache per shipped set: what an older build missed and compiled here is mostly in a newer one's set, and compiling it all again at every
// start kept this thread (and the heap lock it takes all the time) busy for minutes, through Luigi's Mansion: Dark Moon's intro, whose sound
// and pictures stuttered meanwhile (1129 shaders, at ~300 ms each, 2026-11-11). A new build starts its cache empty; the old file goes.
const std::string &cache_path() {
    static std::string p;
    if (p.empty()) {
        char name[48];
        snprintf(name, sizeof name, "/shader_cache_%016llx.bin", (unsigned long long)gpu::shipped_set_id);
        p = wiiu::data_dir() + name;
    }
    return p;
}

std::string native_prefix() {
    char name[48];
    snprintf(name, sizeof name, "shader_native_%016llx_", (unsigned long long)shipped_set_id);
    return name;
}
std::string native_path(const Job &j) {
    char name[32];
    snprintf(name, sizeof name, "%c_%016llx.bin", j.vs ? 'v' : 'p', (unsigned long long)j.key);
    return wiiu::data_dir() + "/" + native_prefix() + name;
}
shader_native_cache::Identity native_identity(const Job &j) {
    const char *version = GetGLSLCompilerVersion();
    return {kShaderGenRevision, shipped_set_id, shader_native_cache::hash(version, strlen(version)), j.key,
            shader_native_cache::hash(j.glsl.data(), j.glsl.size()), j.vs, j.inputs};
}
// Called with fs_begin held. An incomplete cache is disposable, never a partial shader.
std::vector<u8> native_read_file(const std::string &path) {
    std::vector<u8> bytes;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        if (!fseek(f, 0, SEEK_END)) {
            const long n = ftell(f);
            if (n > 0 && (unsigned long)n <= shader_native_cache::kMaxFile && !fseek(f, 0, SEEK_SET)) {
                bytes.resize(n);
                if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
            }
        }
        fclose(f);
    }
    return bytes;
}
bool native_load(const Job &j, CompileDone &d) {
    wiiu::fs_begin(0xC00B);
    const auto file = native_read_file(native_path(j));
    wiiu::fs_end();
    const u8 *gfd;
    size_t size;
    if (!shader_native_cache::decode(file, native_identity(j), gfd, size)) return false;
    const u32 header_size = j.vs ? GFDGetVertexShaderHeaderSize(0, gfd) : GFDGetPixelShaderHeaderSize(0, gfd);
    const u32 program_size = j.vs ? GFDGetVertexShaderProgramSize(0, gfd) : GFDGetPixelShaderProgramSize(0, gfd);
    if (!header_size || !program_size) return false;
    // GFD copies the header (including GX2R fields); own both blocks directly
    // instead of letting that copy erase a WHB-created GX2R buffer's ownership.
    void *header = memalign(64, header_size);
    void *program = memalign(GX2_SHADER_PROGRAM_ALIGNMENT, program_size);
    if (!header || !program || !(j.vs ? GFDGetVertexShader((GX2VertexShader *)header, program, 0, gfd)
                                     : GFDGetPixelShader((GX2PixelShader *)header, program, 0, gfd))) {
        free(program); free(header);
        return false;
    }
    d.from_gfd = true;
    if (j.vs) d.vs_shader = (GX2VertexShader *)header;
    else d.ps_shader = (GX2PixelShader *)header;
    u32 &resources = j.vs ? d.vs_shader->regs.sq_pgm_resources_vs : d.ps_shader->regs.sq_pgm_resources_ps;
    u32 registers = resources & 255;
    if ((j.vs && (d.vs_shader->size <= 16 || (inputs_read(j.glsl) && !d.vs_shader->attribVarCount))) ||
        !fit_program(j.vs ? d.vs_shader->program : d.ps_shader->program, j.vs ? d.vs_shader->size : d.ps_shader->size,
                     registers, shader_budget::kOrdinary)) {
        shader_compile_discard(d);
        return false;
    }
    resources = (resources & ~255u) | registers;
    return true;
}
void native_save(const Job &j, const CompileDone &d) {
    // Include both temporary files in the budget; this cache shares the title's save quota.
    if (!shader_native_cache::fits_budget(native_disk_bytes, shader_native_cache::kMaxFile * 2) ||
        !(j.vs ? shader_native_cache::writer_fits(*d.vs_shader) : shader_native_cache::writer_fits(*d.ps_shader))) return;
    const std::string path = native_path(j), raw = path + ".gsh.tmp", temp = path + ".tmp";
    GFDFile gfd;
    if (j.vs) gfd.vertexShaders.push_back(d.vs_shader);
    else gfd.pixelShaders.push_back(d.ps_shader);
    wiiu::fs_begin(0xC00B);
    if (WriteGFD(gfd, raw)) {
        const auto bytes = shader_native_cache::encode(native_read_file(raw), native_identity(j));
        if (!bytes.empty()) if (FILE *f = fopen(temp.c_str(), "wb")) {
            const bool written = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
            const bool closed = fclose(f) == 0;
            if (written && closed) {
                struct stat st{};
                if (!stat(path.c_str(), &st) && !remove(path.c_str())) native_disk_bytes -= std::min<uint64_t>(native_disk_bytes, st.st_size);
                if (!rename(temp.c_str(), path.c_str())) native_disk_bytes += bytes.size();
            }
        }
    }
    for (const auto *p : {&raw, &temp}) if (remove(p->c_str())) {
        struct stat st{};
        if (!stat(p->c_str(), &st) && st.st_size > 0) native_disk_bytes += st.st_size;
    }
    wiiu::fs_end();  // no extra save flush: cache loss only costs a recompile
}

static void cache_drop_old() {
    const std::string dir = wiiu::data_dir();
    remove((dir + "/shader_cache.bin").c_str());
    native_disk_bytes = 0;
    if (DIR *d = opendir(dir.c_str())) {
        std::vector<std::string> old;
        while (dirent *e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("shader_cache_", 0) == 0 && dir + "/" + n != cache_path()) old.push_back(dir + "/" + n);
            if (n.rfind("shader_native_", 0) == 0) {
                if (n.rfind(native_prefix(), 0) != 0 || n.size() < 4 || n.substr(n.size() - 4) != ".bin") old.push_back(dir + "/" + n);
                else {
                    struct stat st{};
                    if (!stat((dir + "/" + n).c_str(), &st) && st.st_size > 0) native_disk_bytes += st.st_size;
                }
            }
        }
        closedir(d);
        for (const std::string &o : old) {
            if (!remove(o.c_str())) LOG("[glsl] an older build's cache gone: %s\n", o.c_str());
            else if (o.rfind(dir + "/shader_native_", 0) == 0) {
                struct stat st{};
                if (!stat(o.c_str(), &st) && st.st_size > 0) native_disk_bytes += st.st_size;
            }
        }
    }
}

// The file starts with these two words. Its GLSL is what the shader generator of the build that wrote it made of each state, so a build whose
// generator writes a state differently (kShaderGenRevision, shadergen.h) starts the file over: compiled from the old text, a shader would be
// the old generator's, mistakes and all, under the new build's key.
const u32 kCacheHead[2] = {0x53484443, kShaderGenRevision};  // 'SHDC'

// u8 vs, u8 pad[3], u32 inputs, u64 key, u32 length, the GLSL: this console's own bytes, nobody else reads it
void cache_append(const Job &j) {
    wiiu::fs_begin(0xC00B);  // its turn with every other file call
    if (FILE *f = fopen(cache_path().c_str(), "ab")) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) fwrite(kCacheHead, 1, sizeof kCacheHead, f);
        const u32 head[5] = {(u32)j.vs << 24, j.inputs, (u32)(j.key >> 32), (u32)j.key, (u32)j.glsl.size()};
        fwrite(head, 1, sizeof head, f);
        fwrite(j.glsl.data(), 1, j.glsl.size(), f);
        fclose(f);
    }
    wiiu::fs_end();
    // No flush of its own: one takes ~400 ms on the card thread, and the game's next file read waits behind it, which stalled Bowser's Inside Story's
    // battles and NSMB2's menus as their shaders compiled (2026-11-08). The cache goes out with the game's next save or at quitting; lost to a power-off,
    // it's only compiled again. A journal that fills up first is flushed by the write that finds it full (write_retrying).
}

void cache_load() {
    cache_drop_old();
    FILE *f = fopen(cache_path().c_str(), "rb");
    if (!f) return;
    u32 n = 0, head[5];
    if (fread(head, 1, sizeof kCacheHead, f) != sizeof kCacheHead || memcmp(head, kCacheHead, sizeof kCacheHead)) {
        fclose(f);
        if ((f = fopen(cache_path().c_str(), "wb"))) fclose(f);  // another generator's, or from before the file said whose: emptied
        LOG("[glsl] the shader cache was another build's generator's: started over\n");
        return;
    }
    while (fread(head, 1, sizeof head, f) == sizeof head && head[4] < (1 << 20)) {
        Job j{(head[0] >> 24) != 0, true, (u64)head[2] << 32 | head[3], head[1], std::string(head[4], '\0')};
        if (fread(&j.glsl[0], 1, head[4], f) != head[4]) break;
        if (known.insert(j.key).second) {
            jobs.push_back(std::move(j));
            n++;
        }
    }
    fclose(f);
    LOG("[glsl] %u shaders from the cache to restore or compile\n", n);
}

void start_compiler() {
    InitGLSLCompiler();
    LOG("[glsl] compiler %s up\n", GetGLSLCompilerVersion());
    if (g_verbose < 2) return;
    static char info[4096];
    {  // two known small shaders first: whether compiling works here at all, how long one takes, and that a loop (r600's jump tracker) gets through: in
        // Cemu it only does with tools/win/patch_cafeglsl_traps.py applied to the library
        const std::string plain = gen_passthrough_vs();
        const std::string with_loop = "#version 450\nlayout(location = 0) in vec4 aPos;\nlayout(binding = 0, std140) uniform VS { vec4 f[96]; vec4 uDraw; ivec4 uInt[4]; };\n"
                                      "void main() { vec4 p = aPos; for (int i = 0; i <= uInt[0].x; i++) p += f[i]; gl_Position = p; }\n";
        const struct {
            const char *name;
            const std::string *glsl;
        } steps[] = {{"plain", &plain}, {"loop", &with_loop}};
        for (const auto &s : steps) {
            LOG("[glsl] test %s: compiling\n", s.name);
            const OSTime t0 = OSGetSystemTime();
            GX2VertexShader *v = nullptr;
            info[0] = 0;
            watched = 0;
            busy_since = t0;
            const GX2ShaderMode m = CompileVertexShader(s.glsl->c_str(), GLSL_COMPILE_BLOCK, &v, info, sizeof info, GLSL_COMPILER_FLAG_NONE);
            busy_since = 0;
            LOG("[glsl] test %s: mode %d, %s, %u ms: %.200s\n", s.name, (int)m, v ? "shader made" : "no shader", (u32)OSTicksToMilliseconds(OSGetSystemTime() - t0), info);
            if (v) FreeVertexShader(v);
        }
    }
}

int compile_thread(int, void *) {
    bool started = false;
    static char info[4096];
    for (;;) {
        Job j;
        OSLockMutex(&mutex);
        const bool any = !jobs.empty();
        if (any) {
            j = std::move(jobs.front());
            jobs.pop_front();
        }
        OSUnlockMutex(&mutex);
        if (!any) {
            OSSleepTicks(OSMillisecondsToTicks(20));
            continue;
        }
        CompileDone d{j.vs, j.key, nullptr, nullptr, j.inputs};
        if (native_load(j, d)) {
            if (!j.cached) cache_append(j);
            LOG("[glsl] %s %016llx restored from native cache\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key);
            OSLockMutex(&mutex);
            done.push_back(d);
            OSUnlockMutex(&mutex);
            continue;
        }
        if (!started) { start_compiler(); started = true; }
        const OSTime t0 = OSGetSystemTime();
        info[0] = 0;
        LOG("[glsl] compiling %s %016llx, %u bytes of GLSL\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, (u32)j.glsl.size());
        watched = 0;
        busy_since = t0;
        const GX2ShaderMode mode = j.vs ? CompileVertexShader(j.glsl.c_str(), GLSL_COMPILE_BLOCK, &d.vs_shader, info, sizeof info, GLSL_COMPILER_FLAG_NONE)
                                        : CompilePixelShader(j.glsl.c_str(), GLSL_COMPILE_BLOCK, &d.ps_shader, info, sizeof info, GLSL_COMPILER_FLAG_NONE);
        busy_since = 0;
        const u32 ms = (u32)(OSTicksToMilliseconds(OSGetSystemTime() - t0));
        const auto failed = [&](const char *why) { LOG("[glsl] %s %016llx failed in %u ms: %.400s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, why); };
        if (mode == GLSL_SHADER_MODE_ERROR || (!d.vs_shader && !d.ps_shader)) {
            failed(info);
            shader_compile_discard(d);
            continue;
        }
        LOG("[glsl] %s %016llx compiled in %u ms%s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, ms, j.cached ? ", from the cache" : "");
        // A vertex shader that reads inputs and comes back with none, or as a 16-byte program, is hollow: the compiler hands those out for
        // what it's given in its first second or so after starting, and the same source compiles properly afterwards (seen in Cemu,
        // 2026-11-10). One is never installed. It's tried again for a couple of seconds, and then given up, so that a later miss can ask
        // afresh; the CPU shades those draws meanwhile. Fewer inputs than the code reads is no sign of it: the compiler rightly drops an
        // input that reaches no output, and the same shader comes back that way every time. Pixel shaders can instead come back as
        // one fully masked export (AC cold builds); alpha-NEVER draws are already skipped by raster.cpp, before requesting a shader.
        const bool hollow = j.vs ? d.vs_shader->size <= 16 || (inputs_read(j.glsl) && !d.vs_shader->attribVarCount)
                                 : latte::hollow_pixel_program(d.ps_shader->program, d.ps_shader->size);
        if (hollow) {
            shader_compile_discard(d);
            const bool again = ++j.tries < 8;
            LOG("[glsl] %s %016llx came back hollow: %s\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, again ? "trying again" : "given up");
            if (again) OSSleepTicks(OSMillisecondsToTicks(250));
            OSLockMutex(&mutex);
            if (again) jobs.push_front(std::move(j));
            else known.erase(j.key);
            OSUnlockMutex(&mutex);
            continue;
        }
        // what this compiler writes that a real GPU may not take, put right before anything draws with the program (latte_program.h)
        u32 &resources = j.vs ? d.vs_shader->regs.sq_pgm_resources_vs : d.ps_shader->regs.sq_pgm_resources_ps;
        u32 registers = resources & 0xFF;
        if (!fit_program(j.vs ? d.vs_shader->program : d.ps_shader->program, j.vs ? d.vs_shader->size : d.ps_shader->size, registers, shader_budget::kOrdinary)) {
            char why[160];
            snprintf(why, sizeof why, "invalid program/metadata or ordinary-register requirement exceeds %u (declared %u, %u bytes)",
                     shader_budget::kOrdinary, resources & 255, j.vs ? d.vs_shader->size : d.ps_shader->size);
            failed(why);
            shader_compile_discard(d);
            continue;
        }
        resources = (resources & ~0xFFu) | registers;
        LOG("[glsl] %s %016llx requires %u ordinary registers\n", j.vs ? "vertex" : "pixel", (unsigned long long)j.key, registers);
        native_save(j, d);  // store the corrected program, before install_compiled's Cemu-only export expansion
        if (!j.cached) cache_append(j);
        OSLockMutex(&mutex);
        done.push_back(d);
        OSUnlockMutex(&mutex);
    }
    return 0;
}
}  // namespace

bool shader_compile_on() { return compiler != nullptr; }

// On unless a file named "noglslcompile" is in the data folder. Its first long run on a console froze (2026-11-06): ContentRedirection's I/O thread sat on
// Aroma's heap lock with this thread as the owner, since this thread's log and cache writes went through that hook, outside fs_begin. Every file call now goes
// straight to IOS (fsa_direct.cpp, on consoles since 2026-11-07); this thread writes only its cache, inside fs_begin, and leaves the log to the card thread.
void shader_compile_init() {
    wiiu::fs_begin(0xC00A);
    const bool off = wiiu::debug_flag("noglslcompile");
    if (!off) {
        OSInitMutex(&mutex);
        cache_load();
    }
    wiiu::fs_end();
    if (off) {
        LOG("[glsl] off: noglslcompile\n");
        return;
    }
    auto *t = (OSThread *)memalign(16, sizeof(OSThread));
    void *stack = memalign(16, STACK);
    // Core 0, just below the card thread (20): a burst of compiles never holds up saves and the log, and only core 0's own threads (net,
    // watchdog, hid, card), which mostly sleep, come first. Not core 2 with the GPU thread: when shaders are missing that thread is at its
    // busiest, shading on the CPU what the missing shaders would, and the compiler below it got no time at all. One compile took 69 s there
    // against under 1 s here, at 7 fps against 57 (NSMB2 with every vertex shader from this compiler, in Cemu, 2026-11-11).
    if (!t || !stack || !OSCreateThread(t, compile_thread, 0, nullptr, (u8 *)stack + STACK, STACK, 21, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
        LOG("[glsl] no compiler thread\n");
        return;
    }
    OSSetThreadName(t, "glsl");
    compiler = t;
    OSResumeThread(t);
    LOG("[glsl] compiler thread ready\n");
}

// (GPU thread) a compile that has run for 8 s, 16 s, 24 s, 32 s: every thread's state on the log, and the log on the file at once, since the
// card thread that normally writes it shares core 0 with the compiler and may be what the compiler keeps from running (or the other way round)
void shader_compile_watch() {
    const OSTime since = busy_since;
    if (!since || watched >= 4 || OSTicksToSeconds(OSGetSystemTime() - since) < 8 * (watched + 1)) return;
    watched = watched + 1;
    LOG("[glsl] the compile in progress has run for %u s: compiler thread ran %u ms, state %x, pc %08x\n", (u32)OSTicksToSeconds(OSGetSystemTime() - since), (u32)(compiler->coreTimeConsumedNs / 1000000),
        (u32)compiler->state, compiler->context.srr0);
    wiiu::log_threads();  // memory only: this is the GPU thread, which never waits for the card
}

// A draw is waiting for this one, so it goes ahead of everything the cache queued at the start, and a shader the cache queued that a draw now
// asks for moves up to the front: after a start with a few hundred cached shaders a miss used to wait its turn behind all of them, its draws
// going through the fallback for as many seconds.
void shader_compile_request(bool vs, u64 key, std::string glsl, u32 inputs) {
    static std::unordered_set<u64> reported_vs;  // at most one request diagnostic per vertex key, even if a caller repeats it
    const char *disposition = "already known (no queued job)";
    OSLockMutex(&mutex);
    const bool report = vs && reported_vs.insert(key).second;
    if (known.insert(key).second) {
        jobs.push_front(Job{vs, false, key, inputs, std::move(glsl)});
        disposition = "queued";
    } else
        for (auto it = jobs.begin(); it != jobs.end(); ++it)
            if (it->key == key && it->vs == vs) {
                Job j = std::move(*it);
                jobs.erase(it);
                jobs.push_front(std::move(j));
                disposition = "promoted";
                break;
            }
    OSUnlockMutex(&mutex);
    if (report) LOG("[glsl] vertex %016llx request: %s\n", (unsigned long long)key, disposition);
}

void shader_compile_discard(CompileDone &d) {
    if (d.vs_shader) { if (d.from_gfd) { free(d.vs_shader->program); free(d.vs_shader); } else FreeVertexShader(d.vs_shader); }
    if (d.ps_shader) { if (d.from_gfd) { free(d.ps_shader->program); free(d.ps_shader); } else FreePixelShader(d.ps_shader); }
    d.vs_shader = nullptr;
    d.ps_shader = nullptr;
    d.from_gfd = false;
}

bool shader_compile_take(CompileDone &out) {
    if (!compiler) return false;
    OSLockMutex(&mutex);
    const bool any = !done.empty();
    if (any) {
        out = done.back();
        done.pop_back();
    }
    OSUnlockMutex(&mutex);
    return any;
}
}  // namespace gpu
