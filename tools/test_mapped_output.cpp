// Exercise the real UDS/SSL handlers; kernel mapping traversal has its own permission tests.
#include <cassert>
#include <cstdlib>
#include <vector>
#include "../third_party/3dsrecomp/runtime/services/misc.cpp"

static std::vector<u8> memory(0x10000, 0xcc);
u8 *g_mem = memory.data();
int g_verbose;
u64 g_frame;
GameInfo g_game;
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
namespace kernel {
bool guest_writable(u32 addr, u32 size) {
    return size && addr >= 0x4000 && (u64)addr + size <= 0x5000;
}
bool guest_readable_locked(u32 addr, u32 size) { return guest_writable(addr, size); }
u64 system_tick() { return 123456789; }
void Service::unimplemented(IPC &, bool) { assert(false); }
std::shared_ptr<Event> make_event(ResetType reset, const char *name) {
    auto event = std::make_shared<Event>(); event->reset = reset; event->name = name; return event;
}
int IPC::handles(int, std::initializer_list<ObjPtr>, bool) { assert(false); return 0; }
void Event::signal() { assert(false); }
u32 IPC::static_buffer(int, u32 *) const { assert(false); return 0; }
void IPC::static_desc(int, int, u32) { assert(false); }
void add_timeout(u64, std::function<void()>) { assert(false); }
u64 now_ns() { assert(false); return 0; }
u32 alloc_linear(u32) { assert(false); return 0; }
ObjPtr get_object(Handle) { assert(false); return {}; }
void register_service(std::shared_ptr<Service>) { assert(false); }
void register_apt() { assert(false); }
void register_cam() { assert(false); }
void register_cfg() { assert(false); }
void register_dsp() { assert(false); }
void register_fs() { assert(false); }
void register_gsp() { assert(false); }
void register_hid() { assert(false); }
void register_ro() { assert(false); }
void register_y2r() { assert(false); }
int mii_maker_place(u32) { assert(false); return -1; }
std::vector<u8> system_archive_file(u64, const std::string &) { assert(false); return {}; }
}

static u32 call(kernel::Service &service, bool uds, u32 requested, u32 capacity, u32 rights, u32 address) {
    memset(gptr(0x1000), 0, 0x100);
    w32(0x1000, uds ? 0x000f0404 : 0x00110042);
    w32(0x1004, requested);
    const int pos = uds ? 19 : 2;
    w32(0x1000 + pos * 4, capacity << 4 | rights);
    w32(0x1004 + pos * 4, address);
    kernel::IPC ipc(nullptr, 0x1000, nullptr);
    service.handle(ipc);
    return r32(0x1004);
}

int main() {
    kernel::Uds uds("nwm::UDS");
    kernel::Ssl ssl("ssl:C");
    for (bool beacon : {true, false}) {
        kernel::Service &service = beacon ? static_cast<kernel::Service &>(uds) : static_cast<kernel::Service &>(ssl);
        const u32 count = beacon ? 12 : 32;
        auto rejected = [&](u32 size, u32 capacity, u32 rights, u32 address, u32 error) {
            std::fill(memory.begin(), memory.end(), 0xcc);
            assert(call(service, beacon, size, capacity, rights, address) == error);
            assert(r32(0x1000) == ((beacon ? 0xfu : 0x11u) << 16 | 0x40));
            for (u32 at = 0; at < memory.size(); at++)
                if (at < 0x1000 || at >= 0x1100) assert(memory[at] == 0xcc);
        };
        for (u32 size = 0; beacon && size < 12; size++)
            rejected(size, 12, 12, 0x4000, kernel::RES_INVALID_COMBINATION);
        rejected(count, count - 1, 12, 0x4000, kernel::RES_INVALID_COMBINATION);
        for (u32 rights : {0u, 2u, 8u, 10u, 13u, 15u})
            rejected(count, count, rights, 0x4000, kernel::RES_INVALID_COMBINATION);
        for (u32 addr : {0u, 0x6000u, 0x4ffcu, 0x8000u, 0xfffffff8u})
            rejected(count, count, 12, addr, kernel::RES_INVALID_ADDRESS);
        rejected(0x10000000, 0x0fffffff, 12, 0x4000, kernel::RES_INVALID_COMBINATION);
        for (u32 rights : {12u, 14u}) {
            std::fill(memory.begin(), memory.end(), 0xcc);
            assert(call(service, beacon, count, count + 16, rights, 0x4001) == 0);
            assert(r32(0x1000) == ((beacon ? 0xfu : 0x11u) << 16 | 0x42));
            assert(r32(0x1008) == ((count + 16) << 4 | rights) && r32(0x100c) == 0x4001);
            assert(memory[0x4000] == 0xcc);
            for (u32 i = 0x4001 + count; i < 0x4100; i++) assert(memory[i] == 0xcc);
            if (beacon) {
                assert(r32(0x4001) == 12 && r32(0x4005) == 12 && r32(0x4009) == 0);
            } else {
                assert(std::any_of(memory.begin() + 0x4001, memory.begin() + 0x4001 + count,
                                   [](u8 byte) { return byte != 0xcc; }));
            }
        }
    }
    std::fill(memory.begin(), memory.end(), 0xcc);
    assert(call(uds, true, 64, 64, 12, 0x4001) == 0);
    assert(r32(0x4001) == 64 && r32(0x4005) == 12 && r32(0x4009) == 0);
    for (u32 at = 0x400d; at < 0x4100; at++) assert(memory[at] == 0xcc);
    std::fill(memory.begin(), memory.end(), 0xcc);
    // The header fits, but the promised output range crosses into non-writable memory.
    assert(call(uds, true, 64, 64, 12, 0x4fe8) == kernel::RES_INVALID_ADDRESS);
    for (u32 at = 0x4000; at < 0x5100; at++) assert(memory[at] == 0xcc);
    assert(call(ssl, false, 0, 0, 12, 0) == 0);
    puts("mapped output: real UDS/SSL IPC rejects bad bounds/permissions without writes; valid replies preserve descriptors and canaries");
}
