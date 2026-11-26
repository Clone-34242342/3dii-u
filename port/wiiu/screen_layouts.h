// Where the 3DS's two screens go on the Wii U's TV and GamePad: the options menu's TV and GAMEPAD lines (settings.ini's tv_layout and
// pad_layout), which split the screens either way between the two or put both on one, as the Wii U's DS Virtual Console lets a player do.
// Only arithmetic, so tools/test_screen_layouts.cpp can check every layout on every screen size.
#pragma once
#include <algorithm>
#include <cstdint>

namespace screens {
struct Rect {
    uint32_t x, y, w, h;
};

// The TV's layouts as settings.ini numbers them: builds already given out hold 0-3, so those added later come after them, and the menu
// steps through them in its own order (kTvOrder): either screen alone, then both.
// 0 the top screen alone, 1 side by side, 2 the top above the bottom, 3 the top large and the bottom at half its scale beside its lower half,
// 4 the bottom screen alone, 5 the bottom large and the top at half its scale beside its upper half, 6 the top screen stretched to fill the
// TV, 16:9 (players asked, 2026-11-16)
constexpr int kTvLayouts = 7;
constexpr int kTvOrder[kTvLayouts] = {0, 6, 4, 1, 2, 3, 5};
inline int next_tv_layout(int layout, int dir) {
    int at = 0;
    while (at < kTvLayouts - 1 && kTvOrder[at] != layout) at++;
    return kTvOrder[(at + dir + kTvLayouts) % kTvLayouts];
}

// where screen `which` (0 the top, 1 the bottom) goes on a TV of tw x th; false where the layout leaves it off the TV
inline bool tv_rect(int layout, int which, uint32_t tw, uint32_t th, Rect &r) {
    auto fit = [&](float w, float h) { return std::min(tw / w, th / h); };  // the scale of a w x h arrangement that fills the TV one way
    switch (layout) {
    case 1: {  // side by side: 720x240
        const float k = fit(720, 240);
        const uint32_t x0 = (uint32_t)((tw - 720 * k) / 2), y = (uint32_t)((th - 240 * k) / 2);
        r = which == 0 ? Rect{x0, y, (uint32_t)(400 * k), (uint32_t)(240 * k)} : Rect{x0 + (uint32_t)(400 * k), y, (uint32_t)(320 * k), (uint32_t)(240 * k)};
        return true;
    }
    case 2: {  // the top screen above the bottom one: 400x480
        const float k = fit(400, 480);
        const uint32_t y0 = (uint32_t)((th - 480 * k) / 2);
        r = which == 0 ? Rect{(uint32_t)((tw - 400 * k) / 2), y0, (uint32_t)(400 * k), (uint32_t)(240 * k)}
                       : Rect{(uint32_t)((tw - 320 * k) / 2), y0 + (uint32_t)(240 * k), (uint32_t)(320 * k), (uint32_t)(240 * k)};
        return true;
    }
    case 3: {  // the top screen large, the bottom one at half its scale beside its lower half: 560x240
        const float k = fit(560, 240);
        const uint32_t x0 = (uint32_t)((tw - 560 * k) / 2), y = (uint32_t)((th - 240 * k) / 2);
        r = which == 0 ? Rect{x0, y, (uint32_t)(400 * k), (uint32_t)(240 * k)}
                       : Rect{x0 + (uint32_t)(400 * k), y + (uint32_t)(120 * k), (uint32_t)(160 * k), (uint32_t)(120 * k)};
        return true;
    }
    case 6:  // the top screen alone, filling the TV
        if (which) return false;
        r = Rect{0, 0, tw, th};
        return true;
    case 4:  // the bottom screen alone, as tall as the TV
        if (!which) return false;
        r = Rect{(tw - th * 4 / 3) / 2, 0, th * 4 / 3, th};
        return true;
    case 5: {  // the bottom screen large, the top one at half its scale beside its upper half: 520x240
        const float k = fit(520, 240);
        const uint32_t x0 = (uint32_t)((tw - 520 * k) / 2), y = (uint32_t)((th - 240 * k) / 2);
        r = which == 1 ? Rect{x0, y, (uint32_t)(320 * k), (uint32_t)(240 * k)} : Rect{x0 + (uint32_t)(320 * k), y, (uint32_t)(200 * k), (uint32_t)(120 * k)};
        return true;
    }
    default:  // the top screen alone, as tall as the TV
        if (which) return false;
        r = Rect{(tw - th * 5 / 3) / 2, 0, th * 5 / 3, th};
        return true;
    }
}

// The GamePad's layouts: 0 the bottom screen, which it touches, 1 the top screen, 2 both at the 3DS's own size, the top above the bottom,
// 3 both side by side, as large as they fit, 4 the top screen stretched to fill the GamePad. Where screen `which` goes on a GamePad of dw x dh (854x480), and the part of the GamePad that
// screen alone has, which is painted around it when it's drawn (black, or the game's bezel picture); false where the layout leaves it off.
constexpr int kPadLayouts = 5;
constexpr int kPadOrder[kPadLayouts] = {0, 1, 4, 2, 3};
inline int next_pad_layout(int layout, int dir) {
    int at = 0;
    while (at < kPadLayouts - 1 && kPadOrder[at] != layout) at++;
    return kPadOrder[(at + dir + kPadLayouts) % kPadLayouts];
}
inline bool pad_rect(int layout, int which, uint32_t dw, uint32_t dh, Rect &r, Rect &own) {
    switch (layout) {
    case 4:  // the top screen alone, filling the GamePad
        if (which) return false;
        r = own = Rect{0, 0, dw, dh};
        return true;
    case 1:  // the top screen, twice the 3DS's size
        if (which) return false;
        r = Rect{(dw - 800) / 2, (dh - 480) / 2, 800, 480};
        own = Rect{0, 0, dw, dh};
        return true;
    case 2:  // both at the 3DS's own size
        r = which ? Rect{(dw - 320) / 2, dh / 2, 320, 240} : Rect{(dw - 400) / 2, dh / 2 - 240, 400, 240};
        own = which ? Rect{0, dh / 2, dw, dh - dh / 2} : Rect{0, 0, dw, dh / 2};
        return true;
    case 3: {  // side by side, as the TV's layout 1: 720x240 scaled to the GamePad's width
        const float k = std::min(dw / 720.0f, dh / 240.0f);
        const uint32_t tw = (uint32_t)(400 * k), bw = (uint32_t)(320 * k), h = (uint32_t)(240 * k), x0 = (dw - tw - bw) / 2, y = (dh - h) / 2;
        r = which ? Rect{x0 + tw, y, bw, h} : Rect{x0, y, tw, h};
        own = which ? Rect{x0 + tw, 0, dw - x0 - tw, dh} : Rect{0, 0, x0 + tw, dh};
        return true;
    }
    default:  // the bottom screen, twice the 3DS's size
        if (!which) return false;
        r = Rect{(dw - 640) / 2, (dh - 480) / 2, 640, 480};
        own = Rect{0, 0, dw, dh};
        return true;
    }
}

// a touch at GamePad pixel (x, y) as the bottom screen's point (tx, ty) in its 320x240; false where the GamePad doesn't show the bottom screen
inline bool pad_touch(int layout, uint32_t x, uint32_t y, uint32_t dw, uint32_t dh, uint32_t &tx, uint32_t &ty) {
    Rect r, own;
    if (!pad_rect(layout, 1, dw, dh, r, own) || x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return false;
    tx = (x - r.x) * 320 / r.w;
    ty = (y - r.y) * 240 / r.h;
    return true;
}
}  // namespace screens
