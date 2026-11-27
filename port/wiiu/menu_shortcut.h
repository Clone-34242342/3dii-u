#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace menu_shortcut {
struct Button {
    std::string_view name;
    std::uint32_t mask;
};

inline std::uint32_t parse(std::string_view text, std::span<const Button> buttons) {
    const auto separator = text.find('+');
    if (separator == text.npos) return 0;
    const auto lookup = [&](std::string_view name) {
        for (const auto &button : buttons)
            if (button.name == name) return button.mask;
        return std::uint32_t{0};
    };
    const auto first = lookup(text.substr(0, separator)), second = lookup(text.substr(separator + 1));
    return first && second && first != second ? first | second : 0;
}

inline bool pressed(std::uint32_t combo, std::uint32_t down, std::uint32_t held) {
    return (held & combo) == combo && (down & combo) != 0;
}
}  // namespace menu_shortcut
