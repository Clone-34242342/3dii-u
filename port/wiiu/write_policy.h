#pragma once
#include <string_view>

namespace wiiu {
// FSA receives absolute paths without the devoptab's "fs:" prefix.
inline bool writable_path(const char *name) {
    if (!name) return false;
    const std::string_view path(name);
    if (path.empty() || path.front() != '/') return false;
    for (char c : path) if (c == '\\' || c == ':' || (unsigned char)c < 0x20) return false;
    for (size_t at = 1; at < path.size();) {
        const size_t end = path.find('/', at);
        const auto part = path.substr(at, end == path.npos ? path.size() - at : end - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        at = end + 1;
    }
    constexpr std::string_view sd = "/vol/external01/3dii-u";
    if (path == sd || (path.size() > sd.size() && path.substr(0, sd.size()) == sd && path[sd.size()] == '/')) return true;
    constexpr std::string_view checkpoint = "/vol/external01/3ds/Checkpoint";
    if (path == checkpoint || (path.size() > checkpoint.size() && path.substr(0, checkpoint.size()) == checkpoint && path[checkpoint.size()] == '/')) return true;
    constexpr std::string_view save = "/vol/save/";
    if (path.substr(0, save.size()) != save) return false;
    const auto rest = path.substr(save.size());
    const auto account = rest.substr(0, rest.find('/'));
    if (account == "common") return true;
    if (account.size() != 8) return false;
    for (char c : account) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

inline bool readonly_mode(const char *mode) {
    return mode && (std::string_view(mode) == "r" || std::string_view(mode) == "rb");
}
}  // namespace wiiu
