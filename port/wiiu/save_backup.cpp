// Checkpoint transports raw save files; pairing metadata stays outside those files.
#include "save_backup.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#ifndef SAVE_BACKUP_TEST
#include "cloud.h"
#include "kernel.h"
#include "loader.h"
#include "wiiu.h"
#endif

namespace wiiu::save_backup {
namespace {
namespace fs = std::filesystem;
using Path = fs::path;
struct Context { Path save, card; uint64_t id; std::string title; };
constexpr const char *parts[] = {"savedata", "extdata"};
#ifdef O_BINARY
constexpr int create_file = O_WRONLY | O_CREAT | O_EXCL | O_BINARY;
#else
constexpr int create_file = O_WRONLY | O_CREAT | O_EXCL;
#endif

template<class Run> bool snapshot(Run run) {
    // FS mutations hold kmutex; Write unlocks it but releases save_lock before reacquiring it.
    // This order drains in-flight writes without waiting on a writer that needs our kernel lock.
    kernel::Lock lk(kernel::kmutex);
    save_lock();
    fs_begin(0xC010);  // recursive OSMutex: save_flush may enter again while draining cached writes
    const bool ok = run();
    fs_end();
    save_unlock();
    return ok;
}

bool permit(const char *op, const Path &from, const Path &to = {}) {
#ifdef SAVE_BACKUP_TEST
    return test_allow(op, from, to);
#else
    return true;
#endif
}
bool flush() {
#ifdef SAVE_BACKUP_TEST
    return test_flush();
#else
    save_dirty();
    save_flush(true);
    return !save_pending();
#endif
}

std::string title_id(const Context &c) {
    char s[17];
    snprintf(s, sizeof s, "%016llx", (unsigned long long)c.id);
    return s;
}
std::string folder_name(const Context &c) {
    char head[16];
    snprintf(head, sizeof head, "0x%05X ", (unsigned)((c.id >> 8) & 0xfffff));
    std::string s = head;
    for (unsigned char ch : c.title) s += ch < 32 || std::string(":/\\*?\"<>|").find(ch) != std::string::npos ? ' ' : ch;
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s;
}
bool backup_name(const std::string &s) {
    if (s.size() < 15 || s.size() > 96 || s[8] != '-' || s.back() == ' ' || s.back() == '.') return false;
    for (size_t i = 0; i < 15; ++i) if (i != 8 && (s[i] < '0' || s[i] > '9')) return false;
    for (unsigned char ch : s) if (ch < 32 || ch > 126 || std::string(":/\\*?\"<>|").find(ch) != std::string::npos) return false;
    return true;
}
bool ext_id(const std::string &s) {
    return s.size() == 16 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Path game(const Context &c, const char *kind) { return c.card / "3ds" / "Checkpoint" / kind / folder_name(c); }
Path transaction(const Context &c) { return c.save / ".checkpoint-restore"; }

// Inspect lexical ancestors before following anything; FAT has no links, PC tests do.
bool checked(const Path &p) {
    for (const auto &part : p) if (part == ".." || part == ".") return false;
    for (Path at = p; !at.empty(); ) {
        std::error_code ec;
        const auto st = fs::symlink_status(at, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return false;
        if (!ec && fs::exists(st)) {
            if (!fs::is_directory(st) && !fs::is_regular_file(st)) return false;
            if (at != p && !fs::is_directory(st)) return false;
#ifdef _WIN32
            if (GetFileAttributesW(at.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return false;
#endif
            if (fs::is_regular_file(st)) {
                if (fs::hard_link_count(at, ec) > 1 || ec) return false;
            }
        }
        const Path parent = at.parent_path();
        if (parent == at) break;
        at = parent;
    }
    return true;
}
bool present(const Path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool directory(const Path &p) {
    std::error_code ec;
    return checked(p) && fs::is_directory(p, ec) && !ec;
}
bool empty_directory(const Path &p) {
    std::error_code ec;
    return fs::is_empty(p, ec) && !ec;
}
bool tree(const Path &p) {
    if (!checked(p)) return false;
    if (!directory(p)) return present(p);
    std::error_code ec;
    fs::directory_iterator it(p, ec), end;
    while (!ec && it != end) {
        if (!tree(it->path())) return false;
        it.increment(ec);
    }
    return !ec;
}
bool mkdirs(const Path &p) {
    if (!checked(p) || !permit("mkdir", p)) return false;
    std::error_code ec;
    fs::create_directories(p, ec);
    return !ec;
}
bool erase(const Path &p) {
    if (!checked(p) || (present(p) && !tree(p)) || !permit("erase", p)) return false;
    std::error_code ec;
    fs::remove_all(p, ec);
    return !ec;
}
bool move(const Path &a, const Path &b) {
    if (!checked(a) || !checked(b) || present(b) || !permit("rename", a, b)) return false;
    std::error_code ec;
    fs::rename(a, b, ec);
    return !ec;
}
bool put(const Path &p, const std::string &s) {
    if (!checked(p) || !permit("write", p)) return false;
    const int fd = open(p.string().c_str(), create_file, 0600);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "wb");
    if (!f) { close(fd); return false; }
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    if (fclose(f)) ok = false;
    if (!ok) erase(p);
    return ok;
}
bool publish(const Path &p, const std::string &s) {
    const Path temp = p.string() + ".tmp";
    if (!put(temp, s)) return false;
    const bool ok = move(temp, p);
    if (!ok) erase(temp);
    return ok;
}
bool read(const Path &p, std::string &s) {
    if (!checked(p)) return false;
    FILE *f = fopen(p.string().c_str(), "rb");
    if (!f) return false;
    char buf[4097];
    const size_t n = fread(buf, 1, sizeof buf, f);
    const bool ok = !ferror(f) && n < sizeof buf;
    fclose(f);
    if (ok) s.assign(buf, n);
    return ok;
}
bool copy_tree(const Path &a, const Path &b) {
    if (!directory(a) || !checked(b) || present(b) || !mkdirs(b)) return false;
    std::error_code ec;
    fs::directory_iterator it(a, ec), end;
    std::vector<char> buffer(64 * 1024);
    while (!ec && it != end) {
        const Path src = it->path(), dst = b / src.filename();
        if (!checked(src)) return false;
        if (directory(src)) {
            if (!copy_tree(src, dst)) return false;
        } else {
            if (!permit("copy", src, dst)) return false;
            FILE *in = fopen(src.string().c_str(), "rb");
            if (!in) return false;
            const int fd = open(dst.string().c_str(), create_file, 0600);
            FILE *out = fd < 0 ? nullptr : fdopen(fd, "wb");
            if (!out && fd >= 0) close(fd);
            bool ok = out != nullptr;
            while (ok) {
                const size_t n = fread(buffer.data(), 1, buffer.size(), in);
                if (n && fwrite(buffer.data(), 1, n, out) != n) ok = false;
                if (n < buffer.size()) break;
            }
            ok = ok && !ferror(in);
            fclose(in);
            if (out && fclose(out)) ok = false;
            if (!ok) return false;
        }
        it.increment(ec);
    }
    return !ec;
}
std::string record(const Context &c, const std::string &name) { return "3DIIU1\n" + title_id(c) + "\n" + name + "\n"; }
bool selection(const Context &c, const std::string &s, std::string &name) {
    const std::string prefix = "3DIIU1\n" + title_id(c) + "\n";
    if (s.compare(0, prefix.size(), prefix) || s.size() <= prefix.size() || s.back() != '\n') return false;
    name = s.substr(prefix.size(), s.size() - prefix.size() - 1);
    return backup_name(name);
}
bool list_ids(const Path &p, std::vector<std::string> &ids) {
    if (!checked(p)) return false;
    if (!present(p)) return true;
    if (!directory(p)) return false;
    std::error_code ec;
    fs::directory_iterator it(p, ec), end;
    while (!ec && it != end) {
        const std::string id = it->path().filename().string();
        if (!ext_id(id) || !directory(it->path())) return false;
        ids.push_back(id);
        it.increment(ec);
    }
    std::sort(ids.begin(), ids.end());
    return !ec;
}

bool export_backup(const Context &c, time_t now, std::string &message) {
    message = "The backup could not be completed; existing backups were kept.";
    if (!directory(c.card)) {
        message = "Insert an SD card; no backup was written.";
        return false;
    }
    if (!directory(c.save / "savedata") || !flush() || !mkdirs(c.card / "3ds")) return false;
    std::vector<std::string> ids;
    if (!list_ids(c.save / "extdata", ids)) return false;
    if (ids.empty() && empty_directory(c.save / "savedata")) return false;
    char stamp[32];
    const auto *tm = localtime(&now);
    if (!tm || !strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tm)) return false;
    const Path saves = game(c, "saves"), extra = game(c, "extdata"), meta = game(c, ".3dii-u");
    if (!mkdirs(saves) || !mkdirs(extra) || !mkdirs(meta)) return false;
    std::string name;
    Path temp;
    bool reserved = false;
    for (unsigned i = 0; i < 1000; ++i) {
        char suffix[16] = "";
        if (i) snprintf(suffix, sizeof suffix, "-%03u", i);
        name = std::string(stamp) + suffix;
        temp = meta / ("." + name + ".partial");
        if (!checked(temp) || !checked(saves/name) || !checked(extra/name) || !checked(meta/name)) return false;
        if (present(saves/name) || present(extra/name) || present(meta/name) || present(temp)) continue;
        std::error_code ec;
        if (fs::create_directory(temp, ec)) { reserved = true; break; }
        if (ec || i == 999) return false;
    }
    if (!reserved) return false;
    bool ok = copy_tree(c.save / "savedata", temp / "save") && mkdirs(temp / "extra") && mkdirs(temp / "meta");
    std::string pairing = record(c, name);
    for (const auto &id : ids) {
        pairing += id + "\n";
        if (ok) ok = copy_tree(c.save / "extdata" / id, temp / "extra" / (ids.size() == 1 ? "data" : id));
    }
    if (ok) ok = put(temp / "meta" / "pair.txt", pairing);
    // Save is published last: an incomplete pair cannot be selected by newest().
    if (ok) ok = move(ids.size() == 1 ? temp / "extra" / "data" : temp / "extra", extra / name);
    if (ok) ok = move(temp / "meta", meta / name);
    if (ok) ok = move(temp / "save", saves / name);
    erase(temp);  // only our exclusively-created staging directory; orphan pairs remain unselectable
    if (ok) message = "Backup saved to " + (saves / name).string();
    return ok;
}

bool stage(const Context &c, std::string &message) {
    message = "No restore was staged. Check the SD card and any pending restore.";
    if (!directory(c.card) || !directory(c.save) || !checked(c.save / "save_restore") || present(c.save / "save_restore") || present(transaction(c) / "ready")) return false;
    const Path saves = game(c, "saves");
    if (!directory(saves)) return false;
    std::error_code ec;
    std::string newest;
    fs::directory_iterator it(saves, ec), end;
    while (!ec && it != end) {
        const std::string name = it->path().filename().string();
        if (backup_name(name) && directory(it->path()) && name > newest) newest = name;
        it.increment(ec);
    }
    if (ec || newest.empty()) return false;
    if (!publish(c.save / "save_restore", record(c, newest)) || !flush()) return false;
    message = "Backup " + newest + " is pending for the next start.";
    return true;
}

struct Plan { std::string name; unsigned selected = 1, original = 0, after = 1; };
std::string plan_record(const Context &c, const Plan &p) {
    return record(c, p.name) + std::to_string(p.selected) + " " + std::to_string(p.original) + " " + std::to_string(p.after) + "\n";
}
bool read_plan(const Context &c, const Path &path, Plan &p) {
    std::string s;
    if (!read(path, s) || s.size() < 6) return false;
    const size_t tail = s.size() - 6;
    if (!selection(c, s.substr(0, tail), p.name) || s[tail+1] != ' ' || s[tail+3] != ' ' || s[tail+5] != '\n') return false;
    p.selected = s[tail] - '0'; p.original = s[tail+2] - '0'; p.after = s[tail+4] - '0';
    return (p.selected == 1 || p.selected == 3) && !(p.original & ~p.selected) && (p.after == 1 || (p.after == 3 && p.selected == 3));
}
bool rollback(const Context &c, const Plan &p) {
    const Path tx = transaction(c);
    for (int i = 1; i >= 0; --i) if (p.selected & (1u << i)) {
        const Path live = c.save / parts[i], old = tx / "previous" / parts[i], next = tx / "next" / parts[i];
        if (!checked(live) || !checked(old) || !checked(next)) return false;
        if (present(old)) {
            if (!(p.original & (1u << i)) || !directory(old)) return false;
            if (present(live) && (!move(live, next) || !flush())) return false;
            if (!move(old, live) || !flush()) return false;
        } else if (p.original & (1u << i)) {
            if (!directory(live) || present(next) != bool(p.after & (1u << i))) return false;
        } else if (present(live) && (!move(live, next) || !flush())) return false;
    }
    return erase(tx / "ready") && flush();
}
bool finish(const Context &c) {
    const Path imported = c.save / "save_imported";
    // An explicit restore must not be replaced by a bundled save at a later startup.
    if (!checked(imported) || (!present(imported) && !publish(imported, "Checkpoint restore\n")) || !flush()) return false;
    return erase(c.save / "save_restore") && flush() && erase(transaction(c) / "ready") && flush();
}

Restore take(const Context &c, std::string &message) {
    message.clear();
    const Path tx = transaction(c), request = c.save / "save_restore";
    if (!checked(c.save) || !checked(tx)) {
        message = "Unsafe restore storage path; no files changed.";
        return Restore::Unsafe;
    }
    Plan prior;
    if (present(tx)) {
        std::string owner;
        if (!tree(tx) || !read(tx / "owner", owner) || owner != title_id(c) + "\n") {
            message = "Unrecognized restore transaction; leave its files intact for recovery.";
            return present(tx / "ready") || present(tx / "previous") ? Restore::Unsafe : Restore::Deferred;
        }
        if (!present(tx / "ready")) {
            std::error_code ec;
            const bool old_files = present(tx / "previous") && !fs::is_empty(tx / "previous", ec);
            if (ec || (old_files && !read_plan(c, tx / "committed", prior))) {
                message = "The previous save has no valid recovery record; leave it intact.";
                return Restore::Unsafe;
            }
        }
        if (present(tx / "ready")) {
            if (!read_plan(c, tx / "ready", prior)) {
                message = "Invalid recovery record; save consistency could not be verified.";
                return Restore::Unsafe;
            }
            std::string committed;
            if (present(tx / "committed")) {
                if (!read(tx / "committed", committed) || committed != plan_record(c, prior)) return Restore::Unsafe;
                for (unsigned i = 0; i < 2; ++i) if (prior.selected & (1u << i)) {
                    // Deferred confirmation cleanup allows play; the live trees may have changed since commit.
                    if (present(tx / "previous" / parts[i]) != bool(prior.original & (1u << i)) || present(tx / "next" / parts[i])) {
                        message = "The completed restore is inconsistent; leave recovery files intact.";
                        return Restore::Unsafe;
                    }
                }
                std::string pending;
                const bool requested = present(request);
                if (!requested || (read(request, pending) && pending == record(c, prior.name))) {
                    if (!finish(c)) return Restore::Deferred;
                    message = "Backup restored. The previous save pair is retained in save storage.";
                    return requested ? Restore::Restored : Restore::None;
                }
                message = "The completed restore has an unexpected pending request; leave it intact.";
                return Restore::Deferred;
            } else if (!rollback(c, prior)) {
                message = "Restore recovery is incomplete; do not start the game or remove recovery files.";
                return Restore::Unsafe;
            }
        }
    }
    // A bad request must not bypass recovery of an already-started pair transaction.
    if (!checked(request)) {
        message = "The pending restore is not an ordinary request file; the current save was kept.";
        return Restore::Deferred;
    }
    if (!present(request)) return Restore::None;
    message = "Restore is still pending; the current save was kept.";
    Plan p;
    std::string pending;
    if (!read(request, pending) || !selection(c, pending, p.name) || !directory(c.card)) return Restore::Deferred;
    const Path save = game(c, "saves") / p.name, extra = game(c, "extdata") / p.name, meta = game(c, ".3dii-u") / p.name;
    if (!directory(save) || !tree(save) || !directory(c.save / "savedata")) return Restore::Deferred;
    std::vector<std::string> ids;
    if (!checked(meta)) return Restore::Deferred;
    if (present(meta)) {
        std::string pairing;
        const std::string prefix = record(c, p.name);
        if (!read(meta / "pair.txt", pairing) || pairing.compare(0, prefix.size(), prefix)) return Restore::Deferred;
        for (size_t pos = prefix.size(); pos < pairing.size();) {
            const size_t end = pairing.find('\n', pos);
            if (end == std::string::npos) return Restore::Deferred;
            std::string id = pairing.substr(pos, end - pos);
            if (!ext_id(id) || (!ids.empty() && id <= ids.back())) return Restore::Deferred;
            ids.push_back(id);
            pos = end + 1;
        }
        if (!directory(extra) || !tree(extra)) return Restore::Deferred;
        p.selected = 3;
        p.after = ids.empty() ? 1 : 3;
    } else if (empty_directory(save)) return Restore::Deferred;
    for (unsigned i = 0; i < 2; ++i) if (p.selected & (1u << i)) {
        const Path live = c.save / parts[i];
        if (!checked(live) || (present(live) && (!directory(live) || !tree(live)))) return Restore::Deferred;
        if (present(live)) p.original |= 1u << i;
    }
    // A completed previous transaction is replaced only after validating the new source.
    if (!erase(tx) || !mkdirs(tx)) return Restore::Deferred;
    if (!put(tx / "owner", title_id(c) + "\n")) { erase(tx); return Restore::Deferred; }
    if (!mkdirs(tx / "next") || !mkdirs(tx / "previous")) return Restore::Deferred;
    bool ok = copy_tree(save, tx / "next" / "savedata");
    if (ok && !ids.empty()) ok = mkdirs(tx / "next" / "extdata");
    for (const auto &id : ids) if (ok) ok = copy_tree(ids.size() == 1 ? extra : extra / id, tx / "next" / "extdata" / id);
    if (!ok || !publish(tx / "ready", plan_record(c, p)) || !flush()) return Restore::Deferred;
    for (unsigned i = 0; ok && i < 2; ++i) if (p.selected & (1u << i)) {
        if (p.original & (1u << i)) ok = move(c.save / parts[i], tx / "previous" / parts[i]) && flush();
        if (ok && (p.after & (1u << i))) ok = move(tx / "next" / parts[i], c.save / parts[i]) && flush();
    }
    if (ok) ok = publish(tx / "committed", plan_record(c, p)) && flush();
    if (!ok) {
        // A partially written commit is not authoritative, even if all renames completed.
        if (!erase(tx / "committed") || !rollback(c, p)) {
            message = "Restore recovery is incomplete; do not start the game or remove recovery files.";
            return Restore::Unsafe;
        }
        return Restore::Deferred;
    }
    message = "Backup restored; confirmation cleanup is pending.";
    if (!finish(c)) return Restore::Deferred;
    message = "Backup restored. The previous save pair is retained in save storage.";
    return Restore::Restored;
}
}  // namespace

#ifndef SAVE_BACKUP_TEST
static Context current() { return {kernel::save_root(), "fs:/vol/external01", g_game.program_id, g_game.name}; }
bool export_to_card(std::string &message) { return snapshot([&] { return export_backup(current(), time(nullptr), message); }); }
bool stage_restore(std::string &message) { return snapshot([&] { return stage(current(), message); }); }
Restore take_staged_restore(std::string &message) { return take(current(), message); }
#endif
}  // namespace wiiu::save_backup
