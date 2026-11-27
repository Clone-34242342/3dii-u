// Headless sink for DSP regression tests; --play belongs to the upstream SDL test build.
#include "audio/host.h"
namespace audio {
void init() {}
namespace hle {
void host_output(const Sample *, size_t) {}
int host_queued() { return 0; }
}
}
namespace mem {
u8 *paddr_ptr(u32 pa) {
    if (pa >= FCRAM_PADDR && pa - FCRAM_PADDR < LINEAR_SIZE) return g_mem + LINEAR_BASE + (pa - FCRAM_PADDR);
    if (pa >= VRAM_PADDR && pa - VRAM_PADDR < VRAM_SIZE) return g_mem + VRAM_BASE + (pa - VRAM_PADDR);
    if (pa >= DSP_PADDR && pa - DSP_PADDR < DSP_SIZE) return g_mem + DSP_BASE + (pa - DSP_PADDR);
    return nullptr;
}
}
