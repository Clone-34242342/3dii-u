// GamePad NV12 snapshots. The CAM callback owns no guest memory and never takes a kernel lock.
#include <camera/camera.h>
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/time.h>
#include <malloc.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "camera_queue.h"
#include "services.h"
#include "wiiu.h"

namespace {
std::atomic<bool> paused{false}, resume_needed{false}, used{false};
struct Camera {
    std::mutex control, frames;
    wiiu::CameraQueue queue;
    CAMSurface surfaces[wiiu::CameraQueue::COUNT] = {};
    CAMWorkMem work = {};
    CAMHandle handle = -1, callback_handle = -1;
    OSDynLoad_Module module = 0;
    bool wanted = false, shutdown = false, detached = false;
    OSTime retry_after = 0;
    int last_error = 0;
    decltype(&CAMInit) init = nullptr;
    decltype(&CAMExit) exit = nullptr;
    decltype(&CAMOpen) open = nullptr;
    decltype(&CAMClose) close = nullptr;
    decltype(&CAMGetMemReq) mem_req = nullptr;
    decltype(&CAMSubmitTargetSurface) submit = nullptr;
    decltype(&CAMCheckMemSegmentation) check_mem = nullptr;
};
Camera &camera() { static Camera c; return c; }

void event(CAMEventData *data) {
    if (!data) return;
    Camera &c = camera();
    std::lock_guard<std::mutex> lock(c.frames);
    if (data->eventType == CAMERA_DRC_DETACH) {
        if (data->detach.handle == c.callback_handle) c.detached = true;
    } else if (data->eventType == CAMERA_DECODE_DONE && data->decode.handle == c.callback_handle) {
        for (int i = 0; i < wiiu::CameraQueue::COUNT; i++)
            if (data->decode.surfaceBuffer == c.surfaces[i].surfaceBuffer) c.queue.complete(i, data->decode.failed);
    }
}

bool load(Camera &c) {
    if (c.module) return true;
    if (OSDynLoad_Acquire("camera.rpl", &c.module) != OS_DYNLOAD_OK) return false;
    const struct { const char *name; void **where; } exports[] = {
        {"CAMInit", (void **)&c.init}, {"CAMExit", (void **)&c.exit}, {"CAMOpen", (void **)&c.open},
        {"CAMClose", (void **)&c.close}, {"CAMGetMemReq", (void **)&c.mem_req},
        {"CAMSubmitTargetSurface", (void **)&c.submit}, {"CAMCheckMemSegmentation", (void **)&c.check_mem}};
    for (const auto &e : exports) {
        if (OSDynLoad_FindExport(c.module, OS_DYNLOAD_EXPORT_FUNC, e.name, e.where) != OS_DYNLOAD_OK || !*e.where) {
            OSDynLoad_Release(c.module);
            c.module = 0;
            return false;
        }
    }
    return true;
}

void failed(Camera &c, int error) {
    c.retry_after = OSGetSystemTime() + OSMillisecondsToTicks(1000);
    if (error != c.last_error) LOG("[cam] GamePad camera unavailable (error %d)\n", error);
    c.last_error = error;
}

// The control mutex is held for every SDK call; callbacks take only the separate frame mutex.
void close(Camera &c) {
    {
        std::lock_guard<std::mutex> lock(c.frames);
        c.callback_handle = -1;
        c.queue.clear();
        c.detached = false;
    }
    if (c.handle >= 0) {
        c.close(c.handle);
        c.exit(c.handle);
        c.handle = -1;
    }
}

int submit_free(Camera &c) {
    int failed = CAMERA_ERROR_OK;
    for (int i = 0; i < wiiu::CameraQueue::COUNT; i++) {
        bool queued;
        {
            std::lock_guard<std::mutex> lock(c.frames);
            queued = c.queue.submit(i);
        }
        if (!queued) continue;
        DCInvalidateRange(c.surfaces[i].surfaceBuffer, CAMERA_YUV_BUFFER_SIZE);
        const auto error = c.submit(c.handle, &c.surfaces[i]);
        if (error != CAMERA_ERROR_OK) {
            failed = error;
            std::lock_guard<std::mutex> lock(c.frames);
            c.queue.complete(i, true);
        }
    }
    return failed;
}

bool open(Camera &c) {
    if (c.shutdown || !c.wanted || paused.load()) return false;
    bool detached;
    { std::lock_guard<std::mutex> lock(c.frames); detached = c.detached; }
    if (resume_needed.exchange(false) || detached) close(c);
    if (c.handle >= 0) return true;
    if (OSGetSystemTime() < c.retry_after) return false;
    if (!load(c)) { failed(c, CAMERA_ERROR_UNINITIALIZED); return false; }
    CAMSetupInfo info = {};
    info.streamInfo = {CAMERA_STREAM_TYPE_1, CAMERA_HEIGHT, CAMERA_WIDTH};
    info.mode = {CAMERA_FORCE_DRC_OFF, CAMERA_FPS_30};
    info.threadAffinity = OS_THREAD_ATTRIB_AFFINITY_ANY;
    info.eventHandler = event;
    const int bytes = c.mem_req(&info.streamInfo);
    if (bytes <= 0 || bytes > 32 * 1024 * 1024) { failed(c, CAMERA_ERROR_INVALID_ARG); return false; }
    if (c.work.pMem && c.work.size != (u32)bytes) { free(c.work.pMem); c.work = {}; }
    if (!c.work.pMem) c.work = {(u32)bytes, memalign(CAMERA_YUV_BUFFER_ALIGNMENT, bytes)};
    if (!c.work.pMem) { failed(c, CAMERA_ERROR_INSUFFICIENT_MEMORY); return false; }
    int error = c.check_mem(c.work.pMem, c.work.size);
    if (error != CAMERA_ERROR_OK) { failed(c, error); return false; }
    for (auto &surface : c.surfaces) {
        if (!surface.surfaceBuffer) surface = {CAMERA_YUV_BUFFER_SIZE, memalign(CAMERA_YUV_BUFFER_ALIGNMENT, CAMERA_YUV_BUFFER_SIZE),
            CAMERA_HEIGHT, CAMERA_WIDTH, CAMERA_PITCH, CAMERA_YUV_BUFFER_ALIGNMENT, 0, 0};
        if (!surface.surfaceBuffer) { failed(c, CAMERA_ERROR_INSUFFICIENT_MEMORY); return false; }
        error = c.check_mem(surface.surfaceBuffer, surface.surfaceSize);
        if (error != CAMERA_ERROR_OK) { failed(c, error); return false; }
        DCFlushRange(surface.surfaceBuffer, CAMERA_YUV_BUFFER_SIZE);
    }
    info.workMem = c.work;
    CAMError init_error = CAMERA_ERROR_OK;
    c.handle = c.init(0, &info, &init_error);
    if (c.handle < 0 || init_error != CAMERA_ERROR_OK) {
        close(c);
        failed(c, init_error ? init_error : CAMERA_ERROR_UNINITIALIZED);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(c.frames);
        c.callback_handle = c.handle;
    }
    error = c.open(c.handle);
    if (error != CAMERA_ERROR_OK) { close(c); failed(c, error); return false; }
    c.last_error = 0;
    error = submit_free(c);
    if (error != CAMERA_ERROR_OK) { close(c); failed(c, error); return false; }
    LOG("[cam] GamePad camera capturing 640x480 NV12 at 30 fps\n");
    return true;
}
}  // namespace

bool cam_host_start() {
    used.store(true);
    Camera &c = camera();
    std::lock_guard<std::mutex> lock(c.control);
    c.wanted = true;
    return open(c);
}

void cam_host_stop() {
    if (!used.load()) return;
    Camera &c = camera();
    std::lock_guard<std::mutex> lock(c.control);
    c.wanted = false;
    close(c);
}

bool cam_host_frame(u8 *out, u32 capacity) {
    if (!out || capacity < CAMERA_YUV_BUFFER_SIZE || !used.load()) return false;
    Camera &c = camera();
    std::lock_guard<std::mutex> lock(c.control);
    if (!open(c)) return false;
    int chosen;
    { std::lock_guard<std::mutex> frame_lock(c.frames); chosen = c.queue.acquire(); }
    if (chosen >= 0) {
        DCInvalidateRange(c.surfaces[chosen].surfaceBuffer, CAMERA_YUV_BUFFER_SIZE);
        memcpy(out, c.surfaces[chosen].surfaceBuffer, CAMERA_YUV_BUFFER_SIZE);
        std::lock_guard<std::mutex> frame_lock(c.frames);
        c.queue.release(chosen);
    }
    const int error = submit_free(c);
    if (error != CAMERA_ERROR_OK) { close(c); failed(c, error); }
    return chosen >= 0;
}

void wiiu::camera_pause(bool on) {
    // ProcUI's callback has a small stack; the service thread reopens after the SDK's automatic background close.
    paused.store(on);
    resume_needed.store(true);
}

void wiiu::camera_shutdown() {
    if (!used.load()) return;
    Camera &c = camera();
    std::lock_guard<std::mutex> lock(c.control);
    c.shutdown = true;
    c.wanted = false;
    close(c);
    for (auto &surface : c.surfaces) { free(surface.surfaceBuffer); surface = {}; }
    free(c.work.pMem);
    c.work = {};
    if (c.module) { OSDynLoad_Release(c.module); c.module = 0; }
}
