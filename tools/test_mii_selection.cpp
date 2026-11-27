#include "mii_select.h"
#include <cassert>
#include <cstdio>

int main() {
    using mii_select::initial_row;
    assert(initial_row({0, 99}, 99, 1) == 1);
    assert(initial_row({0, 99}, 17, 1) == 1);
    assert(initial_row({0, 99}, UINT32_MAX, 1) == 1);
    assert(initial_row({0, 99}, 0, 1) == 0);
    assert(initial_row({3, 40, 99, -1}, 40, 2) == 1);
    assert(initial_row({3, 40, 99, -1}, UINT32_MAX, -1) == 0);
    assert(initial_row({3, 40, 99, -1}, UINT32_MAX, 3) == 3);
    assert(initial_row({3, 40, 99, -1}, UINT32_MAX, 4) == 0);
    assert(initial_row({}, 0, 0) == 0);
    for (int slot = 1; slot < 100; ++slot) {
        assert(initial_row({0, slot}, slot, 0) == 1);
        assert(initial_row({0, slot}, 100, 1) == 1);
    }
    puts("Mii selection: sparse slots, personal fallback, invalid indices passed");
}
