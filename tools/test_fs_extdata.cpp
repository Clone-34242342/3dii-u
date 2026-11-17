// Exercise the real filesystem service in an empty temporary directory, without game files.
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include "../third_party/3dsrecomp/runtime/services/fs.cpp"

u8 *g_mem;
int g_verbose;
u64 g_frame;
GameInfo g_game;
std::string g_save_base;
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
void cloud_save_written() {}
namespace stacks { void save_written(const std::string &) {} }
namespace kernel {
u64 g_missing_system_archive;
const std::vector<std::array<u8, 0x60>> &console_miis(int *) { assert(false); static const std::vector<std::array<u8, 0x60>> empty; return empty; }
u16 mii_crc16(const u8 *, size_t) { assert(false); return 0; }
void run_blocked(Lock &, const std::function<void()> &) { assert(false); }
void sleep_current(u64, Lock &) { assert(false); }
void register_service(std::shared_ptr<Service>) {}
void Service::unimplemented(IPC &, bool) { assert(false && "unexpected unimplemented filesystem command"); }
Handle new_session(std::shared_ptr<Service>) { assert(false); return 0; }
int IPC::handles(int, std::initializer_list<ObjPtr>, bool) { assert(false); return 0; }
void IPC::static_desc(int, int, u32) { assert(false); }
u32 IPC::static_buffer(int, u32 *) const { assert(false); return 0; }
}

static void call(kernel::FsUser &service, u32 header, std::initializer_list<u32> words) {
    memset(gptr(0x1000), 0, 256);
    w32(0x1000, header);
    int i = 1;
    for (u32 word : words) w32(0x1000 + 4 * i++, word);
    kernel::IPC ipc(nullptr, 0x1000, nullptr);
    service.handle(ipc);
    assert(r32(0x1004) == 0);
}

static void format(const std::string &dir, std::initializer_list<u32> expected) {
    assert(fsys::is_directory(dir));
    std::ifstream input(dir + ".format", std::ios::binary);
    assert(input);
    for (u32 value : expected) {
        u32 actual = 0;
        input.read((char *)&actual, sizeof actual);
        assert(input && actual == value);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const fsys::path root = fsys::absolute(argv[1]);
    assert(fsys::is_directory(root) && fsys::is_empty(root));
    g_save_base = root.string();
    g_game.program_id = 0x0004000000001234;
    g_mem = (u8 *)calloc(1, 0x200000);
    assert(g_mem);
    kernel::FsUser service("fs:USER");
    const std::string legacy = kernel::save_root() + "/extdata/0000000000000042";
    const std::string modern = kernel::save_root() + "/extdata/1234567800000042";
    const u32 descriptor = (0x40 << 4) | 0xA, pointer = 0x6000;
    call(service, 0x08300182, {1, 0x42, 0, 0x40, 2, 3, descriptor, pointer});
    format(legacy, {~0u, 2, 3, 0});
    assert(r32(0x1000) == 0x08300042 && r32(0x1008) == descriptor && r32(0x100c) == pointer);
    call(service, 0x08510242, {1, 0x42, 0x12345678, 0, 4, 5, 0x1000, 0, 0x40, descriptor, pointer});
    format(modern, {0, 4, 5, 0});
    // The legacy request has no high ID: a stale third word cannot select another archive.
    call(service, 0x08350080, {1, 0x42, 0x12345678});
    assert(!fsys::exists(legacy) && !fsys::exists(legacy + ".format"));
    assert(fsys::is_directory(modern));
    call(service, 0x08520100, {1, 0x42, 0x12345678, 0});
    assert(!fsys::exists(modern) && !fsys::exists(modern + ".format"));
    free(g_mem);
    puts("filesystem extdata: legacy/modern creates, quotas, mapped reply and isolated deletes passed");
}
