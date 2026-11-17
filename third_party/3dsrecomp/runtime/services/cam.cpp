// cam:u: one host lens supplies the three logical sensors; guest settings belong to each sensor/context.
#include "../service.h"
#include "services.h"
#include "camera_frame.h"

__attribute__((weak)) bool cam_host_start() { return false; }
__attribute__((weak)) void cam_host_stop() {}
__attribute__((weak)) bool cam_host_frame(u8 *, u32) { return false; }

namespace kernel {
namespace {
constexpr u32 MIN_TRANSFER_UNIT = 256, MAX_BUFFER_SIZE = 2560;
constexpr u64 HOST_POLL_NS = 33333334;
constexpr u64 PERIOD_NS[] = {66666667, 66666667, 66666667, 100000000, 117647059, 200000000,
                             50000000, 50000000, 33333334, 33333334, 66666667, 50000000, 33333334};

struct Camera {
    camera_frame::Context contexts[2];
    u32 context = 0, rate = 0;
    bool auto_exposure = true, auto_white_balance = true;
};
struct Port {
    std::shared_ptr<Event> vsync = make_event(ONESHOT, "cam vsync");
    std::shared_ptr<Event> error = make_event(ONESHOT, "cam error");
    std::shared_ptr<Event> received = make_event(STICKY, "cam received");
    camera_frame::Trim trim;
    u32 camera = 0, dst = 0, size = 0, unit = MIN_TRANSFER_UNIT;
    u64 next_frame = 0, last_frame = 0;
    std::array<u64, 5> vsync_times = {};
    bool active = false, receiving = false, capturing = false;
};

struct Cam : Service {
    using Service::Service;
    Camera cameras[3];
    Port ports[2];
    std::array<u8, camera_frame::HOST_SIZE> snapshot;
    u64 frame_number = 0, generation = 0, warned_errors = 0;
    u32 sleep_camera = 0;
    bool ticking = false, host_started = false;

    template <class F> void each(u32 mask, F &&f) {
        for (u32 i = 0; i < 2; i++) if (mask & (1u << i)) f(ports[i]);
    }
    template <class F> void contexts(u32 mask, u32 context_mask, F &&f) {
        for (u32 i = 0; i < 3; i++) if (mask & (1u << i))
            for (u32 j = 0; j < 2; j++) if (context_mask & (1u << j)) f(cameras[i].contexts[j]);
    }
    bool select(IPC &ipc, u32 value, u32 limit, bool single = false) {
        if (value < limit && (!single || (value && !(value & (value - 1))))) return true;
        ipc.result(RES_INVALID_COMBINATION);
        return false;
    }
    camera_frame::Context &context(Port &p) { return cameras[p.camera].contexts[cameras[p.camera].context]; }
    u32 activated() {
        u32 mask = 0;
        for (auto &p : ports) if (p.active) mask |= 1u << p.camera;
        return mask;
    }
    bool start_host(IPC &ipc) {
        if (host_started || (host_started = cam_host_start())) return true;
        ipc.result(RES_NOT_FOUND);
        return false;
    }
    void stop() {
        for (auto &p : ports) {
            p.active = p.capturing = p.receiving = false;
            p.vsync->clear();
            p.error->clear();
            p.received->clear();
            p.last_frame = 0;
            p.vsync_times = {};
        }
        if (host_started) cam_host_stop();
        host_started = ticking = false;
        frame_number = 0;
        generation++;  // scheduled callbacks from before finalization must not touch a new session
    }
    void initialize() {
        stop();
        sleep_camera = 0;
        for (u32 i = 0; i < 3; i++) {
            cameras[i] = {};
            cameras[i].contexts[0].resolution = camera_frame::PRESETS[5];
            cameras[i].contexts[1].resolution = camera_frame::PRESETS[0];
            for (auto &c : cameras[i].contexts) c.flip = i == 1 ? 1 : 0;
        }
        for (u32 i = 0; i < 2; i++) {
            ports[i].camera = i ? 2 : 0;
            ports[i].trim = {};
            ports[i].unit = MIN_TRANSFER_UNIT;
        }
    }
    bool destination(Port &p, u32 addr, u32 size) {
        const u32 needed = camera_frame::output_size(context(p), p.trim);
        return needed && guest_writable(addr, size);
    }
    void set_rate(u32 mask, u32 rate) {
        for (u32 i = 0; i < 3; i++) if (mask & (1u << i)) cameras[i].rate = rate;
        for (auto &p : ports) if (p.active && (mask & (1u << p.camera))) p.next_frame = now_ns() + PERIOD_NS[rate];
    }
    void schedule() {
        const u64 now = now_ns();
        u64 next = now + HOST_POLL_NS;
        for (auto &p : ports) if (p.active && p.next_frame > now) next = std::min(next, p.next_frame);
        const u64 token = generation;
        add_timeout(next, [this, token] { if (token == generation) frame(); });
    }
    // The kernel lock is held; host snapshots never retain pointers into guest memory.
    void frame() {
        if (!activated()) { ticking = false; return; }
        if (cam_host_frame(snapshot.data(), snapshot.size())) frame_number++;
        const u64 now = now_ns();
        for (auto &p : ports) {
            if (!p.active || now < p.next_frame || p.last_frame == frame_number) continue;
            p.last_frame = frame_number;
            p.next_frame += PERIOD_NS[cameras[p.camera].rate];
            if (p.next_frame <= now) p.next_frame = now + PERIOD_NS[cameras[p.camera].rate];
            for (u32 i = p.vsync_times.size() - 1; i > 0; i--) p.vsync_times[i] = p.vsync_times[i - 1];
            p.vsync_times[0] = now / 1000;
            p.vsync->signal();
            if (!p.capturing || !p.receiving) continue;
            p.receiving = false;
            if (!destination(p, p.dst, p.size) ||
                !camera_frame::convert(snapshot.data(), snapshot.size(), context(p), p.trim, gptr(p.dst), p.size)) {
                p.error->signal();
                continue;
            }
            gsp_cpu_wrote(p.dst, std::min(p.size, camera_frame::output_size(context(p), p.trim)));
            p.received->signal();
        }
        schedule();
    }
    void start_frames() {
        if (!ticking) { ticking = true; schedule(); }
    }

    void handle(IPC &ipc) override {
        VLOG("[cam] command %04x: %x %x %x %x\n", ipc.cmd(), ipc[1], ipc[2], ipc[3], ipc[4]);
        // SDK wrappers can write byte arguments with STRB, leaving the rest of each IPC word unchanged.
        const u32 byte1 = ipc[1] & 0xff, byte2 = ipc[2] & 0xff, byte3 = ipc[3] & 0xff;
        switch (ipc.cmd()) {
        case 0x0001: {  // StartCapture(port)
            if (!select(ipc, byte1, 4)) break;
            bool active = false;
            each(byte1, [&](Port &p) { active |= p.active; });
            if (!active) { ipc.ok(); break; }
            if (!start_host(ipc)) break;
            each(byte1, [](Port &p) { if (p.active) p.capturing = true; });
            start_frames();
            ipc.ok();
            break;
        }
        case 0x0002:  // StopCapture(port): pending receives are cancelled, including ones armed before capture
            if (!select(ipc, byte1, 4)) break;
            each(byte1, [](Port &p) { p.capturing = p.receiving = false; p.received->clear(); });
            ipc.ok();
            break;
        case 0x0003: {  // IsBusy(port)
            if (!select(ipc, byte1, 4)) break;
            bool busy = true;
            each(byte1, [&](Port &p) { busy &= p.capturing; });
            ipc.ok(2);
            ipc.set(2, busy);
            break;
        }
        case 0x0004:  // ClearBuffer(port)
            if (!select(ipc, byte1, 4)) break;
            each(byte1, [](Port &p) { p.error->clear(); p.received->clear(); });
            ipc.ok();
            break;
        case 0x0005:  // GetVsyncInterruptEvent(port)
        case 0x0006: {  // GetBufferErrorInterruptEvent(port)
            if (!select(ipc, byte1, 4, true)) break;
            Port &p = ports[byte1 == 2];
            ipc.ok(1, 2);
            ipc.handles(2, {ipc.cmd() == 5 ? p.vsync : p.error});
            break;
        }
        case 0x0007: {  // SetReceiving(destination, port, image size, transfer unit, process handle)
            if (!select(ipc, byte2, 4, true)) break;
            Port &p = ports[byte2 == 2];
            if (!destination(p, ipc[1], ipc[3])) { ipc.result(RES_INVALID_ADDRESS); break; }
            p.dst = ipc[1];
            p.size = ipc[3];
            p.receiving = true;
            p.received->clear();
            ipc.ok(1, 2);
            ipc.handles(2, {p.received});
            break;
        }
        case 0x0008:  // IsFinishedReceiving(port)
            if (!select(ipc, byte1, 4, true)) break;
            ipc.ok(2);
            ipc.set(2, !ports[byte1 == 2].receiving);
            break;
        case 0x0009:  // SetTransferLines(port, lines, width, height)
        case 0x000B:  // SetTransferBytes(port, bytes, width, height)
            if (!select(ipc, byte1, 4)) break;
            each(byte1, [&](Port &p) { p.unit = (ipc[2] & 0xffff) * (ipc.cmd() == 9 ? (ipc[3] & 0xffff) * 2 : 1); });
            ipc.ok();
            break;
        case 0x000A: {  // GetMaxLines(width, height)
            const u32 w = ipc[1] & 0xffff, h = ipc[2] & 0xffff;
            u32 lines = w ? std::min(MAX_BUFFER_SIZE / w, h) : 0;
            while (lines && (h % lines || lines * w * 2 % MIN_TRANSFER_UNIT)) lines--;
            ipc.ok(2);
            ipc.set(2, lines);
            break;
        }
        case 0x000C:  // GetTransferBytes(port)
            if (!select(ipc, byte1, 4, true)) break;
            ipc.ok(2);
            ipc.set(2, ports[byte1 == 2].unit);
            break;
        case 0x000D: {  // GetMaxBytes(width, height)
            const u32 size = (ipc[1] & 0xffff) * (ipc[2] & 0xffff) * 2;
            u32 bytes = MAX_BUFFER_SIZE;
            while (bytes > MIN_TRANSFER_UNIT && size % bytes) bytes -= MIN_TRANSFER_UNIT;
            ipc.ok(2);
            ipc.set(2, size ? bytes : 0);
            break;
        }
        case 0x000E:  // SetTrimming(port, enabled)
            if (!select(ipc, byte1, 4)) break;
            each(byte1, [&](Port &p) { p.trim.enabled = (ipc[2] & 0xff) != 0; });
            ipc.ok();
            break;
        case 0x000F:  // IsTrimming(port)
            if (!select(ipc, byte1, 4, true)) break;
            ipc.ok(2);
            ipc.set(2, ports[byte1 == 2].trim.enabled);
            break;
        case 0x0010:  // SetTrimmingParams(port, x0, y0, x1, y1)
        case 0x0012: {  // SetTrimmingParamsCenter(port, trim width, trim height, camera width, camera height)
            if (!select(ipc, byte1, 4)) break;
            u32 x0 = ipc[2] & 0xffff, y0 = ipc[3] & 0xffff, x1 = ipc[4] & 0xffff, y1 = ipc[5] & 0xffff;
            if (ipc.cmd() == 0x12) {
                if (!x0 || !y0 || x0 > x1 || y0 > y1) { ipc.result(RES_INVALID_COMBINATION); break; }
                const u32 w = x0, h = y0;
                x0 = (x1 - w) / 2; y0 = (y1 - h) / 2; x1 = x0 + w; y1 = y0 + h;
            }
            if (x0 >= x1 || y0 >= y1 || x1 > 640 || y1 > 480) { ipc.result(RES_INVALID_COMBINATION); break; }
            each(byte1, [&](Port &p) { p.trim.x0 = x0; p.trim.y0 = y0; p.trim.x1 = x1; p.trim.y1 = y1; });
            ipc.ok();
            break;
        }
        case 0x0011: {  // GetTrimmingParams(port)
            if (!select(ipc, byte1, 4, true)) break;
            const auto &t = ports[byte1 == 2].trim;
            ipc.ok(5);
            ipc.set(2, t.x0); ipc.set(3, t.y0); ipc.set(4, t.x1); ipc.set(5, t.y1);
            break;
        }
        case 0x0013: {  // Activate(camera): outer 1 / inner share port 1; outer 2 uses port 2
            if (!select(ipc, byte1, 8)) break;
            const u32 mask = byte1;
            if ((mask & 3) == 3) { ipc.result(RES_INVALID_COMBINATION); break; }
            if (!mask) { stop(); ipc.ok(); break; }
            if (!start_host(ipc)) break;
            for (u32 i = 0; i < 2; i++) {
                if (!(mask & (i ? 4 : 3))) continue;
                Port &p = ports[i];
                const u32 camera = i ? 2 : (mask & 1 ? 0 : 1);
                if (p.active && p.camera != camera) { p.active = p.capturing = p.receiving = false; p.received->clear(); }
                p.camera = camera;
                if (!p.active) { p.next_frame = now_ns() + PERIOD_NS[cameras[camera].rate]; p.last_frame = frame_number; }
                p.active = true;
            }
            start_frames();
            ipc.ok();
            break;
        }
        case 0x0014:  // SwitchContext(camera mask, one context bit)
            if (!select(ipc, byte1, 8) || !select(ipc, byte2, 4, true)) break;
            for (u32 i = 0; i < 3; i++) if (byte1 & (1u << i)) cameras[i].context = byte2 == 2;
            ipc.ok();
            break;
        case 0x001D:  // FlipImage(camera, flip, contexts)
        case 0x001F:  // SetSize(camera, preset, contexts)
        case 0x0025: {  // SetOutputFormat(camera, format, contexts)
            if (!select(ipc, byte1, 8) || !select(ipc, byte3, 4)) break;
            const u32 limit = ipc.cmd() == 0x1d ? 4 : ipc.cmd() == 0x1f ? 8 : 2;
            if (!select(ipc, byte2, limit)) break;
            contexts(byte1, byte3, [&](camera_frame::Context &c) {
                if (ipc.cmd() == 0x1d) c.flip = byte2;
                else if (ipc.cmd() == 0x1f) c.resolution = camera_frame::PRESETS[byte2];
                else c.format = byte2;
            });
            ipc.ok();
            break;
        }
        case 0x0019:  // SetAutoExposure(camera mask, enabled)
        case 0x001B:  // SetAutoWhiteBalance(camera mask, enabled)
            if (!select(ipc, byte1, 8)) break;
            for (u32 i = 0; i < 3; i++) if (byte1 & (1u << i)) {
                if (ipc.cmd() == 0x19) cameras[i].auto_exposure = (ipc[2] & 0xff) != 0;
                else cameras[i].auto_white_balance = (ipc[2] & 0xff) != 0;
            }
            ipc.ok();
            break;
        case 0x001A:  // IsAutoExposure(single camera)
        case 0x001C: {  // IsAutoWhiteBalance(single camera)
            if (!select(ipc, byte1, 8, true)) break;
            const Camera &c = cameras[byte1 == 4 ? 2 : byte1 == 2 ? 1 : 0];
            ipc.ok(2);
            ipc.set(2, ipc.cmd() == 0x1a ? c.auto_exposure : c.auto_white_balance);
            break;
        }
        case 0x001E: {  // SetDetailSize(camera, width, height, crop x0/y0/x1/y1, contexts)
            if (!select(ipc, byte1, 8) || !select(ipc, ipc[8] & 0xff, 4)) break;
            const camera_frame::Resolution r{ipc[2] & 0xffff, ipc[3] & 0xffff, ipc[4] & 0xffff,
                                             ipc[5] & 0xffff, ipc[6] & 0xffff, ipc[7] & 0xffff};
            if (!camera_frame::valid(r)) { ipc.result(RES_INVALID_COMBINATION); break; }
            contexts(byte1, ipc[8] & 0xff, [&](camera_frame::Context &c) { c.resolution = r; });
            ipc.ok();
            break;
        }
        case 0x0020:  // SetFrameRate(camera mask, rate)
            if (!select(ipc, byte1, 8) || !select(ipc, byte2, 13)) break;
            set_rate(byte1, byte2);
            ipc.ok();
            break;
        case 0x0033: {  // SetPackageParameterWithoutContext: raw packed bytes, independent of host byte order
            const u32 mask = ipc[1] & 0xff, rate = (ipc[2] >> 16) & 0xff;
            if (!select(ipc, mask, 8) || !select(ipc, rate, 13)) break;
            for (u32 i = 0; i < 3; i++) if (mask & (1u << i)) {
                cameras[i].auto_exposure = (ipc[2] & 0xff) != 0;
                cameras[i].auto_white_balance = ((ipc[2] >> 8) & 0xff) != 0;
            }
            set_rate(mask, rate);
            ipc.ok();
            break;
        }
        case 0x0034:  // SetPackageParameterWithContext
        case 0x0035: {  // SetPackageParameterWithContextDetail
            const u32 mask = ipc[1] & 0xff, context_mask = (ipc[1] >> 8) & 0xff, flip = (ipc[1] >> 16) & 0xff;
            if (!select(ipc, mask, 8) || !select(ipc, context_mask, 4) || !select(ipc, flip, 4)) break;
            camera_frame::Resolution r;
            if (ipc.cmd() == 0x34) {
                const u32 size = ipc[2] & 0xff;
                if (!select(ipc, size, 8)) break;
                r = camera_frame::PRESETS[size];
            } else {
                r = {ipc[2] & 0xffff, ipc[2] >> 16, ipc[3] & 0xffff, ipc[3] >> 16, ipc[4] & 0xffff, ipc[4] >> 16};
                if (!camera_frame::valid(r)) { ipc.result(RES_INVALID_COMBINATION); break; }
            }
            contexts(mask, context_mask, [&](camera_frame::Context &c) { c.flip = flip; c.resolution = r; });
            ipc.ok();
            break;
        }
        case 0x0036:  // GetSuitableY2rStandardCoefficient: ITU-R BT.601, TV ranges
            ipc.ok(2); ipc.set(2, 2);
            break;
        case 0x003B:  // GetActivatedCamera
            ipc.ok(2); ipc.set(2, activated());
            break;
        case 0x003C:  // GetSleepCamera
            ipc.ok(2); ipc.set(2, sleep_camera);
            break;
        case 0x003D:  // SetSleepCamera
            if (!select(ipc, byte1, 8)) break;
            sleep_camera = byte1;
            ipc.ok();
            break;
        case 0x002A: {  // GetLatestVsyncTiming(single port, count): little-endian microseconds, newest first
            if (!select(ipc, byte1, 4, true) || !select(ipc, ipc[2], 6)) break;
            const u32 bytes = ipc[2] * 8;
            u32 capacity = 0;
            const u32 dst = ipc.static_buffer(0, &capacity);
            if (bytes && (capacity < bytes || !guest_writable(dst, bytes))) { ipc.result(RES_INVALID_ADDRESS); break; }
            const Port &p = ports[byte1 == 2];
            for (u32 i = 0; i < ipc[2]; i++) w64(dst + i * 8, p.vsync_times[i]);
            if (bytes) gsp_cpu_wrote(dst, bytes);
            ipc.ok(1, 2);
            ipc.static_desc(2, 0, bytes);
            break;
        }
        case 0x002B: {  // GetStereoCameraCalibrationData
            // Nominal rectification keeps client math finite; one physical lens still cannot provide stereo depth.
            constexpr float parameters[] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 65.0f, 65.0f, 250.0f, 35.0f};
            ipc.ok(17);
            for (u32 i = 0; i < 16; i++) ipc.set(2 + i, 0);
            for (u32 i = 0; i < 10; i++) {
                u32 bits;
                memcpy(&bits, &parameters[i], sizeof bits);
                ipc.set(3 + i, bits);
            }
            ipc.set(13, 640u | 480u << 16);
            break;
        }
        case 0x0032:  // GetImageQualityCalibrationData: no measured host-lens calibration to return
            ipc.result(RES_NOT_FOUND);
            break;
        case 0x0039: initialize(); ipc.ok(); break;  // DriverInitialize
        case 0x003A: stop(); ipc.ok(); break;        // DriverFinalize
        default:
            // shortcut: host exposure/white balance/effects remain automatic; add controls when the host API exposes them.
            ipc.ok();
        }
        const u32 result = r32(ipc.buf + 4);
        if (result && ipc.cmd() < 64 && !(warned_errors & (1ull << ipc.cmd()))) {
            warned_errors |= 1ull << ipc.cmd();
            LOG("[cam] command %04x failed %08x (request %08x %08x %08x)\n", ipc.cmd(), result, ipc[1], ipc[2], ipc[3]);
        }
    }
};
}  // namespace

void register_cam() {
    auto cam = std::make_shared<Cam>("cam:u");
    cam->initialize();
    register_service(cam);
}
}  // namespace kernel
