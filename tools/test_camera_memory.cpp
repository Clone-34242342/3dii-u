// Include the real map traversal so DMA validation tests use the kernel's region permissions.
#include "kernel.cpp"
#include <cassert>

GameInfo g_game;
u8 *g_mem;
int g_verbose;
u64 rt_lookups, rt_lookup_misses;
void rt_log(const char *, ...) { abort(); }
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
void run_guest(CPU *) { abort(); }
std::string read_cstr(u32, size_t) { abort(); }
namespace kernel {
u64 g_missing_system_archive;
void gsp_cache_op(u32, u32, bool) { abort(); }
u32 service_request(std::shared_ptr<Session>, Thread *, Lock &) { abort(); }
std::shared_ptr<Service> find_port(const std::string &) { abort(); }
const char *system_archive_name(u64) { abort(); }
}

int main() {
    using namespace kernel;
    g_game.text_addr = 0x100000;
    g_game.data_addr = 0x120000;
    g_game.data_size = 0x1000;
    g_game.bss_size = 0x1000;
    assert(guest_writable(0x120000, 0x2000));
    assert(!guest_writable(0x11ffff, 2));
    assert(!guest_writable(0x100000, 4));
    assert(!guest_writable(0x122000, 1));
    add_region(0x8000000, 0x1000, 3, 5);
    add_region(0x8001000, 0x1000, 3, 5);
    add_region(0x8002000, 0x1000, 1, 5);
    add_region(0x8004000, 0x1000, 3, 5);
    assert(guest_writable(0x8000001, 0x1fff));
    assert(!guest_writable(0x8000001, 0x2000));
    assert(!guest_writable(0x8002000, 4));
    assert(!guest_writable(0x8003000, 4));
    assert(!guest_writable(0x8004000, 0));
    assert(!guest_writable(0xfffffff0, 0x100));
    assert(!guest_writable(0, 0xffffffff));
    puts("camera DMA: actual kernel write permission, adjacent maps, gaps, program data and overflow passed");
}
