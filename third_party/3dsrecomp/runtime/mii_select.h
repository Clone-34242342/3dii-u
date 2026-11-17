// The 3DS Mii selector applet: services/apt.cpp holds the game's request until the player picks one of the console's Miis or cancels; a
// port whose mii_select_shown() says yes shows the list and takes the input (no kernel types here). Otherwise a game gets the player's
// Mii at once.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mii_select {
inline int initial_row(const std::vector<int> &places, uint32_t requested, int personal) {
    auto at = requested < 100 ? std::find(places.begin(), places.end(), (int)requested) : places.end();
    if (at == places.end() && personal >= 0) at = std::find(places.begin(), places.end(), personal);
    return at == places.end() ? 0 : (int)(at - places.begin());
}
struct Request {
    std::string title;               // the game's line above the list, UTF-8 ("" for none)
    std::vector<std::string> names;  // the Miis to pick from, in Mii Maker's order, UTF-8
    int initial = 0;                 // the one picked at first
    bool can_cancel = false;
    unsigned serial = 0;             // changes with every request
};
bool pending(Request *out);  // (any thread) a request waits for the player
void finish(int index);      // (host thread, kernel lock not held) the Mii picked, or -1 to cancel
}  // namespace mii_select

bool mii_select_shown();  // a port defines it, saying whether it can show the list now
