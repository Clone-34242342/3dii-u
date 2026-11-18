// Standalone frontend only: no PICA/GX2 shader, state, vertex or pixel computation lives here.
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/filesystem.h>
#include <coreinit/memdefaultheap.h>
#include <gx2/event.h>
#include <whb/proc.h>
#include <malloc.h>
#include <cstdarg>
#include <cstdio>
#include "loader.h"
#include "settings.h"
#include "services/services.h"
#include "wiiu.h"

u8 *g_mem;
u64 g_frame;
GameInfo g_game;
Settings g_settings;
std::string g_save_base;
int g_verbose;
static FILE *log_file;
alignas(64) static FSClient save_client;
alignas(64) static FSCmdBlock save_block;
static FSStatus (*save_flush)(FSClient *, FSCmdBlock *, u8, FSErrorFlag);
void replay_backend_init();

void rt_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (log_file) { vfprintf(log_file, format, args); fflush(log_file); }
    va_end(args);
}
[[noreturn]] void rt_fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (log_file) { vfprintf(log_file, format, args); fflush(log_file); }
    va_end(args);
    abort();
}

namespace mem {
void init() {
    if (g_mem) return;
    g_mem = (u8 *)MEMAllocFromDefaultHeapEx(0x20010000, 0x1000);
    if (!g_mem) FATAL("REPLAY FAIL: guest memory allocation\n");
    memset(g_mem + LINEAR_BASE, 0, LINEAR_SIZE);
    memset(g_mem + VRAM_BASE, 0, VRAM_SIZE);
}
u32 paddr_to_vaddr(u32 pa) {
    if (pa >= FCRAM_PADDR && pa < FCRAM_PADDR + LINEAR_SIZE) return pa - FCRAM_PADDR + LINEAR_BASE;
    if (pa >= VRAM_PADDR && pa < VRAM_PADDR + VRAM_SIZE) return pa - VRAM_PADDR + VRAM_BASE;
    return 0;
}
u8 *paddr_ptr(u32 pa) { const u32 va = paddr_to_vaddr(pa); return va ? g_mem + va : nullptr; }
u32 vaddr_to_paddr(u32 va) {
    if (va >= LINEAR_BASE && va < LINEAR_BASE + LINEAR_SIZE) return va - LINEAR_BASE + FCRAM_PADDR;
    if (va >= VRAM_BASE && va < VRAM_BASE + VRAM_SIZE) return va - VRAM_BASE + VRAM_PADDR;
    return 0;
}
// The standalone fixture has no guest program heap to lend. These are the production
// core.cpp allocator's ordinary-heap path, with the same alignment and ownership.
void *host_alloc(u32 align, u32 size) { return memalign(align, size); }
void host_free(void *pointer) { free(pointer); }
}

namespace wiiu {
volatile u32 last_vs, last_ps;
volatile bool settings_due;
const std::string &data_dir() { static const std::string path = "fs:/vol/save/common"; return path; }
void fs_begin(u32) {}
void fs_end() {}
void romfs_begin() {}
void romfs_end() {}
void note(const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (log_file) { vfprintf(log_file, format, args); fprintf(log_file, "\n"); fflush(log_file); }
    va_end(args);
}
bool debug_flag(const char *name) {
    // Ahead-of-time fixture pack only; a missing native shader must fail the replay.
    if (!strcmp(name, "noglslcompile") || !strcmp(name, "cemu") || !strcmp(name, "nolaps")) return true;
    if (strcmp(name, "default480")) return false;
    FILE *file = fopen((data_dir() + "/default480").c_str(), "rb");
    if (!file) return false;
    fclose(file);
    return true;
}
void log_threads() {}
}

namespace kernel {
KStats kstats;
int g_system_language = 1;
FramebufferInfo gsp_framebuffer(int screen) {
    return {mem::LINEAR_BASE + 0x400000u + (screen ? 0x100000u : 0u), 0, 240 * 4, 0, true};
}
u32 gsp_swaps(int) { return 0; }
}

void replay_target_init() {
    WHBProcInit();
    OSDynLoad_Module module;
    if (OSDynLoad_Acquire("nn_save.rpl", &module) != OS_DYNLOAD_OK) abort();
    int (*init)() = nullptr, (*init_dir)(u8) = nullptr;
    OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "SAVEInit", (void **)&init);
    OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "SAVEInitSaveDir", (void **)&init_dir);
    OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, "SAVEFlushQuota", (void **)&save_flush);
    if (!init || !init_dir || !save_flush) abort();
    FSInit();
    if (FSAddClient(&save_client, FS_ERROR_FLAG_ALL) != FS_STATUS_OK) abort();
    FSInitCmdBlock(&save_block);
    if (init() || init_dir(0xff)) abort();
    log_file = fopen((wiiu::data_dir() + "/log.txt").c_str(), "wb");
    if (!log_file) abort();
    g_settings.resolution = 480;
    g_settings.msaa = 1;
    g_settings.aniso = 1;
    replay_backend_init();
}

void replay_target_finish() {
    GX2DrawDone();
    if (log_file) { fclose(log_file); log_file = nullptr; }
    if (save_flush(&save_client, &save_block, 0xff, FS_ERROR_FLAG_ALL) < 0) abort();
    WHBProcShutdown();
}
