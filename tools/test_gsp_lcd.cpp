// Exercise the production GSP IPC handler with LCD registers in synthetic guest memory.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include "../third_party/3dsrecomp/runtime/service.cpp"
#include "../third_party/3dsrecomp/runtime/services/gsp.cpp"

static u8 memory[0x8000];
u8 *g_mem = memory;
int g_verbose;
GameInfo g_game;
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { std::abort(); }
namespace mem {
u32 paddr_to_vaddr(u32 addr) { return addr; }
u32 vaddr_to_paddr(u32 addr) { return addr; }
u32 hle_alloc(u32, u32) { assert(false); return 0; }
}
namespace kernel {
std::mutex kmutex;
KStats kstats;
Handle add_handle(ObjPtr) { assert(false); return 0; }
ObjPtr get_object(Handle) { assert(false); return {}; }
void frontend_wake() { assert(false); }
void register_all_services() { assert(false); }
void run_when_quiescent(std::function<void()>) { assert(false); }
void schedule_async() { assert(false); }
void Event::signal() { assert(false); }
void add_timeout(u64, std::function<void()>) { assert(false); }
u64 now_ns() { assert(false); return 0; }
bool guest_readable_locked(u32 addr, u32 size) { return addr >= 0x2000 && u64(addr) + size <= sizeof memory; }
bool guest_writable(u32 addr, u32 size) { return guest_readable_locked(addr, size); }
}
namespace gpu {
bool save_state(const char *) { assert(false); return false; }
void dma_written(u32, u32) { assert(false); }
void invalidate_memory(u32, u32) { assert(false); }
void texture_copy(u32, u32, u32, u32, u32, u32) { assert(false); }
void service_written(u32, u32) { assert(false); }
void memory_fill(u32, u32, u32, u32) { assert(false); }
void display_transfer(u32, u32, u32, u32, u32) { assert(false); }
void process_command_list(u32, u32) { assert(false); }
}

using namespace kernel;
constexpr u32 command = 0x1000, input = 0x2000, masks = 0x3000, output = 0x4000;
static u32 call(GspGpu &service, Thread &thread, u32 head, std::initializer_list<u32> args) {
    memset(gptr(command), 0, 0x100);
    w32(command, head);
    int i = 1;
    for (u32 word : args) w32(command + 4 * i++, word);
    IPC ipc(&thread, command, nullptr);
    service.handle(ipc);
    return r32(command + 4);
}

int main() {
    GspGpu service("gsp::Gpu");
    Thread thread{};
    thread.tls = 0x1200;
    const u32 desc = (4 << 14) | 2;
    const u32 top_format = 0x400470, select = 0x400478;
    w32(thread.tls + 0x180, desc);
    w32(thread.tls + 0x184, output);

    w32(input, 0x141);
    assert(call(service, thread, 0x00010082, {top_format, 4, desc, input}) == 0);
    w32(input, 0x20);
    w8(masks, 0x20);  // A short mask affects only the bits it contains.
    assert(call(service, thread, 0x00020084, {top_format, 4, desc, input, (1 << 14) | 0x402, masks}) == 0);
    assert(call(service, thread, 0x00040080, {top_format, 4}) == 0);
    assert(r32(output) == 0x161 && gsp_framebuffer(0).format == 0x161);

    w32(input, 0x20200000);
    w32(input + 4, 0x20400000);
    assert(call(service, thread, 0x00010082, {0x400468, 8, (8 << 14) | 2, input}) == 0);
    w32(input, 0x101);
    assert(call(service, thread, 0x00010082, {select, 4, desc, input}) == 0);
    assert(gsp_framebuffer(0).addr_left == 0x20400000);
    w32(input, 0x100);
    w32(masks, 0x100);
    assert(call(service, thread, 0x00020084, {select, 4, desc, input, desc | 0x400, masks}) == 0);
    assert(gsp_framebuffer(0).addr_left == 0x20400000);
    assert(call(service, thread, 0x00040080, {select, 4}) == 0 && r32(output) == 0x101);
    assert(call(service, thread, 0x00040080, {top_format, 8}) == RES_INVALID_COMBINATION);
    assert(call(service, thread, 0x00020084, {select, 4, desc, input, desc | 0x400, 0x8000}) == RES_INVALID_ADDRESS);

    puts("GSP LCD register masking and readback passed.");
}
