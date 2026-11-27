#pragma once

#include <cstring>
#include "../service.h"

namespace kernel {
inline void get_wireless_reboot_info(IPC &ipc) {
    const u32 size = ipc[1];
    u32 capacity;
    const u32 dst = ipc.static_buffer(0, &capacity);
    if (size > 0x10 || size > capacity) return ipc.result(RES_INVALID_COMBINATION);
    if (size && !guest_writable(dst, size)) return ipc.result(RES_INVALID_ADDRESS);
    // A boot without a wireless handoff gets an empty record, even if the guest reused the buffer.
    if (size) std::memset(gptr(dst), 0, size);
    VLOG("[apt] wireless reboot info: no pending session (%u bytes)\n", size);
    ipc.ok(1, 2);
    ipc.static_desc(2, 0, size);
}
}  // namespace kernel
