#include <cassert>
#include <cstdint>
#include "camera_queue.h"

int main() {
    wiiu::CameraQueue queue;
    assert(queue.acquire() == -1);
    assert(queue.submit(0) && queue.submit(1));
    assert(!queue.submit(0));
    queue.complete(0, true);
    assert(queue.acquire() == -1 && queue.submit(0));
    queue.complete(0, false);
    queue.complete(1, false);
    assert(queue.acquire() == 1);  // discard the older snapshot
    assert(queue.state[0] == queue.FREE && queue.submit(0));
    assert(!queue.submit(1));  // DMA cannot overwrite a leased snapshot
    queue.complete(1, false);  // duplicate/stale callback cannot release the lease
    assert(queue.state[1] == queue.READING);
    queue.release(1);
    assert(queue.acquire() == -1);  // no synthetic fresh frame
    queue.clear();
    queue.complete(0, false);  // a callback after close does not create a frame
    assert(queue.acquire() == -1);
    queue.latest = UINT32_MAX - 1;
    assert(queue.submit(0) && queue.submit(1));
    queue.complete(0, false);
    queue.complete(1, false);
    assert(queue.acquire() == 1);  // sequence wrap still selects the newest
}
