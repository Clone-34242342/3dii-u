// Fault-injection hooks before the production save code, and executable cases after it.
#if !defined(SAVE_MIGRATION_CASES)

#include <cerrno>
#include <cstdarg>
#include <stdexcept>
#include <unistd.h>

static bool fail_writes, fail_install, fail_rollback, fail_unpublish;
static int fail_close_at, close_calls, failed_closes, failed_writes, failed_renames;
static long long forced_size;
static std::string account_save;
static std::filesystem::path bad_open, bad_increment, bad_status, bad_entry, bad_exists, bad_empty;
static int directory_faults;

static bool directory_fault(const std::filesystem::path &path, const std::filesystem::path &bad, std::error_code &ec) {
    if (bad.empty() || path != bad) return false;
    ++directory_faults;
    ec = std::make_error_code(std::errc::permission_denied);
    return true;
}

struct MigrationDirectory {
    std::filesystem::path dir;
    std::filesystem::directory_iterator it;

    MigrationDirectory() = default;
    MigrationDirectory(const std::filesystem::path &directory, std::error_code &ec) : dir(directory) {
        if (!directory_fault(dir, bad_open, ec)) it = std::filesystem::directory_iterator(dir, ec);
    }
    MigrationDirectory begin() const { return *this; }
    MigrationDirectory end() const { return {}; }
    bool operator!=(const MigrationDirectory &other) const { return it != other.it; }
    const MigrationDirectory &operator*() const { return *this; }
    const MigrationDirectory *operator->() const { return this; }
    const std::filesystem::path &path() const { return it->path(); }
    bool is_directory(std::error_code &ec) const {
        return !directory_fault(path(), bad_entry, ec) && it->is_directory(ec);
    }
    void increment(std::error_code &ec) {
        if (directory_fault(dir, bad_increment, ec)) it = {};
        else it.increment(ec);
    }
    MigrationDirectory &operator++() {
        std::error_code ec;
        increment(ec);
        if (ec) throw std::filesystem::filesystem_error("injected iteration error", dir, ec);
        return *this;
    }
};

static std::filesystem::file_status migration_status(const std::filesystem::path &path, std::error_code &ec) {
    return directory_fault(path, bad_status, ec) ? std::filesystem::file_status() : std::filesystem::status(path, ec);
}

static bool migration_exists(const std::filesystem::path &path, std::error_code &ec) {
    if (!bad_increment.empty() && path.filename() == "savedata" && path.parent_path().parent_path() == bad_increment) {
        ec.clear();  // the first entry examined has no save; the injected read fails before the next entry
        return false;
    }
    return !directory_fault(path, bad_exists, ec) && std::filesystem::exists(path, ec);
}

static bool migration_empty(const std::filesystem::path &path, std::error_code &ec) {
    return !directory_fault(path, bad_empty, ec) && std::filesystem::is_empty(path, ec);
}

static ssize_t migration_pwrite(int fd, const void *data, size_t size, long long at) {
    if (fail_writes) {
        ++failed_writes;
        errno = EIO;
        return -1;
    }
    return pwrite(fd, data, size, at);
}

static int migration_close(int fd) {
    const int rc = close(fd);
    if (++close_calls == fail_close_at) {
        assert(rc == 0);
        ++failed_closes;
        errno = EIO;
        return -1;
    }
    return rc;
}

static int migration_fstat(int fd, struct stat *info) {
    static_assert(sizeof(info->st_size) >= 8);
    const int rc = fstat(fd, info);
    if (!rc && forced_size) info->st_size = forced_size;
    return rc;
}

static void migration_rename(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec) {
    if ((to == account_save && ((fail_install && from == account_save + "_new") ||
                               (fail_rollback && from == account_save + "_old"))) ||
        (fail_unpublish && from == account_save && to == account_save + "_new")) {
        ++failed_renames;
        ec = std::make_error_code(std::errc::permission_denied);
        return;
    }
    std::filesystem::rename(from, to, ec);
}

static const char *const TEST_MARKER = "/test_run_once", *const TEST_FOLDER = "/test_run";
static bool in_test;
static std::string test_flags, common_data;
static OSTime OSGetTime() { return 0; }
static OSTime OSSecondsToTicks(OSTime seconds) { return seconds; }

#else

static void require(bool good, const char *message) {
    if (good) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

struct SaveFatal : std::runtime_error { using std::runtime_error::runtime_error; };

void rt_fatal(const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    throw SaveFatal(buffer);
}

static int quota_result, account_flushes;
static bool old_at_flush, new_at_flush;

static FSStatus quota(FSClient *, FSCmdBlock *, u8 slot, FSErrorFlag) {
    if (slot != account_slot) return 0;
    ++account_flushes;
    old_at_flush = get(fs::path(account_save + "_old") / "extdata/100/keep") == "account extdata" + BYTES;
    new_at_flush = get(fs::path(account_save) / "title/savedata/main") == "common data" + BYTES;
    return quota_result;
}

int main(int argc, char **argv) {
    require(argc == 3, "expected a case and its private fixture directory");
    _fmode = _O_BINARY;
    const std::string scenario = argv[1];
    const fs::path root = argv[2];
    if (scenario == "packed") return packed_save_checks();
    if (scenario == "shorter") {
        require(shorter_copy(root), "a shorter copy retained destination bytes");
        return 0;
    }
    if (scenario == "too-large" || scenario == "negative-size") {
        const fs::path from = root / "from", to = root / "to";
        put(from / "data", "abc");
        // The low 32 bits are 1, so an unchecked cast can copy a real byte and report success.
        forced_size = scenario == "too-large" ? 0x100000001LL : -0xffffffffLL;
        require(!wiiu::save_copy_tree(from.string(), to.string()), "invalid host file size was narrowed into a successful copy");
        require(!fs::exists(to / "data"), "invalid host file size was rejected after creating the destination file");
        return 0;
    }

    const fs::path common = root / "common/save", account = root / "account/save";
    const fs::path old = account.generic_string() + "_old", fresh = account.generic_string() + "_new";
    const fs::path main = "title/savedata/main", keep = "extdata/100/keep", overlap = "extdata/100/shared";
    common_data = common.parent_path().generic_string();
    account_dir = account.parent_path().generic_string();  // save_storage.cpp's real current account
    account_slot = 1;
    account_save = account.generic_string();
    flush_quota = quota;
    if (scenario == "new-account") {
        require(wiiu::save_folder() == account_save && account_flushes == 0, "an empty console could not start a new account save");
        return 0;
    }
    put(common / main, "common data" + BYTES);
    put(common / overlap, "legacy longer data" + BYTES);
    put(account / keep, "account extdata" + BYTES);
    put(account / overlap, BYTES);

    if (scenario.rfind("probe-", 0) == 0) {
        if (scenario == "probe-open" || scenario == "probe-no-account") bad_open = common;
        else if (scenario == "probe-increment") bad_increment = common;
        else if (scenario == "probe-root-status") bad_status = common;
        else if (scenario == "probe-entry") bad_entry = common / "title";
        else if (scenario == "probe-savedata-status") bad_exists = common / main.parent_path();
        else if (scenario == "probe-account-status") bad_exists = account / "extdata/savedata";
        else if (scenario == "probe-empty") bad_empty = common / main.parent_path();
        else require(false, "unknown probe case");
        if (scenario == "probe-no-account") account_dir.clear();
        bool stopped = false;
        try {
            wiiu::save_folder();
        } catch (const SaveFatal &e) {
            stopped = std::string(e.what()).find("Cannot read the save directory") != std::string::npos;
        } catch (const std::filesystem::filesystem_error &) {
            require(false, "directory iteration threw instead of stopping with a save read error");
        }
        require(stopped, "unreadable save storage did not produce a save read error before selecting a directory");
        require(directory_faults == 1 && account_flushes == 0, "save discovery did not stop at the read error");
        require(get(account / keep) == "account extdata" + BYTES && get(account / overlap) == BYTES && !fs::exists(account / main),
                "failed discovery changed the existing account storage");
    } else if (scenario == "copy-increment") {
        bad_increment = common / main.parent_path();
        std::string chosen;
        try {
            chosen = wiiu::save_folder();
        } catch (const std::filesystem::filesystem_error &) {
            require(false, "copy iteration threw instead of discarding the incomplete copy");
        }
        require(directory_faults == 1 && account_flushes == 0 && chosen == common.generic_string(), "copy iteration error did not keep the common save selected");
        require(get(account / keep) == "account extdata" + BYTES && !fs::exists(account / main), "copy iteration error changed account storage");
    } else if (scenario == "flush-unavailable" || scenario == "flush-negative" || scenario == "flush-confirmed") {
        if (scenario == "flush-unavailable") flush_quota = nullptr;
        if (scenario == "flush-negative") quota_result = -EIO;
        const std::string selected = wiiu::save_folder();
        if (scenario == "flush-confirmed") {
            require(selected == account_save && get(account / main) == "common data" + BYTES, "successful account commit was not selected");
        } else {
            require(selected == common.generic_string(), "unconfirmed account commit was selected");
            require(get(account / keep) == "account extdata" + BYTES && get(account / overlap) == BYTES && !fs::exists(account / main),
                    "unconfirmed account commit replaced existing account storage");
        }
        if (flush_quota) require(account_flushes == 1 && old_at_flush && new_at_flush, "previous account disappeared before the new account was flushed");
        else require(account_flushes == 0, "the unavailable quota callback ran");
    } else if (scenario == "flush-rollback-stuck") {
        quota_result = -EIO;
        fail_unpublish = true;
        bool stopped = false;
        try {
            wiiu::save_folder();
        } catch (const SaveFatal &e) {
            stopped = std::string(e.what()).find("Cannot roll back the unconfirmed account save") != std::string::npos;
        }
        require(stopped && failed_renames == 1 && get(old / keep) == "account extdata" + BYTES,
                "failed commit and rollback did not stop with the previous account recoverable");
        stopped = false;
        try {
            wiiu::save_folder();
        } catch (const SaveFatal &e) {
            stopped = std::string(e.what()).find("Cannot confirm the account save") != std::string::npos;
        }
        require(stopped && get(old / keep) == "account extdata" + BYTES, "next startup selected an unconfirmed account save");
        fail_unpublish = false;
        quota_result = 0;
        require(wiiu::save_folder() == account_save && get(account / keep) == "account extdata" + BYTES,
                "account save could not be confirmed once quota flush recovered");
    } else if (scenario == "merge") {
        require(wiiu::save_folder() == account_save, "successful migration did not select the account");
        require(get(account / main) == "common data" + BYTES, "common save data was not merged");
        require(get(account / keep) == "account extdata" + BYTES, "account-only extdata was lost");
        require(get(account / overlap) == BYTES, "account extdata collision kept legacy or trailing bytes");
    } else if (scenario == "account-existing") {
        put(account / main, "new account save" + BYTES);
        require(wiiu::save_folder() == account_save, "existing account was not selected");
        require(get(account / main) == "new account save" + BYTES, "migration replaced a real account save");
    } else {
        int locked = -1;
        if (scenario == "locked-common") {
            _sopen_s(&locked, (common / main).string().c_str(), _O_RDONLY, _SH_DENYRW, _S_IREAD);
            require(locked >= 0, "could not lock the common save fixture");
        } else if (scenario == "write") {
            fail_writes = true;
        } else if (scenario == "input-close" || scenario == "output-close") {
            fail_close_at = scenario == "input-close" ? 1 : 2;
        } else if (scenario == "rename" || scenario == "rollback") {
            fail_install = true;
            fail_rollback = scenario == "rollback";
        } else {
            require(false, "unknown test case");
        }
        const std::string selected = wiiu::save_folder();
        if (locked >= 0) _close(locked);
        if (fail_rollback) {
            require(selected == common.generic_string(), "failed rollback did not keep the common save selected");
            require(failed_renames == 2, "the simulated installation and rollback failures did not both occur");
            require(!fs::exists(account) && get(old / keep) == "account extdata" + BYTES && get(fresh / main) == "common data" + BYTES,
                    "failed rollback lost recoverable trees");
            fail_install = fail_rollback = false;
            require(wiiu::save_folder() == account_save, "next startup did not recover the interrupted migration");
            require(get(account / keep) == "account extdata" + BYTES && get(account / overlap) == BYTES && get(account / main) == "common data" + BYTES,
                    "rollback recovery did not merge both saves");
        } else {
            require(get(account / keep) == "account extdata" + BYTES && get(account / overlap) == BYTES, "failed copy deleted or corrupted the existing account extdata");
            require(selected == common.generic_string(), "failed migration did not fall back to intact common storage");
            require(!fs::exists(account / main), "an incomplete copy was published in the account save");
            if (fail_writes) require(failed_writes == 2, "testing did not exercise the failed write and its retry");
            if (fail_close_at) require(failed_closes == 1, "the simulated close failure did not occur");
            if (fail_install) require(failed_renames == 1, "the simulated rename failure did not occur");
        }
    }
    require(get(common / main) == "common data" + BYTES && get(common / overlap) == "legacy longer data" + BYTES, "common storage was modified");
    require(!fs::exists(old) && !fs::exists(fresh), "migration left stale staging files");
    return 0;
}

#endif
