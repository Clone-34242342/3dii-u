// The 3DS Mii selector applet: services/apt.cpp holds the game's request until the player picks one of the console's Miis or cancels; a
// port whose mii_select_shown() says yes shows the list and takes the input (no kernel types here). Otherwise a game gets the player's
// Mii at once.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mii_select {
inline int initial_row(const std::vector<int> &places, uint32_t requested, int personal_row) {
    const auto at = requested < 100 ? std::find(places.begin(), places.end(), (int)requested) : places.end();
    if (at != places.end()) return (int)(at - places.begin());
    return personal_row >= 0 && (size_t)personal_row < places.size() ? personal_row : 0;
}
struct Request {
    struct Entry {
        std::string name;
        std::array<uint8_t, 0x60> store_data;
    };
    std::string title;               // the game's line above the list, UTF-8 ("" for none)
    std::vector<Entry> entries;       // in Mii Maker's order, with 3DS-order store data
    int initial = 0;                 // the one picked at first
    bool can_cancel = false;
};
std::shared_ptr<const Request> pending();         // (any thread) the request waiting for the player
void finish(const Request &request, int index);   // (host thread, kernel lock not held) the Mii picked, or -1 to cancel
}  // namespace mii_select

bool mii_select_shown();  // a port defines it, saying whether it can show the list now
