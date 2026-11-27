#include "service.h"
#include "services/services.h"
#include "services/camera_frame.h"
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>

using namespace camera_frame;
static std::vector<u8> memory(3 * 1024 * 1024, 0xcc), source(HOST_SIZE, 0xed);
u8 *g_mem = memory.data();
int g_verbose = 0;
void rt_log(const char *, ...) {}
static u64 clock_ns = 0, host_serial = 0;
static bool available = true, fresh = true, opened = false, writable = true, published = false;
static u32 receive_address = 0x1d0001, receive_capacity = 40;
static int starts = 0, stops = 0, writes = 0;
static u32 last_write_addr, last_write_size;
static bool last_write_exact;
static std::shared_ptr<kernel::Service> service;
static std::vector<kernel::ObjPtr> handles;
static std::multimap<u64, std::function<void()>> timers;
static std::vector<u64> vsync_times;
static std::mutex ipc_mutex;
static kernel::Lock ipc_lock(ipc_mutex, std::defer_lock);

bool cam_host_start() { starts++; return opened = available; }
void cam_host_stop() { stops++; opened = false; host_serial = 0; }
bool cam_host_frame(u8 *out, u32 size) {
    assert(opened && size >= HOST_SIZE);
    const u64 serial = clock_ns / 33333334;
    if (!fresh || serial <= host_serial) return false;
    host_serial = serial;
    memcpy(out, source.data(), HOST_SIZE);
    return true;
}

namespace kernel {
void register_service(std::shared_ptr<Service> s) { service = std::move(s); }
void Service::unimplemented(IPC &, bool) { assert(false); }
void run_blocked(Lock &, const std::function<void()> &f) { f(); }
u64 now_ns() { return clock_ns; }
void add_timeout(u64 deadline, std::function<void()> f) { assert(deadline > clock_ns); timers.emplace(deadline, std::move(f)); }
bool guest_writable(u32 addr, u32 size) { return writable && size && addr >= 0x1000 && (u64)addr + size <= memory.size(); }
void gsp_cpu_wrote(u32 addr, u32 size) {
    assert(guest_writable(addr, size));
    last_write_addr = addr;
    last_write_size = size;
    last_write_exact = true;
    writes++;
    published = true;
}
void gsp_cache_op(u32 addr, u32 size, bool cpu_will_read) {
    assert(!cpu_will_read);
    gsp_cpu_wrote(addr, size);
    last_write_exact = false;
}
std::shared_ptr<Event> make_event(ResetType reset, const char *name) {
    auto event = std::make_shared<Event>(); event->reset = reset; event->name = name; return event;
}
void Event::signal() {
    if (name == "cam received") { assert(published); published = false; }
    if (name == "cam vsync") vsync_times.push_back(clock_ns);
    signaled = true;
}
int IPC::handles(int pos, std::initializer_list<ObjPtr> objs, bool) {
    set(pos++, 0);
    for (const auto &o : objs) { ::handles.push_back(o); set(pos++, ::handles.size()); }
    return pos;
}
u32 IPC::static_buffer(int index, u32 *size) const {
    assert(index == 0);
    if (size) *size = receive_capacity;
    return receive_address;
}
void IPC::static_desc(int pos, int index, u32 size) {
    assert(index == 0);
    set(pos, size << 14 | 2);
    set(pos + 1, receive_address);
}
}

static u32 call(u32 command, std::initializer_list<u32> args = {}) {
    for (u32 i = 0; i < 64; i++) w32(0x100 + i * 4, 0);
    w32(0x100, command << 16 | static_cast<u32>(args.size()) << 6);
    u32 i = 1;
    for (u32 arg : args) w32(0x100 + i++ * 4, arg);
    kernel::IPC ipc(nullptr, 0x100, &ipc_lock);
    service->handle(ipc);
    return r32(0x104);
}
static std::shared_ptr<kernel::Event> event_call(u32 command, std::initializer_list<u32> args) {
    assert(call(command, args) == 0);
    return std::dynamic_pointer_cast<kernel::Event>(handles.back());
}
static void advance(u64 delta) {
    const u64 end = clock_ns + delta;
    int count = 0;
    while (!timers.empty() && timers.begin()->first <= end) {
        assert(++count < 1000);
        auto timer = timers.extract(timers.begin());
        clock_ns = timer.key();
        timer.mapped()();
    }
    clock_ns = end;
}
static u8 luma(u32 x, u32 y) { return 16 + (x * 3 + y * 5) % 220; }
static void pattern() {
    std::fill(source.begin(), source.end(), 0xed);
    for (u32 y = 0; y < HOST_HEIGHT; y++) for (u32 x = 0; x < HOST_WIDTH; x++) source[y * HOST_PITCH + x] = luma(x, y);
    for (u32 y = 0; y < HOST_HEIGHT / 2; y++) for (u32 x = 0; x < HOST_WIDTH; x += 2) {
        source[HOST_Y_SIZE + y * HOST_PITCH + x] = (x * 7 + y * 3) % 255;
        source[HOST_Y_SIZE + y * HOST_PITCH + x + 1] = (x * 5 + y * 11 + 71) % 255;
    }
}
static void color(u8 y, u8 u, u8 v) {
    for (u32 row = 0; row < HOST_HEIGHT; row++) std::fill_n(source.data() + row * HOST_PITCH, HOST_WIDTH, y);
    for (u32 row = 0; row < HOST_HEIGHT / 2; row++) for (u32 x = 0; x < HOST_WIDTH; x += 2) {
        source[HOST_Y_SIZE + row * HOST_PITCH + x] = u;
        source[HOST_Y_SIZE + row * HOST_PITCH + x + 1] = v;
    }
}

// Frozen pre-optimization converter: keep its sampling math independent of the optimized path.
static bool frozen_convert(const uint8_t *src, uint32_t source_size, const Context &c, const Trim &t,
                    uint8_t *dst, uint32_t capacity) {
    const uint32_t size = output_size(c, t);
    if (!src || !dst || source_size < HOST_SIZE || !size || !capacity) return false;
    uint32_t remaining = std::min(size, capacity);
    auto put = [&](uint8_t value) { *dst++ = value; return --remaining == 0; };
    const auto &r = c.resolution;
    const uint32_t x0 = t.enabled ? t.x0 : 0, y0 = t.enabled ? t.y0 : 0;
    const uint32_t width = t.enabled ? t.x1 - t.x0 : r.width;
    const uint32_t height = t.enabled ? t.y1 - t.y0 : r.height;
    auto sample = [&](uint32_t x, uint32_t y, int &luma, int &u, int &v) {
        if (c.flip & 1) x = r.width - 1 - x;
        if (c.flip & 2) y = r.height - 1 - y;
        x = r.x0 + x * (r.x1 - r.x0 + 1) / r.width;
        y = r.y0 + y * (r.y1 - r.y0 + 1) / r.height;
        luma = src[y * HOST_PITCH + x];
        const uint32_t uv = HOST_Y_SIZE + (y / 2) * HOST_PITCH + (x & ~1u);
        u = src[uv];
        v = src[uv + 1];
    };
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x += c.format ? 1 : 2) {
            int luma, u, v;
            sample(x0 + x, y0 + y, luma, u, v);
            if (!c.format) {
                int luma1, u1, v1;
                sample(x0 + x + 1, y0 + y, luma1, u1, v1);
                if (put(static_cast<uint8_t>(luma))) return true;
                if (put(static_cast<uint8_t>((u + u1 + 1) / 2))) return true;
                if (put(static_cast<uint8_t>(luma1))) return true;
                if (put(static_cast<uint8_t>((v + v1 + 1) / 2))) return true;
            } else {
                const int yv = 298 * (luma - 16), uv = u - 128, vv = v - 128;
                const int red = std::clamp((yv + 409 * vv + 128) >> 8, 0, 255);
                const int green = std::clamp((yv - 100 * uv - 208 * vv + 128) >> 8, 0, 255);
                const int blue = std::clamp((yv + 516 * uv + 128) >> 8, 0, 255);
                const uint16_t rgb = (red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3);
                if (put(static_cast<uint8_t>(rgb))) return true;
                if (put(static_cast<uint8_t>(rgb >> 8))) return true;
            }
        }
    }
    return true;
}

static void conversion_oracle() {
    pattern();
    u32 cases = 0;
    auto verify = [&](const Context &c, const Trim &t) {
        const u32 size = output_size(c, t);
        assert(size > 3);
        std::vector<u8> expected(size + 5), actual(size + 5);
        for (u32 capacity : {1u, 3u, size / 2 + 1, size - 1, size, size + 3}) {
            std::fill(expected.begin(), expected.end(), 0xa5);
            std::fill(actual.begin(), actual.end(), 0xa5);
            assert(frozen_convert(source.data(), source.size(), c, t, expected.data() + 1, capacity));
            assert(convert(source.data(), source.size(), c, t, actual.data() + 1, capacity));
            assert(actual == expected);
            assert(actual[0] == 0xa5 && actual[1 + std::min(size, capacity)] == 0xa5);
            cases++;
        }
    };
    for (const auto &resolution : PRESETS) for (u32 flip = 0; flip < 4; flip++) for (u32 format = 0; format < 2; format++) {
        const Context c{resolution, flip, format};
        verify(c, {});
        verify(c, {true, 2, 3, resolution.width - 4, resolution.height - 2});
    }
    for (u32 flip = 0; flip < 4; flip++) {
        const Context c{{3, 3, 6, 8, 13, 13}, flip, 1};
        verify(c, {});
        verify(c, {true, 1, 1, 3, 3});
    }
    printf("camera conversion oracle: %u byte-identical full/trim/partial cases with canaries\n", cases);
}

using Converter = bool (*)(const u8 *, u32, const Context &, const Trim &, u8 *, u32);
static volatile u64 benchmark_checksum;
__attribute__((noinline)) static double measure(Converter converter, const Context &c) {
    std::vector<u8> output(MAX_OUTPUT_SIZE);
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < 100; i++) {
        assert(converter(source.data(), source.size(), c, {}, output.data(), output.size()));
        benchmark_checksum = benchmark_checksum + output[(i * 97) % output.size()];
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 100;
}
static void benchmark() {
    pattern();
    for (u32 format = 0; format < 2; format++) {
        Context c{PRESETS[0], 0, format};
        std::array<double, 5> before, after;
        for (u32 i = 0; i < before.size(); i++) {
            if (i & 1) { after[i] = measure(convert, c); before[i] = measure(frozen_convert, c); }
            else { before[i] = measure(frozen_convert, c); after[i] = measure(convert, c); }
        }
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        printf("host CPU VGA %s: frozen %.3f ms/frame, mapped %.3f ms/frame, %.2fx (median of 5 x 100 frames)\n",
               format ? "RGB565" : "YUYV", before[2], after[2], before[2] / after[2]);
    }
    printf("host CPU conversion only; no Wii U sensor/GPU/FPS measurement; checksum %llu\n", (unsigned long long)benchmark_checksum);
}

static void conversion() {
    pattern();
    std::vector<u8> output(MAX_OUTPUT_SIZE + 2, 0xa5);
    Context c;
    Trim trim;
    for (const auto &preset : PRESETS) {
        c.resolution = preset;
        assert(convert(source.data(), source.size(), c, trim, output.data() + 1, MAX_OUTPUT_SIZE));
        assert(output[1] == luma(preset.x0, preset.y0));
        const u32 sx = preset.x0 + (preset.width - 1) * (preset.x1 - preset.x0 + 1) / preset.width;
        const u32 sy = preset.y0 + (preset.height - 1) * (preset.y1 - preset.y0 + 1) / preset.height;
        assert(output[1 + (preset.width * preset.height - 1) * 2] == luma(sx, sy));
        assert(output.front() == 0xa5 && output.back() == 0xa5);
    }
    c.resolution = {4, 2, 6, 8, 13, 11};
    for (u32 flip = 0; flip < 4; flip++) {
        c.flip = flip;
        assert(convert(source.data(), source.size(), c, trim, output.data() + 1, 16));
        for (u32 y = 0; y < 2; y++) for (u32 x = 0; x < 4; x++)
            assert(output[1 + (y * 4 + x) * 2] == luma(6 + ((flip & 1) ? 3 - x : x) * 2,
                                                                     8 + ((flip & 2) ? 1 - y : y) * 2));
    }
    c.flip = 0;
    trim = {true, 2, 1, 4, 2};
    assert(convert(source.data(), source.size(), c, trim, output.data() + 1, 4));
    assert(output[1] == luma(10, 10) && output[3] == luma(12, 10));
    assert(output[2] == (source[HOST_Y_SIZE + 5 * HOST_PITCH + 10] + source[HOST_Y_SIZE + 5 * HOST_PITCH + 12] + 1) / 2);
    assert(output[4] == (source[HOST_Y_SIZE + 5 * HOST_PITCH + 11] + source[HOST_Y_SIZE + 5 * HOST_PITCH + 13] + 1) / 2);
    c.format = 1;
    for (auto test : {std::array<int, 4>{16, 128, 128, 0}, {235, 128, 128, 0xffff},
                      {81, 90, 240, 0xf800}, {145, 54, 34, 0x07e0}, {41, 240, 110, 0x001f}}) {
        color(test[0], test[1], test[2]);
        assert(convert(source.data(), source.size(), c, trim, output.data() + 1, 4));
        assert(output[1] == (test[3] & 255) && output[2] == (test[3] >> 8));
    }
    c.resolution = {3, 1, 0, 0, 2, 0}; trim = {};
    assert(output_size(c, trim) == 6 && convert(source.data(), source.size(), c, trim, output.data() + 1, 6));
    c.format = 0;
    assert(!output_size(c, trim));
    c.format = 1; c.resolution = {4, 2, 6, 8, 13, 11}; trim = {true, 2, 1, 4, 2};
    std::array<u8, 4> full;
    assert(convert(source.data(), HOST_SIZE, c, trim, full.data(), full.size()));
    for (u32 n = 1; n < full.size(); n++) {
        std::fill(output.begin(), output.end(), 0xa5);
        assert(convert(source.data(), HOST_SIZE, c, trim, output.data() + 1, n));
        assert(std::equal(full.begin(), full.begin() + n, output.begin() + 1));
        assert(output[0] == 0xa5 && output[n + 1] == 0xa5);
    }
    std::fill(output.begin(), output.end(), 0xa5);
    assert(!convert(source.data(), HOST_SIZE - 1, c, trim, output.data() + 1, 4));
    assert(!convert(source.data(), HOST_SIZE, c, trim, output.data() + 1, 0));
    c.resolution.width = 0xffffffff;
    assert(!convert(source.data(), HOST_SIZE, c, trim, output.data() + 1, MAX_OUTPUT_SIZE));
    c.resolution = PRESETS[0]; c.format = 0; trim = {true, 1, 0, 4, 2};
    assert(!convert(source.data(), HOST_SIZE, c, trim, output.data() + 1, MAX_OUTPUT_SIZE));
    trim = {true, 0, 0, 642, 480};
    assert(!convert(source.data(), HOST_SIZE, c, trim, output.data() + 1, MAX_OUTPUT_SIZE));
    for (u8 b : output) assert(b == 0xa5);
}

static void ipc_service() {
    using namespace kernel;
    pattern();
    register_cam();
    assert(call(1, {0}) == 0 && starts == 0 && timers.empty());
    available = false;
    assert(call(0x13, {1}) != 0 && timers.empty());
    available = true;
    assert(call(0x13, {3}) != 0);
    assert(call(0x13, {1}) == 0);
    auto vsync = event_call(5, {1}), error = event_call(6, {1});
    assert(call(5, {3}) != 0 && call(7, {0x1000, 3, 98304, 256}) != 0);
    assert(call(0x1f, {1, 8, 1}) != 0 && call(0x20, {1, 13}) != 0 && call(0x14, {1, 3}) != 0);
    auto received = event_call(7, {0x1001, 1, 98304, 256});
    advance(70000000);
    assert(vsync->signaled && !received->signaled && writes == 0);
    assert(call(1, {1}) == 0);
    assert(call(3, {0}) == 0 && r32(0x108) == 1);
    assert(call(3, {1}) == 0 && r32(0x108) == 1);
    assert(call(3, {3}) == 0 && r32(0x108) == 0);
    advance(70000000);
    assert(received->signaled && writes == 1 && memory[0x1001] == luma(0, 0));
    assert(memory[0x1000] == 0xcc && memory[0x1001 + 98304] == 0xcc);
    assert(call(8, {1}) == 0 && r32(0x108) == 1);
    received = event_call(7, {0x20001, 1, 98304, 256});
    assert(!received->signaled);
    assert(call(2, {1}) == 0);
    advance(70000000);
    assert(!received->signaled && writes == 1 && memory[0x20001] == 0xcc);
    received = event_call(7, {0x20001, 1, 98304, 256});
    assert(call(1, {1}) == 0);
    advance(70000000);
    assert(received->signaled && writes == 2);
    received = event_call(7, {0x40001, 1, 98304, 256});
    fresh = false;
    advance(300000000);
    assert(!received->signaled && writes == 2);
    fresh = true;
    advance(70000000);
    assert(received->signaled && writes == 3);
    writable = false;
    assert(call(7, {0x60001, 1, 98304, 256}) == RES_INVALID_ADDRESS);
    writable = true;
    assert(call(7, {0xfffffff0, 1, 98304, 256}) == RES_INVALID_ADDRESS);
    assert(call(7, {static_cast<u32>(memory.size()) - 16, 1, 98304, 256}) == RES_INVALID_ADDRESS);
    received = event_call(7, {0x120001, 1, 98303, 256});
    advance(70000000);
    assert(received->signaled && writes == 4);
    std::vector<u8> full_yuyv(98304);
    assert(convert(source.data(), source.size(), Context{}, Trim{}, full_yuyv.data(), full_yuyv.size()));
    assert(std::equal(full_yuyv.begin(), full_yuyv.end() - 1, memory.begin() + 0x120001));
    assert(memory[0x120000] == 0xcc && memory[0x120001 + 98303] == 0xcc);
    received = event_call(7, {0x60001, 1, 98304, 256});
    writable = false;  // memory unmapped after arming must be checked again when the frame arrives
    advance(70000000);
    writable = true;
    assert(error->signaled && !received->signaled && writes == 4 && memory[0x60001] == 0xcc);
    assert(call(4, {1}) == 0 && !error->signaled);
    assert(call(0x1f, {1, 5, 1}) == 0);
    assert(call(0x13, {2}) == 0);  // inner sensor has its own default mirror
    received = event_call(7, {0x80001, 1, 98304, 256});
    assert(call(1, {1}) == 0);
    advance(70000000);
    assert(received->signaled && memory[0x80001] == luma(637, 0));
    assert(call(0x13, {1}) == 0);
    assert(call(0x1e, {1, 4, 2, 6, 8, 13, 11, 2}) == 0);
    assert(call(0x1d, {1, 2, 2}) == 0);
    assert(call(0x14, {1, 2}) == 0);
    assert(call(0x12, {1, 2, 1, 4, 2}) == 0);
    assert(call(0xe, {1, 1}) == 0);
    assert(call(0xf, {1}) == 0 && r32(0x108) == 1);
    assert(call(0x11, {1}) == 0 && r32(0x108) == 1 && r32(0x110) == 3);
    assert(call(7, {0xa0001, 1, 4, 256}) != 0);  // odd YUYV crop origin rejected
    assert(call(0x10, {1, 2, 1, 4, 2}) == 0);
    assert(call(0x13, {4}) == 0);  // activating port 2 retains port 1
    assert(call(0x3b) == 0 && r32(0x108) == 5);
    assert(call(0x1e, {4, 2, 2, 0, 0, 1, 1, 1}) == 0);
    assert(call(0x25, {4, 1, 1}) == 0);
    received = event_call(7, {0xa0001, 1, 4, 256});
    auto second = event_call(7, {0xb0001, 2, 8, 256});
    assert(call(1, {3}) == 0);
    assert(call(3, {3}) == 0 && r32(0x108) == 1);
    color(81, 90, 240);
    const int before = writes;
    advance(70000000);
    assert(received->signaled && second->signaled && writes == before + 2);
    assert(memory[0xa0001] == 81 && memory[0xa0002] == 90 && memory[0xa0003] == 81 && memory[0xa0004] == 240);
    assert(memory[0xb0001] == 0 && memory[0xb0002] == 0xf8);
    assert(memory[0xa0000] == 0xcc && memory[0xa0005] == 0xcc && memory[0xb0000] == 0xcc && memory[0xb0009] == 0xcc);
    received = event_call(7, {0xc0001, 1, 4, 256});
    assert(call(0x3a) == 0 && !opened && !received->signaled);
    const int final_writes = writes;
    advance(300000000);
    assert(writes == final_writes && timers.empty());
    assert(call(0x39) == 0 && call(0x13, {1}) == 0);  // reset restores the two context defaults
    assert(call(0x14, {1, 2}) == 0);
    auto reset_frame = event_call(7, {0xc0001, 1, 614400, 256});
    assert(call(1, {1}) == 0);
    advance(70000000);
    assert(reset_frame->signaled && memory[0xc0001 + 614400 - 1] == 240);
    assert(call(0x13, {0}) == 0 && !opened && stops == 2);
}

static void cadence_and_packages() {
    // 20 fps must keep its 50 ms deadlines even though the native lens produces 30 fps.
    for (auto rate : {std::array<u64, 2>{6, 50000000}, {5, 200000000}, {8, 33333334}, {4, 117647059}}) {
        assert(call(0x39) == 0);
        assert(call(0x33, {1, static_cast<u32>(rate[0] << 16)}) == 0);
        assert(call(0x13, {1}) == 0);
        vsync_times.clear();
        const u64 begin = clock_ns;
        advance(rate[1] * 4);
        assert(vsync_times.size() == 4);
        for (u32 i = 0; i < 4; i++) assert(vsync_times[i] == begin + rate[1] * (i + 1));
        assert(call(0x13, {0}) == 0);
    }
    assert(call(0x39) == 0);
    assert(call(0x34, {1 | 2 << 8 | 1 << 16, 7}) == 0);
    assert(call(0x14, {1, 2}) == 0);
    assert(call(7, {static_cast<u32>(memory.size()) - 16, 1, 400 * 240 * 2 - 1, 256}) != 0);
    assert(call(0x35, {1 | 2 << 8 | 2 << 16, 4 | 2 << 16, 6 | 8 << 16, 13 | 11 << 16}) == 0);
    assert(call(0x13, {1}) == 0 && call(1, {1}) == 0);
    auto received = event_call(7, {0xd0001, 1, 16, 256});
    pattern();
    advance(70000000);
    assert(received->signaled && memory[0xd0001] == luma(6, 10));
    assert(call(0x34, {1 | 2 << 8, 8}) != 0);
    assert(call(0x35, {1 | 2 << 8, 0xffffffff, 0, 0}) != 0);
    assert(call(0x3a) == 0);
}

static void y2r_parity() {
    auto camera = service;
    assert(call(0x39) == 0);
    assert(call(0x1e, {1, 8, 2, 0, 0, 7, 1, 1}) == 0);
    assert(call(0x20, {1, 8}) == 0);
    assert(call(0x36) == 0 && r32(0x108) == 2);
    const u32 coefficient = r32(0x108);
    kernel::register_y2r();
    auto y2r = service;
    service = camera;
    assert(call(0x13, {1}) == 0 && call(1, {1}) == 0);
    for (u32 y : {16u, 235u}) {
        color(y, 128, 128);
        assert(call(0x25, {1, 0, 1}) == 0);
        auto received = event_call(7, {0x170001, 1, 32, 256});
        advance(40000000);
        assert(received->signaled);
        assert(call(0x25, {1, 1, 1}) == 0);
        received = event_call(7, {0x180001, 1, 32, 256});
        advance(40000000);
        assert(received->signaled);
        service = y2r;
        assert(call(0x29, {4 | 3 << 8, 8 | 2 << 16, coefficient | 0xff << 16}) == 0);
        assert(call(0x13, {0x170001, 32, 0, 0}) == 0);
        assert(call(0x18, {0x190001, 32, 0, 0}) == 0);
        auto done = event_call(0xf, {});
        assert(call(0x26) == 0 && done->signaled);
        assert(last_write_exact && last_write_addr == 0x190001 && last_write_size == 32);
        for (u32 i = 0; i < 32; i++) {
            assert(memory[0x180001 + i] == (y == 16 ? 0 : 0xff));
            assert(memory[0x180001 + i] == memory[0x190001 + i]);
        }
        service = camera;
    }
    assert(call(0x3a) == 0);
}

static void y2r_strided_output() {
    const auto camera = service;
    kernel::register_y2r();
    constexpr u32 src = 0x170001, dst = 0x190001;
    assert(call(0x29, {4 | 3 << 8 | 1 << 24, 8 | 8 << 16, 2 | 0xff << 16}) == 0);
    assert(call(0x13, {src, 128, 0, 0}) == 0);
    for (u32 unit : {4u, 3u}) {
        constexpr u32 gap = 4;
        const u32 span = 128 + 127 / unit * gap;
        assert(call(0x18, {dst, 128, unit, gap}) == 0);
        std::fill_n(memory.data() + dst, span + 1, 0xcc);
        for (u8 y : {16, 235}) {
            for (u32 i = 0; i < 128; ++i) memory[src + i] = i & 1 ? 128 : y;
            assert(call(0x26) == 0);
            assert(!last_write_exact && last_write_addr == dst && last_write_size == span);
            for (u32 i = 0; i <= span; ++i)
                assert(memory[dst + i] == (i == span || i % (unit + gap) >= unit ? 0xcc : y == 16 ? 0 : 0xff));
        }
        for (u32 i = 0; i < 64; ++i) {
            memory[src + 2 * i] = ((i & 1) ^ ((i >> 3) & 1)) ? 235 : 16;
            memory[src + 2 * i + 1] = 128;
        }
        assert(call(0x26) == 0);
        for (u32 i = 0; i < 128; ++i) {
            const u32 pixel = i / 2;
            const u8 expected = ((pixel & 1) ^ ((pixel >> 1) & 1)) ? 0xff : 0;
            assert(memory[dst + i + i / unit * gap] == expected);
        }
    }
    const int old_writes = writes;
    assert(call(0x18, {dst, 127, 4, 4}) == 0 && call(0x26) == kernel::RES_INVALID_ADDRESS);
    assert(call(0x18, {static_cast<u32>(memory.size()) - 128, 128, 4, 4}) == 0 &&
           call(0x26) == kernel::RES_INVALID_ADDRESS);
    assert(writes == old_writes);
    published = false;
    service = camera;
}

static void query_replies() {
    assert(call(0x39) == 0);
    for (u32 cmd : {0x1au, 0x1cu}) {
        assert(call(cmd, {1}) == 0 && r32(0x100) == (cmd << 16 | 0x80) && r32(0x108) == 1);
        assert(call(cmd, {0}) != 0 && call(cmd, {3}) != 0);
        assert(call(cmd - 1, {5, 0}) == 0);
        assert(call(cmd, {1}) == 0 && r32(0x108) == 0);
        assert(call(cmd, {2}) == 0 && r32(0x108) == 1);
        assert(call(cmd, {4}) == 0 && r32(0x108) == 0);
    }
    assert(call(0x33, {1, 1 | 8 << 16}) == 0);
    assert(call(0x1a, {1}) == 0 && r32(0x108) == 1);
    assert(call(0x1c, {1}) == 0 && r32(0x108) == 0);
    assert(call(0x3c) == 0 && r32(0x108) == 0);
    assert(call(0x3d, {4}) == 0 && call(0x3c) == 0 && r32(0x108) == 4);
    assert(call(0x3d, {8}) != 0);
    assert(call(0x2b) == 0 && r32(0x100) == 0x002b0440);
    auto number = [](u32 word) { const u32 bits = r32(0x100 + word * 4); float f; memcpy(&f, &bits, sizeof f); return f; };
    assert(number(3) == 1 && number(9) == 65 && number(10) == 65 && number(11) == 250 && number(12) == 35);
    assert(r32(0x134) == (640u | 480u << 16));
    for (u32 i = 14; i < 18; i++) assert(r32(0x100 + i * 4) == 0);
    assert(call(0x32) != 0);
    assert(call(0x13, {1}) == 0);
    const u64 began = clock_ns;
    advance(100000002);
    assert(call(0x2a, {1, 5}) == 0 && r32(0x100) == 0x002a0042);
    assert(r32(0x108) == (40u << 14 | 2) && r32(0x10c) == receive_address);
    for (u32 i = 0; i < 3; i++) assert(r64(receive_address + i * 8) == (began + (3 - i) * 33333334) / 1000);
    assert(r64(receive_address + 24) == 0 && r64(receive_address + 32) == 0);
    assert(memory[receive_address - 1] == 0xcc && memory[receive_address + 40] == 0xcc);
    std::fill_n(memory.data() + receive_address, 40, 0xa5);
    receive_capacity = 8;
    assert(call(0x2a, {1, 5}) == kernel::RES_INVALID_ADDRESS);
    receive_capacity = 40;
    writable = false;
    assert(call(0x2a, {1, 5}) == kernel::RES_INVALID_ADDRESS);
    writable = true;
    for (u32 i = 0; i < 40; i++) assert(memory[receive_address + i] == 0xa5);
    assert(call(0x2a, {3, 1}) != 0 && call(0x2a, {1, 6}) != 0);
    assert(call(0x2a, {1, 0}) == 0 && r32(0x108) == 2);
    assert(call(0x13, {0}) == 0 && call(0x39) == 0);
    assert(call(0x2a, {1, 5}) == 0 && r64(receive_address) == 0);
}

static void byte_arguments() {
    assert(call(0x39) == 0);
    // Nintendo's wrapper uses STRB, so the other three bytes of each IPC word can remain dirty.
    assert(call(0x1d, {0xaabbcc03, 0x44556601, 0x77889903}) == 0);
    assert(call(0x1d, {0xaabbcc08, 0x44556601, 0x77889903}) != 0);
    assert(call(0x1d, {0xaabbcc03, 0x44556604, 0x77889903}) != 0);
    assert(call(0x1d, {0xaabbcc03, 0x44556601, 0x77889904}) != 0);
    assert(call(0x1f, {0xaabbcc04, 0x44556605, 0x77889901}) == 0);
    assert(call(0x1d, {0xaabbcc04, 0x44556600, 0x77889901}) == 0);
    assert(call(0x14, {0xaabbcc04, 0x44556601}) == 0);
    assert(call(0x25, {0xaabbcc04, 0x44556600, 0x77889901}) == 0);
    assert(call(0x20, {0xaabbcc04, 0x44556608}) == 0);
    assert(call(0x19, {0xaabbcc04, 0}) == 0);
    assert(call(0x1a, {0xaabbcc04}) == 0 && r32(0x108) == 0);
    assert(call(0x1c, {0xaabbcc04}) == 0 && r32(0x108) == 1);
    assert(call(0x3d, {0xaabbcc04}) == 0 && call(0x3c) == 0 && r32(0x108) == 4);
    assert(call(0x13, {0xaabbcc04}) == 0);
    assert(call(1, {0xaabbcc02}) == 0);
    auto received = event_call(7, {0x1e0001, 0xaabbcc02, 98304, 256});
    advance(70000000);
    assert(received->signaled && memory[0x1e0001 + 98304] == 0xcc);
    assert(call(3, {0xaabbcc02}) == 0 && r32(0x108) == 1);
    assert(call(0x2a, {0xaabbcc02, 1}) == 0 && r64(receive_address) != 0);
    assert(call(0x2a, {0xaabbcc02, 0x100}) != 0);  // the history count is a full word, not a byte
    assert(call(0x13, {0xaabbcc00}) == 0);
}

static void y2r_coefficients() {
    const auto camera = service;
    kernel::register_y2r();
    assert(call(0x2b) == 0);
    auto done = event_call(0xf, {});
    // Wire words are independent fixtures, including the signed offsets' two's-complement halves.
    constexpr std::array<std::array<u32, 4>, 4> standard = {{
        {0x01660100, 0x005800b6, 0xe99101c5, 0xe3a510ee},
        {0x01930100, 0x002f0077, 0xe6cd01db, 0xe2af0a7c},
        {0x0198012a, 0x006400d0, 0xe4220204, 0xdd6510f2},
        {0x01ca012a, 0x00360088, 0xe0fc021c, 0xdbdf099c},
    }};
    constexpr std::array<u32, 4> zero{}, custom{0x8000ffff, 0x7fff0001, 0xabcd1234, 0x0000fedc};
    auto query = [&](u32 command, u32 index, const std::array<u32, 4> &expected, u32 result = 0) {
        constexpr u32 sentinel = 0xa55a3cc3;
        w32(0xfc, sentinel);
        w32(0x200, sentinel);
        for (u32 i = 0; i < 64; i++) w32(0x100 + 4 * i, sentinel);
        w32(0x100, command << 16 | (command == 0x21 ? 0x40 : 0));
        if (command == 0x21) w32(0x104, index);
        kernel::IPC ipc(nullptr, 0x100, &ipc_lock);
        service->handle(ipc);
        assert(r32(0x100) == (command << 16 | (result ? 1u : 5u) << 6));
        assert(r32(0x104) == result);
        std::array<u32, 4> actual;
        for (u32 i = 0; i < 4; i++) {
            actual[i] = r32(0x108 + 4 * i);
            assert(actual[i] == (result ? sentinel : expected[i]));
        }
        for (u32 i = 6; i < 64; i++) assert(r32(0x100 + 4 * i) == sentinel);
        assert(r32(0xfc) == sentinel && r32(0x200) == sentinel);
        return actual;
    };
    puts("Y2R coefficient getter IPC tests");
    fflush(stdout);
    query(0x21, 0, standard[0]);
    query(0x1f, 0, zero);  // reading a standard did not replace DriverInitialize's zero coefficients
    assert(!done->signaled);
    assert(call(0x1e, {custom[0], custom[1], custom[2], custom[3]}) == 0);
    for (u32 index = 0; index < standard.size(); index++) {
        query(0x21, index, standard[index]);
        query(0x1f, 0, custom);
        assert(!done->signaled);
    }
    for (u32 index : {4u, 0x100u, 0x80000000u, 0xffffffffu}) {
        query(0x21, index, zero, 0xe0e053ed);
        query(0x1f, 0, custom);
        assert(!done->signaled);
    }
    assert(call(0x2b) == 0);
    query(0x1f, 0, zero);

    constexpr u32 input = 0x210001, direct_output = 0x220001, restored_output = 0x230001;
    constexpr std::array<u8, 32> samples = {
        16, 128, 16, 128, 235, 128, 235, 128, 81, 90, 81, 240, 145, 54, 145, 34,
        41, 240, 41, 110, 0, 0, 255, 255, 64, 200, 192, 20, 17, 129, 234, 127,
    };
    std::fill_n(memory.data() + input - 1, samples.size() + 2, 0xa5);
    std::copy(samples.begin(), samples.end(), memory.begin() + input);
    assert(call(1, {4}) == 0 && call(3, {3}) == 0);  // YUYV422 to RGB565, linear 8x2
    assert(call(0x1a, {8}) == 0 && call(0x1c, {2}) == 0 && call(0x22, {0xa6}) == 0);
    assert(call(0x13, {input, samples.size(), 0, 0}) == 0);
    auto convert_frame = [&](u32 address) {
        std::fill_n(memory.data() + address - 1, samples.size() + 2, 0xa5);
        assert(call(0x18, {address, samples.size(), 0, 0}) == 0);
        done->signaled = false;
        assert(call(0x26) == 0 && done->signaled);
        assert(memory[address - 1] == 0xa5 && memory[address + samples.size()] == 0xa5);
        return std::vector<u8>(memory.begin() + address, memory.begin() + address + samples.size());
    };
    for (u32 index = 0; index < standard.size(); index++) {
        assert(call(0x20, {index}) == 0);
        query(0x1f, 0, standard[index]);
        const auto direct = convert_frame(direct_output);
        assert(call(0x1e, {custom[0], custom[1], custom[2], custom[3]}) == 0);
        done->signaled = false;
        const int before_writes = writes;
        const auto returned = query(0x21, index, standard[index]);
        query(0x1f, 0, custom);
        assert(!done->signaled && writes == before_writes);
        assert(call(0x1e, {returned[0], returned[1], returned[2], returned[3]}) == 0);
        assert(convert_frame(restored_output) == direct);
        if (index == 2) {
            const std::array<u16, 5> colors = {0, 0xffff, 0xf800, 0x07e0, 0x001f};
            for (u32 i = 0; i < colors.size(); i++) {
                assert(direct[4 * i] == (colors[i] & 0xff));
                assert(direct[4 * i + 1] == (colors[i] >> 8));
            }
        }
        assert(call(2) == 0 && r32(0x108) == 4);
        assert(call(4) == 0 && r32(0x108) == 3);
        assert(call(0x1b) == 0 && r32(0x108) == 8);
        assert(call(0x1d) == 0 && r32(0x108) == 2);
        assert(call(0x23) == 0 && r32(0x108) == 0xa6);
    }
    assert(std::equal(samples.begin(), samples.end(), memory.begin() + input));
    assert(memory[input - 1] == 0xa5 && memory[input + samples.size()] == 0xa5);
    service = camera;
    puts("Y2R getters: four standards, signed custom/zero words, guards, errors, state and real conversion parity passed");
}

int main(int argc, char **argv) {
    conversion_oracle();
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) { benchmark(); return 0; }
    conversion();
    ipc_service();
    cadence_and_packages();
    y2r_parity();
    y2r_strided_output();
    query_replies();
    byte_arguments();
    y2r_coefficients();
    puts("camera: padded NV12, crops/flips, RGB565LE/Y2R parity, actual IPC partial receive/cancel/rearm, busy masks and bounds passed");
}
