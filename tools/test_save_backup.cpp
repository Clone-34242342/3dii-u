// Executes the console implementation against private temporary files; no runtime or SD mount.
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace kernel {
struct Mutex {
    std::mutex mutex;
    std::atomic<bool> held{false};
    void lock() { mutex.lock(); held = true; }
    void unlock() { held = false; mutex.unlock(); }
};
static Mutex kmutex;
using Lock = std::unique_lock<Mutex>;
}
static std::mutex saves;
static std::recursive_mutex storage;
static thread_local unsigned save_locks, file_locks;
static void save_lock() { saves.lock(); ++save_locks; }
static void save_unlock() { --save_locks; saves.unlock(); }
namespace wiiu {
static void fs_begin(unsigned) { storage.lock(); ++file_locks; }
static void fs_end() { --file_locks; storage.unlock(); }
}

namespace fs = std::filesystem;
static int operation, fail_at;
static std::function<bool(const char *, const fs::path &, const fs::path &)> allow;
static bool test_allow(const char *op, const fs::path &a, const fs::path &b) {
    ++operation;
    return operation != fail_at && (!allow || allow(op, a, b));
}
static bool test_flush() {
    wiiu::fs_begin(0);
    const bool ok = test_allow("flush", {}, {});
    wiiu::fs_end();
    return ok;
}
#define SAVE_BACKUP_TEST
#include "../port/wiiu/save_backup.cpp"
using namespace wiiu::save_backup;

static void file(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), s.size());
    assert(f.good());
}
static std::string bytes(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::map<std::string, std::string> snapshot(const fs::path &p) {
    std::map<std::string, std::string> out;
    if (fs::exists(p)) for (const auto &e : fs::recursive_directory_iterator(p)) {
        const auto name = fs::relative(e.path(), p).generic_string();
        out[name] = e.is_directory() ? "DIRECTORY" : bytes(e.path());
    }
    return out;
}
static void reset_faults() { operation = fail_at = 0; allow = {}; }
static const std::string id1 = "0000000000000863", id2 = "0000000000001234";
static const std::string binary("\0\x1a\r\n\xff", 5);

int main() {
    const std::string account = "/vol/save/80000001";
    const Path save = Path(account) / "save/0004000000086300";
    const Path common = "fs:/vol/save/common/save/0004000000086300";
    auto fsa_status = [](const Path &p, std::error_code &ec) {
        if (p == "fs:" || p == "fs:/vol/save" || p == "/vol/save") {
            ec = std::make_error_code(p == "fs:" ? std::errc::not_enough_memory : std::errc::permission_denied);
            return fs::file_status(fs::file_type::none);
        }
        if (p.filename() == ".checkpoint-restore") {
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return fs::file_status(fs::file_type::not_found);
        }
        return fs::file_status(fs::file_type::directory);
    };
    for (const Path &p : {save, common, save / ".checkpoint-restore", common / ".checkpoint-restore", Path(card_mount)}) {
        assert(!checked_path(p, nullptr, fsa_status));
        assert(checked_path(p, &account, fsa_status));
    }
    assert(!checked_path("/vol/save/80000002/save", &account, fsa_status));
    assert(!checked_path(save / "../savedata", &account, fsa_status));
    auto bad_child = [&](fs::file_type type, std::errc error) {
        return checked_path(save / ".checkpoint-restore", &account, [&](const Path &p, std::error_code &ec) {
            if (p.filename() != ".checkpoint-restore") return fsa_status(p, ec);
            ec = std::make_error_code(error);
            return fs::file_status(type);
        });
    };
    assert(!bad_child(fs::file_type::symlink, {}));
    assert(!bad_child(fs::file_type::none, std::errc::permission_denied));

    const fs::path temp = fs::canonical(fs::temp_directory_path());
    const fs::path root = temp / ("3dii-u-backup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));  // never remove/reuse someone else's test folder
    unsigned serial = 0;
    auto context = [&] {
        Context c{root / std::to_string(++serial) / "title", root / std::to_string(serial) / "card", 0x0004000000086300ull, "Animal Crossing: New Leaf"};
        fs::create_directories(c.card);
        file(c.save / "savedata" / "garden.dat", "backup" + binary);
        file(c.save / "savedata" / "nested" / "empty", "");
        file(c.save / "extdata" / id1 / "photo", "photo1" + binary);
        file(c.save / "extdata" / id2 / "file", "photo2");
        file(c.save / "settings.ini", "must stay");
        return c;
    };
    std::string message;
    {
        const auto c = context();
        const auto files = snapshot(c.save);
        reset_faults();
        assert(take(c, message) == Restore::None && message.empty());
        assert(operation == 0 && snapshot(c.save) == files);
    }
    const time_t now = 1700000000;
    auto exported = [&](const Context &c) {
        assert(export_backup(c, now, message));
        assert(stage(c, message));
        std::string name;
        assert(selection(c, bytes(c.save / "save_restore"), name));
        return name;
    };
    auto current_progress = [&](const Context &c) {
        fs::remove_all(c.save / "savedata");
        fs::remove_all(c.save / "extdata");
        file(c.save / "savedata" / "new-progress", "current save");
        file(c.save / "extdata" / id1 / "current", "current extra");
    };
    auto assert_current = [&](const Context &c) {
        assert(bytes(c.save / "savedata" / "new-progress") == "current save");
        assert(bytes(c.save / "extdata" / id1 / "current") == "current extra");
        assert(!fs::exists(c.save / "savedata" / "garden.dat"));
        assert(!fs::exists(c.save / "extdata" / id2));
    };

    // Whole pair, including a previously absent ID and binary/empty files; unrelated state survives.
    Context c = context();
    assert(folder_name(c) == "0x00863 Animal Crossing  New Leaf");
    const auto saved = snapshot(c.save / "savedata"), extra = snapshot(c.save / "extdata");
    const auto name = exported(c);
    current_progress(c);
    assert(take(c, message) == Restore::Restored);
    assert(snapshot(c.save / "savedata") == saved && snapshot(c.save / "extdata") == extra);
    assert(bytes(c.save / "settings.ini") == "must stay");
    assert(bytes(transaction(c) / "previous" / "savedata" / "new-progress") == "current save");
    assert(!fs::exists(c.save / "save_restore"));
    assert(bytes(c.save / "save_imported") == "Checkpoint restore\n");
    assert(take(c, message) == Restore::None);
    // Restaging the same backup must restore again, not confuse an older outcome with this request.
    current_progress(c);
    assert(stage(c, message) && take(c, message) == Restore::Restored);
    assert(snapshot(c.save / "savedata") == saved);

    // Once committed, cleanup failure may allow play. Never roll back or reject later game changes.
    c = context(); exported(c); current_progress(c);
    allow = [&](const char *op, const fs::path &from, const fs::path &) {
        return std::string(op) != "erase" || from != c.save / "save_restore";
    };
    assert(take(c, message) == Restore::Deferred);
    reset_faults();
    fs::remove_all(c.save / "extdata");  // a legitimate in-game "reset extra data"
    file(c.save / "savedata" / "garden.dat", "progress after restored save");
    assert(take(c, message) == Restore::Restored);
    assert(!fs::exists(c.save / "extdata"));
    assert(bytes(c.save / "savedata" / "garden.dat") == "progress after restored save");

    c = context(); assert(export_backup(c, now, message));

    // Timestamp collisions never overwrite an earlier backup; failed publication leaves it selectable.
    auto first = snapshot(game(c, "saves") / name);
    file(c.save / "savedata" / "garden.dat", "second export");
    assert(export_backup(c, now, message));
    assert(snapshot(game(c, "saves") / name) == first);
    assert(fs::exists(game(c, "saves") / (name + "-001")));
    allow = [&](const char *op, const fs::path &, const fs::path &to) { return std::string(op) != "rename" || to.parent_path() != game(c, "saves"); };
    assert(!export_backup(c, now, message));
    reset_faults();
    assert(!fs::exists(game(c, "saves") / (name + "-002")));
    assert(stage(c, message));
    std::string selected;
    assert(selection(c, bytes(c.save / "save_restore"), selected) && selected == name + "-001");

    // Every export failure preserves older published bytes; no partial tree becomes selectable.
    c = context(); reset_faults();
    assert(export_backup(c, now, message));
    const int export_mutations = operation;
    for (int failure = 1; failure <= export_mutations; ++failure) {
        reset_faults(); c = context();
        assert(export_backup(c, now, message));
        const auto older = snapshot(game(c, "saves"));
        reset_faults(); fail_at = failure;
        const bool ok = export_backup(c, now, message);
        reset_faults();
        if (!ok) assert(snapshot(game(c, "saves")) == older);
        assert(stage(c, message));
        assert(take(c, message) == Restore::Restored);
    }

    // One-ID export stays flat for Checkpoint while metadata identifies it on a fresh extdata tree.
    c = context();
    fs::remove_all(c.save / "extdata" / id2);
    const auto single = exported(c);
    assert(bytes(game(c, "extdata") / single / "photo") == "photo1" + binary);
    fs::remove_all(c.save / "extdata");
    assert(take(c, message) == Restore::Restored);
    assert(bytes(c.save / "extdata" / id1 / "photo") == "photo1" + binary);

    // A paired snapshot with no extdata removes later extdata, keeping its previous copy.
    c = context();
    fs::remove_all(c.save / "extdata");
    exported(c);
    file(c.save / "extdata" / id2 / "later", "later");
    assert(take(c, message) == Restore::Restored && !fs::exists(c.save / "extdata"));
    assert(bytes(transaction(c) / "previous" / "extdata" / id2 / "later") == "later");

    // An ordinary external Checkpoint save has no metadata: leave existing extdata alone.
    c = context();
    file(game(c, "saves") / "20261117-074500 external" / "external.dat", "from 3DS");
    const auto untouched_extra = snapshot(c.save / "extdata");
    assert(stage(c, message) && take(c, message) == Restore::Restored);
    assert(bytes(c.save / "savedata" / "external.dat") == "from 3DS");
    assert(snapshot(c.save / "extdata") == untouched_extra);

    // Missing SD and malformed/foreign requests retain both the current pair and request.
    c = context();
    exported(c);
    current_progress(c);
    fs::rename(c.card, c.card.string() + "-unplugged");
    assert(take(c, message) == Restore::Deferred);
    assert_current(c);
    assert(fs::exists(c.save / "save_restore"));
    assert(!export_backup(c, now, message) && !fs::exists(c.card));
    fs::rename(c.card.string() + "-unplugged", c.card);
    for (const std::string &bad : {std::string("x"), std::string(5000, 'x'), "fs:/vol/storage_mlc01/ticket/" + name,
                                   "3DIIU1\n0004000000000000\n" + name + "\n", record(c, "../" + name)}) {
        file(c.save / "save_restore", bad);
        assert(take(c, message) == Restore::Deferred);
        assert(bytes(c.save / "save_restore") == bad);
        assert_current(c);
    }

    // Reject damaged pairing metadata instead of silently restoring the wrong extdata.
    c = context(); const auto paired = exported(c); current_progress(c);
    file(game(c, ".3dii-u") / paired / "pair.txt", record(c, paired) + "../ticket\n");
    assert(take(c, message) == Restore::Deferred); assert_current(c);
    // A write failure while staging creates no request and remains retryable.
    c = context(); assert(export_backup(c, now, message));
    allow = [](const char *op, const fs::path &, const fs::path &) { return std::string(op) != "write"; };
    assert(!stage(c, message) && !fs::exists(c.save / "save_restore"));
    reset_faults(); assert(stage(c, message));

    // Fault each mutation/flush in a real restore. A nonfatal outcome never exposes a mixed pair.
    c = context(); exported(c); current_progress(c); reset_faults();
    assert(take(c, message) == Restore::Restored);
    const int mutations = operation;
    for (int failure = 1; failure <= mutations; ++failure) {
        reset_faults();
        c = context(); exported(c); current_progress(c);
        const auto before_save = snapshot(c.save / "savedata"), before_extra = snapshot(c.save / "extdata");
        reset_faults(); fail_at = failure;
        const Restore result = take(c, message);
        reset_faults();
        if (result != Restore::Unsafe) {
            const bool old = snapshot(c.save / "savedata") == before_save && snapshot(c.save / "extdata") == before_extra;
            const bool restored = snapshot(c.save / "savedata") == saved && snapshot(c.save / "extdata") == extra;
            assert(old || restored);
        }
        assert(bytes(c.save / "settings.ini") == "must stay");
        // A transient fault is recoverable. It may finish immediately or require one restart after cleanup.
        Restore recovered = take(c, message);
        assert(recovered != Restore::Unsafe);
        if (recovered == Restore::Deferred) recovered = take(c, message);
        assert(recovered == Restore::Restored || recovered == Restore::None);
        assert(snapshot(c.save / "savedata") == saved && snapshot(c.save / "extdata") == extra);
    }

    // Simulate an interrupted mixed pair, then a failed rollback: originals are retained, startup is blocked.
    c = context(); const auto crash_name = exported(c); current_progress(c);
    Plan p{crash_name, 3, 3, 3};
    const auto tx = transaction(c);
    assert(mkdirs(tx / "next") && mkdirs(tx / "previous"));
    assert(put(tx / "owner", title_id(c) + "\n") && put(tx / "ready", plan_record(c, p)));
    assert(copy_tree(game(c, "saves") / crash_name, tx / "next" / "savedata"));
    assert(copy_tree(c.save / "extdata", tx / "next" / "extdata"));
    assert(move(c.save / "savedata", tx / "previous" / "savedata"));
    assert(move(tx / "next" / "savedata", c.save / "savedata"));
    fs::rename(c.card, c.card.string() + "-unplugged");
    allow = [&](const char *op, const fs::path &from, const fs::path &) { return std::string(op) != "rename" || from != tx / "previous" / "savedata"; };
    assert(take(c, message) == Restore::Unsafe);
    assert(bytes(tx / "previous" / "savedata" / "new-progress") == "current save");
    reset_faults();
    const auto malformed_request = root / "synthetic-request-ticket";
    file(malformed_request, "synthetic protected bytes");
    fs::remove(c.save / "save_restore");
    fs::create_hard_link(malformed_request, c.save / "save_restore");
    assert(take(c, message) == Restore::Deferred);
    assert_current(c);
    assert(bytes(malformed_request) == "synthetic protected bytes" && fs::hard_link_count(malformed_request) == 2);

    // Hardlinked input cannot be followed or truncated. Synthetic ticket remains identical.
    c = context();
    const auto ticket = root / "synthetic-ticket";
    file(ticket, "synthetic ticket only");
    fs::create_hard_link(ticket, c.save / "savedata" / "alias");
    assert(!export_backup(c, now, message));
    assert(bytes(ticket) == "synthetic ticket only" && fs::hard_link_count(ticket) == 2);

    // The exact production snapshot guard drains a Write that already released kmutex,
    // and does not let it reacquire kmutex while the snapshot or nested flush is active.
    reset_faults(); c = context();
    std::promise<void> writing, finish_write;
    auto released = finish_write.get_future();
    std::atomic<bool> wrote{false}, resumed{false};
    std::thread writer([&] {
        save_lock(); writing.set_value(); released.wait();
        file(c.save / "savedata" / "async-write", "finished before snapshot");
        wrote = true; save_unlock();
        kernel::Lock lock(kernel::kmutex); resumed = true;
    });
    writing.get_future().wait();
    std::thread exporter([&] {
        assert(wiiu::save_backup::snapshot([&] {
            assert(wrote && !resumed && save_locks == 1 && file_locks == 1);
            const bool ok = export_backup(c, now, message);
            assert(!resumed && file_locks == 1);
            return ok;
        }));
        assert(save_locks == 0 && file_locks == 0);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!kernel::kmutex.held && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(kernel::kmutex.held && !wrote);
    finish_write.set_value(); exporter.join(); writer.join();
    assert(resumed);
    assert(stage(c, message));
    std::string async_name;
    assert(selection(c, bytes(c.save / "save_restore"), async_name));
    assert(bytes(game(c, "saves") / async_name / "async-write") == "finished before snapshot");
    assert(!wiiu::save_backup::snapshot([] { return false; }));
    assert(!kernel::kmutex.held && save_locks == 0 && file_locks == 0);

    Requests requests;
    assert(!requests.request(Action::None));
    assert(requests.request(Action::Export) && requests.view().busy);
    assert(requests.take() == Action::Export && requests.take() == Action::None);
    assert(!requests.request(Action::StageRestore));  // cannot replace a running request
    requests.complete("Backup complete");
    assert(!requests.view().busy && requests.view().message == "Backup complete");
    assert(requests.request(Action::StageRestore) && requests.take() == Action::StageRestore);
    requests.complete("Restore pending");

    // Cleanup is confined to this exclusively-created temporary root, never a reused fixed path.
    assert(root.parent_path() == temp && root.filename().string().rfind("3dii-u-backup-", 0) == 0);
    fs::remove_all(root);
    printf("save backup: full orchestration, %d export and %d restore fault points passed\n", export_mutations, mutations);
}
