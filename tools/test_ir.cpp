// Actual IR handlers and wire format, with synthetic guest memory and scheduled callbacks.
#undef NDEBUG
#include <cassert>
#include <cstdlib>
#include <limits>
#include <vector>
#include "../third_party/3dsrecomp/runtime/services/misc.cpp"
// Exercise the console live-feed branch with the same synthetic host memory primitives as the IPC test.
#define __WIIU__
#include "../third_party/3dsrecomp/runtime/services/hid.cpp"
#undef __WIIU__

static std::vector<u8> memory(0x10000, 0xcc);
u8 *g_mem = memory.data();
int g_verbose;
u64 g_frame;
GameInfo g_game;
std::atomic<int> g_winshot_request;
std::atomic<u32> g_live_host;
static std::vector<std::string> live_texts, live_keys;
void frontend_live_text(const std::string &text) { live_texts.push_back(text); }
void frontend_live_key(const std::string &key) { live_keys.push_back(key); }
namespace onescreen { u32 host_mask(const char *) { assert(false); return 0; } }
namespace mem { u32 hle_alloc(u32, u32) { assert(false); return 0; } }
void rt_log(const char *, ...) {}
[[noreturn]] void rt_fatal(const char *, ...) { abort(); }
namespace kernel {
std::mutex kmutex;
static unsigned live_dumps;
void dump_threads() { ++live_dumps; }
void dump_memory(const char *) { assert(false); }
void schedule_async() {}
Handle add_handle(ObjPtr) { assert(false); return 0; }
static bool readable = true, writable = true;
static std::map<Handle, ObjPtr> objects;
static std::map<std::string, std::shared_ptr<Service>> services;
static std::map<Event *, unsigned> signals;
static std::vector<std::function<void()>> pending;
bool guest_readable_locked(u32 a, u32 n) { return readable && n && a >= 0x2000 && (u64)a + n <= 0xc000; }
bool guest_writable(u32 a, u32 n) { return writable && n && a >= 0x4000 && (u64)a + n <= 0xc000; }
u64 system_tick() { return 123456789; }
void Service::unimplemented(IPC &, bool) { assert(false); }
std::shared_ptr<Event> make_event(ResetType reset, const char *name) {
    auto event = std::make_shared<Event>(); event->reset = reset; event->name = name; return event;
}
int IPC::handles(int pos, std::initializer_list<ObjPtr> values, bool) {
    set(pos++, 0);
    for (const auto &o : values) { const Handle h = (Handle)objects.size() + 100; objects[h] = o; set(pos++, h); }
    return pos;
}
void Event::signal() { signaled = true; ++signals[this]; }
u32 IPC::static_buffer(int, u32 *) const { assert(false); return 0; }
void IPC::static_desc(int, int, u32) { assert(false); }
void add_timeout(u64, std::function<void()> callback) { pending.push_back(std::move(callback)); }
u64 now_ns() { return 1000; }
u32 alloc_linear(u32) { assert(false); return 0; }
ObjPtr get_object(Handle h) { auto it = objects.find(h); return it == objects.end() ? nullptr : it->second; }
void register_service(std::shared_ptr<Service> s) { services[s->name] = s; }
void register_apt() { assert(false); }
void register_cam() {}
void register_cfg() { assert(false); }
void register_dsp() { assert(false); }
void register_fs() { assert(false); }
void register_gsp() { assert(false); }
void register_ro() { assert(false); }
void register_y2r() {}
int mii_maker_place(u32) { assert(false); return -1; }
std::vector<u8> system_archive_file(u64, const std::string &) { assert(false); return {}; }
}
using namespace kernel;
static u32 call(Service &ir, u32 header, std::initializer_list<u32> words = {}) {
    memset(gptr(0x1000), 0, 0x100);
    w32(0x1000, header);
    u32 i = 1;
    for (u32 v : words) w32(0x1000 + 4 * i++, v);
    IPC ipc(nullptr, 0x1000, nullptr); ir.handle(ipc);
    return r32(0x1004);
}
static u32 init(Ir &ir, u32 size = 0x1000, u32 recvsize = 0x800, u32 count = 16, u32 send = 0x100, u32 sendcount = 2, u32 handle = 1) {
    return call(ir, 0x180182, {size, recvsize, count, send, sendcount, 4, 0, handle});
}
static u32 send_bytes(Ir &ir, std::initializer_list<u8> bytes, bool large = false) {
    u32 n = 0; for (u8 b : bytes) w8(0x2000 + n++, b);
    return call(ir, large ? 0xe0042 : 0xd0042, {n, large ? n << 4 | 10 : n << 14 | 2, 0x2000});
}
static void unchanged(const std::vector<u8> &before) {
    for (u32 a = 0; a < memory.size(); ++a) if (a < 0x1000 || a >= 0x1100) assert(memory[a] == before[a]);
}
static std::vector<u8> packet(const Ir &ir, u32 index) {
    const u32 list = ir.mem->backing + 0x20, data = list + ir.packets_max * 8;
    const u32 offset = r32(list + index * 8), n = r32(list + index * 8 + 4);
    std::vector<u8> bytes;
    for (u32 i = 0; i < n; ++i) bytes.push_back(r8(data + (offset + i) % ir.data_max));
    // Independent polynomial long division, including the received CRC: remainder must be zero.
    unsigned remainder = 0;
    for (u8 b : bytes) {
        remainder ^= unsigned(b) << 8;
        for (int j = 0; j < 8; ++j) {
            if (remainder & 0x8000) remainder ^= 0x107 << 7;
            remainder <<= 1;
        }
        remainder &= 0xffff;
    }
    assert(remainder == 0);
    return bytes;
}
static void drain_once() {
    auto callbacks = std::move(pending); pending.clear();
    for (auto &callback : callbacks) callback();
}
int main() {
    auto block = std::make_shared<SharedMem>(); block->backing = 0x4000; block->size = 0x1000; objects[1] = block;
    register_misc();
    auto ir = std::dynamic_pointer_cast<Ir>(services.at("ir:USER"));
    auto raw = std::dynamic_pointer_cast<Iru>(services.at("ir:u"));
    assert(ir && raw);
    // Reject the original out-of-block repro and bad arithmetic before touching any guest memory.
    for (auto args : {std::array<u32,5>{0x1000,0x2100,0x400,0,0}, {0x1000,0x800,0,0,0},
                     {0x1000,0x800,0xffffffff,0,0}, {0xffffffff,0x800,16,0,0},
                     {0x1000,0x800,16,0x100,0xffffffff}, {0x1000,0x800,16,0xffffffff,0},
                     {0x1000,0x80,16,0,0}, {0x920,0x800,16,0x100,2}}) {
        auto before = memory;
        assert(init(*ir,args[0],args[1],args[2],args[3],args[4]) == RES_INVALID_COMBINATION); unchanged(before);
    }
    for (u32 address : {0u,0xc000u,0xfffffff0u}) {
        block->backing = address; auto before = memory;
        assert(init(*ir) == RES_INVALID_ADDRESS); unchanged(before);
    }
    block->backing = 0x4000;
    for (bool reads : {false,true}) {
        readable=reads; writable=!reads; auto before=memory;
        assert(init(*ir)==RES_INVALID_ADDRESS); unchanged(before);
    }
    readable=writable=true;
    auto before=memory;
    assert(init(*ir,0x1000,0x800,16,0x100,2,99)==RES_INVALID_HANDLE); unchanged(before);
    assert(call(*ir,0x180182,{0x1000,0x800,16,0x100,2,4,0x20,1})==RES_INVALID_COMBINATION); unchanged(before);
    assert(init(*ir)==0);
    assert(r8(0x400e)==1 && r32(0x401c)==0 && !extra_pad_connected());
    assert(call(*ir,0x60040,{99})==0 && r8(0x4008)==1 && r8(0x4009)==2 && !ir->conn->signaled);
    assert(call(*ir,0x90000)==0 && r8(0x4008)==0 && r8(0x4009)==0 && !ir->conn->signaled);
    assert(send_bytes(*ir,{1,10,0})==Ir::NOT_CONNECTED && !ir->send->signaled);
    assert(call(*ir,0x60040,{1})==0 && extra_pad_connected() && r8(0x4008)==2 && r8(0x400a)==2 && r8(0x400c)==1);
    for (auto [command,event] : {std::pair{0xa,ir->recv}, {0xb,ir->send}, {0xc,ir->conn}}) {
        assert(call(*ir,command<<16)==0); assert(r32(0x1000)==(u32(command)<<16|0x42));
        assert(get_object(r32(0x100c))==event);
        const size_t count=objects.size();
        assert(call(*ir,command<<16|0x40,{0})==RES_INVALID_COMBINATION && objects.size()==count);
    }
    // Malformed sends never allocate, read inaccessible memory, signal, or change polling state.
    for (u32 command : {0xdu,0xeu}) {
        const u32 desc = command==0xd ? 6<<14|2 : 6<<4|10;
        for (u32 n : {0u,1u,2u,4u,5u,7u,0xffffffffu}) {
            auto old=memory; unsigned count=signals[ir->send.get()];
            assert(call(*ir,command<<16|0x42,{n,desc,0x2000})==RES_INVALID_COMBINATION);
            unchanged(old); assert(signals[ir->send.get()]==count && ir->period_ms==0);
        }
        for (u32 address : {0u,0xbffeu,0xfffffffeu}) {
            auto old=memory; assert(call(*ir,command<<16|0x42,{3,desc,address})==RES_INVALID_ADDRESS); unchanged(old);
        }
        for (u32 bad : {0u,8u,12u,15u}) {
            auto old=memory; assert(call(*ir,command<<16|0x42,{3,bad,0x2000})==RES_INVALID_COMBINATION); unchanged(old);
        }
        assert(call(*ir,command<<16|0x41,{3,desc,0x2000})==RES_INVALID_COMBINATION);
        const u32 small = command==0xd ? 2<<14|2 : 2<<4|10;
        assert(call(*ir,command<<16|0x42,{3,small,0x2000})==RES_INVALID_COMBINATION);
    }
    assert(send_bytes(*ir,{1,0,0,0,0,0})==RES_INVALID_COMBINATION);
    readable=false;
    assert(send_bytes(*ir,{1,10,0})==RES_INVALID_ADDRESS && ir->period_ms==0 && !ir->send->signaled);
    readable=true;
    assert(send_bytes(*ir,{2,0,0})==RES_INVALID_COMBINATION);
    assert(send_bytes(*ir,{2,0,1,0,16,0})==RES_INVALID_COMBINATION);
    assert(send_bytes(*ir,{2,0,64,0,16,0})==RES_INVALID_COMBINATION);
    // Full calibration uses extended framing. These are protocol data, not game bytes.
    assert(send_bytes(*ir,{2,0,0,0,64,0},true)==0);
    assert(r32(0x1000)==0xe0042 && r32(0x1008)==(6<<4|10) && r32(0x100c)==0x2000);
    auto cal=packet(*ir,0); assert(cal.size()==74 && cal[0]==0xa5 && cal[2]==0x40 && cal[3]==69 && cal[4]==0x11);
    assert(cal[9]==0 && cal[11]==8 && cal[12]==0x80 && cal[24]==0xf5 && cal[40]==0x65 && cal[72]==0x65);
    // Oversized releases preserve both the queue and event. Ring tampering cannot steer a write.
    before=memory; const auto receive_count=signals[ir->recv.get()];
    assert(call(*ir,0x190040,{2})==Ir::NO_DATA); unchanged(before); assert(signals[ir->recv.get()]==receive_count);
    for (u32 address : {0x4010u,0x4014u,0x4018u,0x401cu,0x4020u,0x4024u}) {
        const auto old=r32(address); w32(address,0xffffffff); before=memory;
        ir->report(); unchanged(before); assert(signals[ir->recv.get()]==receive_count);
        assert(call(*ir,0x190040,{1})==RES_INVALID_COMBINATION); unchanged(before);
        w32(address,old);
    }
    assert(call(*ir,0x190040,{1})==0 && r32(0x4018)==0 && r32(0x401c)==0);
    // Endpoint clamping, active-low buttons, and malformed host float inputs.
    hid_extra_pad(-2,2,true,false,true); ir->report();
    auto p=packet(*ir,ir->first); assert(p.size()==10 && p[2]==6 && p[3]==0x10);
    u32 xy=p[4]|p[5]<<8|p[6]<<16; assert((xy&0xfff)==1 && (xy>>12)==0xfff && p[7]==0x5f);
    assert(call(*ir,0x190040,{1})==0);
    hid_extra_pad(std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),false,false,false);
    ir->report(); p=packet(*ir,ir->first); xy=p[4]|p[5]<<8|p[6]<<16;
    assert((xy&0xfff)==0x800 && (xy>>12)==0x800 && p[7]==0xff);
    assert(call(*ir,0x190040,{1})==0);
    // Existing live commands reach real CPP packets and persist through intervening physical refreshes.
    live_feed("ZR 6");
    g_frame=0; hid_update(0,0,0,false,0,0);
    for (int i=0;i<3;++i) {
        hid_extra_pad(0,0,false,false,false);
        ir->report(); p=packet(*ir,ir->first); assert(p[7]==0xbf);
        assert(call(*ir,0x190040,{1})==0);
    }
    g_frame=6; hid_update(0,0,0,false,0,0);
    hid_extra_pad(0.25f,0,false,false,true);
    ir->report(); p=packet(*ir,ir->first); assert(p[7]==0x7f);
    assert(call(*ir,0x190040,{1})==0);
    live_feed("ZL+R 3"); g_frame=10; hid_update(0,0,0,false,0,0);
    hid_extra_pad(0,0,false,false,false);
    ir->report(); p=packet(*ir,ir->first); assert(p[7]==0x5f);
    assert(call(*ir,0x190040,{1})==0);
    g_frame=13; hid_update(0,0,0,false,0,0);
    ir->report(); p=packet(*ir,ir->first); assert(p[7]==0xff);
    assert(call(*ir,0x190040,{1})==0);
    // A tiny ring wraps data and descriptors, drops full reports without signaling, then resumes after release.
    assert(init(*ir,0x1000,8*3+25,3,0,0)==0); assert(call(*ir,0x60040,{1})==0);
    ir->report(); ir->report(); auto count=signals[ir->recv.get()]; before=memory;
    ir->report(); unchanged(before); assert(signals[ir->recv.get()]==count);
    assert(call(*ir,0x190040,{1})==0); ir->report(); assert(packet(*ir,2).size()==10 && r32(0x4030)==20);
    assert(call(*ir,0x190040,{1})==0); ir->report(); assert(packet(*ir,0).size()==10 && r32(0x4020)==5);
    assert(r32(0x401c)==0);
    // Polling lifetime: disconnect, reinit, remap/permission loss, and destruction.
    assert(send_bytes(*ir,{1,10,0})==0 && pending.size()==1); drain_once(); assert(pending.size()==1);
    assert(call(*ir,0x90000)==0 && !extra_pad_connected() && !ir->period_ms); before=memory;
    drain_once(); unchanged(before); assert(pending.empty());
    assert(call(*ir,0x60040,{1})==0); assert(send_bytes(*ir,{1,10,0})==0);
    before=memory; const auto generation=ir->generation;
    assert(init(*ir,0x1000,0x2100,0x400,0,0)==RES_INVALID_COMBINATION);
    unchanged(before); assert(ir->connected && ir->period_ms==10 && ir->generation==generation);
    assert(init(*ir)==0 && !extra_pad_connected() && !ir->period_ms); before=memory;
    drain_once(); unchanged(before); assert(pending.empty());
    assert(call(*ir,0x60040,{1})==0); assert(send_bytes(*ir,{1,10,0})==0);
    writable=false; before=memory; drain_once(); unchanged(before); assert(pending.empty()); writable=true;
    assert(send_bytes(*ir,{1,10,0})==0); block->backing=0xffffff00; before=memory;
    drain_once(); unchanged(before); block->backing=0x4000;
    // A second service's finalization must not disable a connected first service.
    auto other=std::make_shared<Ir>("ir:USER"); assert(call(*other,0x20000)==0 && extra_pad_connected());
    for (auto [command,event] : {std::pair{0xd,raw->send}, {0xe,raw->recv}}) {
        assert(call(*raw,command<<16)==0 && r32(0x1000)==(u32(command)<<16|0x42));
        assert(get_object(r32(0x100c))==event && !event->signaled && extra_pad_connected());
    }
    assert(call(*raw,0x60000)==0 && extra_pad_connected());
    assert(call(*raw,0x90040,{4})==0 && extra_pad_connected());
    assert(call(*raw,0x20000)==0 && extra_pad_connected());
    assert(send_bytes(*ir,{1,10,0})==0); services.erase("ir:USER"); ir.reset(); assert(!extra_pad_connected()); before=memory;
    drain_once(); unchanged(before); assert(pending.empty());
    // The complete test leaves all guard areas outside the shared block untouched.
    for(u32 a=0x3000;a<0x4000;++a) assert(memory[a]==0xcc);
    for(u32 a=0x5000;a<memory.size();++a) assert(memory[a]==0xcc);
    assert(live_keys.empty() && live_texts.empty() && live_dumps==0 && g_winshot_request==0 && !live.f);
    puts("IR: actual IPC, bounded shared rings, CRC/calibration, malformed sends, events, reinit and weak callback lifetime passed");
}
