#include "mii_select.h"
#include <array>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
#define LOG(...) ((void)0)

static bool show = true;
bool mii_select_shown() { return show; }

namespace kernel {
using Mii = std::array<u8, 0x60>;
using Lock = std::lock_guard<std::mutex>;
static std::mutex kmutex;
static std::vector<Mii> miis(3012);
static std::vector<int> database_slots(miis.size(), -1);
static std::vector<u8> applet_reply;
static int personal = 1;

struct Parameter {
    u32 id, dest, signal;
    void *object;
    std::vector<u8> data;
};
void send_parameter(Parameter p) {
    assert(p.dest == 0x300 && p.signal == 10);
    applet_reply = std::move(p.data);
}
const std::vector<Mii> &console_miis(int *mine) { *mine = personal; return miis; }
Mii console_mii(u32 row) { return miis.at(row); }
Mii player_mii() { return miis.at(personal); }
int mii_maker_place(u32 row) { return database_slots.at(row); }
void run_when_quiescent(auto f) { f(); }
void schedule_async() {}

#include "mii_applet.inc"
} // namespace kernel
#include "mii_requests.inc"

enum : u32 {
    VPAD_BUTTON_UP = 1, VPAD_BUTTON_DOWN = 2, VPAD_BUTTON_LEFT = 4, VPAD_BUTTON_RIGHT = 8,
    VPAD_BUTTON_A = 16, VPAD_BUTTON_B = 32, VPAD_BUTTON_L = 64, VPAD_BUTTON_R = 128, VPAD_BUTTON_Y = 256
};
#include "mii_input.inc"

static void reply(bool selected, unsigned id = 0) {
    const auto &r = kernel::applet_reply;
    assert(r.size() == 0x84 && r[0] == (selected ? 0 : 1));
    assert(r[4] == 0 && r[8] == 0xFF && r[11] == 0xFF);
    if (selected) assert((r[0x0C + 0x0C] << 8 | r[0x0C + 0x0D]) == id);
    kernel::applet_reply.clear();
}

int main() {
    for (unsigned i = 0; i < kernel::miis.size(); ++i) {
        kernel::miis[i][0x0C] = u8(i >> 8);
        kernel::miis[i][0x0D] = u8(i);
        kernel::miis[i][0x1A] = 'M';
    }
    kernel::database_slots[4] = 0;
    kernel::database_slots[1] = 7;
    kernel::database_slots[9] = 99;
    kernel::miis[4][0x1A] = 'Z';
    kernel::miis[1][0x1A] = 'A';
    kernel::miis[9][0x1A] = 0x55;  // さ
    kernel::miis[9][0x1B] = 0x30;

    std::vector<u8> cfg(0x104);
    std::fill(cfg.begin() + 0x9A, cfg.begin() + 0xFE, 1);
    cfg[0x90] = 99; // preselect a sparse guest database slot
    kernel::mii_select_run(0x402, cfg);
    auto request = mii_select::pending();
    assert(request && request->entries.size() == 3012 && request->initial == 2);
    assert(request->entries[2].name == "\xE3\x81\x95");
    assert(request->entries[2].store_data == kernel::miis[9]);
    mii_selector_input(0, false, 0, 0);
    assert(mii_selector_open() && ms.sel == 2 && ms.top() == 0);
    mii_selector_input(VPAD_BUTTON_B, false, 0, 0);
    assert(mii_selector_open() && kernel::applet_reply.empty());
    mii_selector_input(VPAD_BUTTON_RIGHT, false, 0, 0);
    assert(ms.sel == 3 && ms.top() == 0);
    mii_selector_input(VPAD_BUTTON_DOWN, false, 0, 0);
    assert(ms.sel == 8 && ms.top() == 0);
    mii_selector_input(VPAD_BUTTON_UP, false, 0, 0);
    mii_selector_input(VPAD_BUTTON_LEFT, false, 0, 0);
    assert(ms.sel == 2 && ms.top() == 0);
    mii_selector_input(VPAD_BUTTON_UP, false, 0, 0);
    assert(ms.sel == 2 && ms.top() == 0);
    ms_presented_layout = ms.layout;
    mii_selector_input(VPAD_BUTTON_R, false, 0, 0);
    assert(ms.sel == 12 && ms.top() == 10);
    mii_selector_input(0, true, MS_X + 2, MS_Y + 2);
    assert(mii_selector_open() && kernel::applet_reply.empty()); // the previous page is still on screen
    mii_selector_input(0, true, MS_L + 10, MS_FOOTER + 10);
    assert(ms.sel == 2 && ms.top() == 0);
    for (int i = 0; i < 380; ++i) mii_selector_input(VPAD_BUTTON_R, false, 0, 0);
    assert(ms.sel == 3011 && ms.top() == 3010);
    mii_selector_input(VPAD_BUTTON_DOWN, false, 0, 0);
    assert(ms.sel == 3011);
    ms_presented_layout = ms.layout;
    mii_selector_input(0, true, MS_X + 2 * (MS_W + MS_GAP) + 2, MS_Y + 2);
    mii_selector_input(0, true, MS_X + MS_W + 2, MS_Y + 2);
    assert(mii_selector_open() && kernel::applet_reply.empty()); // empty cells and gaps don't choose
    mii_selector_input(0, true, MS_X + MS_W + MS_GAP + 2, MS_Y + 2);
    assert(!mii_selector_open() && !mii_select::pending());
    reply(true, 3011);

    cfg[0] = 1;
    cfg[0x9A + 99] = 0;
    kernel::mii_select_run(0x402, cfg);
    const auto restricted = mii_select::pending();
    assert(restricted && restricted->entries.size() == 2 && restricted->initial == 1);
    mii_select::finish(*request, 0);
    assert(mii_select::pending() == restricted); // completion from an old rendered request
    mii_selector_input(0, false, 0, 0);
    mii_selector_input(VPAD_BUTTON_Y, false, 0, 0);
    assert(ms.sel == 0 && ms.row(ms.sel) == 1);
    mii_selector_input(VPAD_BUTTON_Y, false, 0, 0);
    assert(ms.sel == 1 && ms.row(ms.sel) == 1);
    mii_selector_input(VPAD_BUTTON_B, false, 0, 0);
    reply(false);

    kernel::mii_select_run(0x402, cfg);
    mii_selector_input(0, false, 0, 0);
    mii_selector_input(VPAD_BUTTON_Y, false, 0, 0);
    ms_presented_layout = ms.layout;
    mii_selector_input(0, true, MS_X + MS_W + MS_GAP + 2, MS_Y + 2);
    reply(true, 4); // sorting moves the tiles without changing who the game receives

    show = false;
    kernel::mii_select_run(0x402, cfg);
    reply(true, 1); // a controller-only fallback respects the whitelist
    cfg[0x9A] = cfg[0x9A + 7] = 0;
    kernel::mii_select_run(0x402, cfg);
    reply(false);

    show = true;
    cfg.resize(0x94); // older shortened requests lack a whitelist
    cfg[0x90] = 17;
    kernel::database_slots[1] = -1;
    kernel::mii_select_run(0x402, cfg);
    assert(mii_select::pending()->entries.size() == 3012);
    mii_selector_input(0, false, 0, 0);
    mii_selector_input(VPAD_BUTTON_A, false, 0, 0);
    reply(true, 1);
    puts("Mii applet whitelist, grid browsing, sorting, UTF-8 names, cancel and request identity passed");
}
