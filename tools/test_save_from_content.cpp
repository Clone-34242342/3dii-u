// save_from_content (port/wiiu/save_storage.cpp) on folders of this PC standing in for the title's content and the console's save: what the
// packed save replaces and keeps, that it waits for the game's own save, that it is taken in once, and one that can't be copied in. Built and
// run by tools/selftest.py, with the Wii U's headers stood in for.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <share.h>
#include <sys/stat.h>

// what the wut headers would give (selftest.py makes them empty)
struct FSClient {};
struct FSCmdBlock {};
struct FSDirectoryEntry { char name[256]; };
typedef int FSDirectoryHandle;
typedef int FSStatus;
typedef int FSErrorFlag;
typedef long long OSTime;
typedef void *OSDynLoad_Module;
enum { OS_DYNLOAD_OK = 0, OS_DYNLOAD_EXPORT_FUNC = 0, FS_ERROR_FLAG_ALL = -1, FS_STATUS_OK = 0,
       FS_STATUS_END = -2, FS_STATUS_NOT_FOUND = -6 };
static int open_status = FS_STATUS_OK, end_status = FS_STATUS_END, close_status = FS_STATUS_OK;
static unsigned opens, closes;
static size_t entry_index;
static std::vector<const char *> entries;
static int FSOpenDir(FSClient *, FSCmdBlock *, const char *path, FSDirectoryHandle *handle, int) {
    assert(!std::strcmp(path, "/vol/save/80000011/save/0004000000086300"));
    ++opens;
    entry_index = 0;
    *handle = 12;
    return open_status;
}
static int FSReadDir(FSClient *, FSCmdBlock *, FSDirectoryHandle handle, FSDirectoryEntry *entry, int) {
    assert(handle == 12);
    if (entry_index == entries.size()) return end_status;
    std::strcpy(entry->name, entries[entry_index++]);
    return FS_STATUS_OK;
}
static int FSCloseDir(FSClient *, FSCmdBlock *, FSDirectoryHandle handle, int) {
    assert(handle == 12);
    ++closes;
    return close_status;
}
static void FSInit() {}
static int FSAddClient(FSClient *, int) { return 0; }
static void FSInitCmdBlock(FSCmdBlock *) {}
static OSTime OSGetSystemTime() { return 0; }
static long long OSTicksToMilliseconds(OSTime t) { return t; }
static long long OSTicksToSeconds(OSTime t) { return t; }
static int OSDynLoad_Acquire(const char *, OSDynLoad_Module *) { return 1; }  // no nn_save here: nothing is flushed
static int OSDynLoad_FindExport(OSDynLoad_Module, int, const char *, void **) { return 1; }
#define memalign(align, n) malloc(n)

#ifndef SAVE_MIGRATION_FIXTURE
constexpr int short_write_fd = 1234;
static unsigned writes, quota_flushes;
static ssize_t test_pwrite(int fd, const void *data, size_t size, long long at) {
    if (fd != short_write_fd) return pwrite(fd, data, size, at);
    ++writes;
    if (writes == 1) {
        assert(at == 0 && size == 5 && !std::memcmp(data, "saved", size));
        return 2;
    }
    assert(writes == 2 && quota_flushes == 1);
    assert(at == 2 && size == 3 && !std::memcmp(data, "ved", size));
    return size;
}
#define pwrite test_pwrite
#endif
#include "../port/wiiu/save_storage.cpp"
#undef pwrite

std::string g_save_base;
void rt_log(const char *, ...) {}
#ifndef SAVE_MIGRATION_FIXTURE
void rt_fatal(const char *, ...) { std::abort(); }
#endif
void wiiu::note(const char *, ...) {}
void wiiu::fs_begin(u32) {}
void wiiu::fs_end() {}
bool wiiu::fsa_direct_flush_quota(const char *) { return false; }
void wiiu::fsa_direct_check_writes() {}

namespace fs = std::filesystem;
static const std::string BYTES("\x00\x1a\r\n\xff save", 10);  // what a text mode would change

static void put(const fs::path &p, const std::string &data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}

static std::string get(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

#ifndef SAVE_MIGRATION_FIXTURE
static void short_write_retry(const fs::path &root) {
    const auto file = (root / "pending").string();
    cache[file] = std::make_shared<wiiu::SaveFile>(wiiu::SaveFile{file, {'o', 'k'}, 0, true});
    flush_quota = [](FSClient *, FSCmdBlock *, u8 slot, FSErrorFlag) {
        assert(slot == COMMON);
        ++quota_flushes;
        return 0;
    };
    assert(write_retrying(short_write_fd, "saved", 5, 0) == 5);
    assert(wiiu::save_pending() && !fs::exists(file));
    wiiu::save_flush(true);
    assert(!wiiu::save_pending() && quota_flushes == 2 && get(file) == "ok");
    flush_quota = nullptr;
    cache.erase(file);
}
#endif

static bool shorter_copy(const fs::path &root) {
    const fs::path from = root / "from", to = root / "to";
    put(from / "shorter", BYTES);
    put(from / "empty", "");
    put(to / "shorter", BYTES + "old tail");
    put(to / "empty", "old contents");
    return wiiu::save_copy_tree(from.string(), to.string()) && get(to / "shorter") == BYTES &&
           fs::is_regular_file(to / "empty") && get(to / "empty").empty();
}

static void restore_listing() {
    const auto lacks_restore = [] {
        return wiiu::save_dir_lacks("fs:/vol/save/80000011/save/0004000000086300", "save_restore", ".checkpoint-restore");
    };
    assert(!lacks_restore() && opens == 0);
    save_client_ready = true;
    entries = {"savedata", "extdata", "settings.ini"};
    assert(lacks_restore() && closes == 1);
    entries = {"save_restore"};
    assert(!lacks_restore() && closes == 2);
    entries = {"savedata", ".checkpoint-restore"};
    assert(!lacks_restore() && closes == 3);
    entries = {"savedata"};
    end_status = FS_STATUS_NOT_FOUND;
    assert(!lacks_restore() && closes == 4);
    end_status = FS_STATUS_END;
    close_status = FS_STATUS_NOT_FOUND;
    assert(!lacks_restore() && closes == 5);
    open_status = FS_STATUS_NOT_FOUND;
    assert(!lacks_restore() && opens == 6 && closes == 5);
    save_client_ready = false;
}

int main() {
    restore_listing();
    _fmode = _O_BINARY;  // as the console's files are
    const fs::path root = fs::temp_directory_path() / "3dii_u_save_from_content";
    fs::remove_all(root);
    const fs::path packed = root / "content" / "save_import", save = root / "save" / "savedata", done = root / "common" / "save_imported";
    const fs::path previous = root / "save" / "savedata_previous";
    const auto run = [&] { wiiu::save_from_content(packed.string(), save.string(), done.string()); };
    const auto leftovers = [&] {
        for (const char *d : {"savedata_new", "savedata_old", "savedata_previous_new", "savedata_previous_old"}) assert(!fs::exists(save.parent_path() / d));
    };
    fs::create_directories(done.parent_path());

    // a game built with no save packed into it: nothing happens, and nothing is marked
    run();
    assert(!fs::exists(save) && !fs::exists(done));
    put(packed / "main", "new" + BYTES);
    put(packed / "sub" / "more.bin", BYTES);

    // no save folder: the game hasn't formatted on this console, so the packed save waits and nothing is made
    run();
    assert(!fs::exists(save) && !fs::exists(previous) && !fs::exists(done));

    // a packed save that can't be copied in (here a locked file where its copy goes) is tried once: the save stays as it was
    put(save / "old.bin", "old" + BYTES);
    const fs::path in_the_way = save.parent_path() / "savedata_new";
    put(in_the_way, "locked");
    int locked = -1;
    _sopen_s(&locked, in_the_way.string().c_str(), _O_RDONLY, _SH_DENYRW, _S_IREAD);
    assert(locked >= 0);
    run();
    _close(locked);
    fs::remove(in_the_way);
    assert(get(save / "old.bin") == "old" + BYTES && !fs::exists(save / "main") && get(previous / "old.bin") == "old" + BYTES);
    assert(get(done) == "failed");
    leftovers();
    run();  // and not again
    assert(!fs::exists(save / "main"));

    // taken in: the save is the packed files and only those, the one it had kept beside it, marked taken
    fs::remove(done);
    run();
    assert(get(save / "main") == "new" + BYTES && get(save / "sub" / "more.bin") == BYTES && !fs::exists(save / "old.bin"));
    assert(get(previous / "old.bin") == "old" + BYTES && get(done) == "taken" && get(packed / "main") == "new" + BYTES);
    leftovers();

    // and never again: the game's progress since stays
    put(save / "main", "played" + BYTES);
    run();
    assert(get(save / "main") == "played" + BYTES && get(previous / "old.bin") == "old" + BYTES && wiiu::files_copied > 0);

    assert(shorter_copy(root / "shorter_copy") && "a shorter copy must replace the whole destination file");

#ifndef SAVE_MIGRATION_FIXTURE
    short_write_retry(root);
#endif
    fs::remove_all(root);
    puts("save_from_content: all good");
    return 0;
}
