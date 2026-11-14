// save_from_card (port/wiiu/save_storage.cpp) on folders of this PC standing in for the console's save and the SD card: what an import replaces and
// keeps, that it is taken in once, what waits, an import that can't be read, and the export. Built and run by tools/selftest.py, with the Wii U's
// headers stood in for.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <share.h>
#include <sys/stat.h>

// what the wut headers would give (selftest.py makes them empty)
struct FSClient {};
struct FSCmdBlock {};
typedef int FSStatus;
typedef int FSErrorFlag;
typedef long long OSTime;
typedef void *OSDynLoad_Module;
enum { OS_DYNLOAD_OK = 0, OS_DYNLOAD_EXPORT_FUNC = 0, FS_ERROR_FLAG_ALL = -1, FS_STATUS_OK = 0 };
static void FSInit() {}
static int FSAddClient(FSClient *, int) { return 0; }
static void FSInitCmdBlock(FSCmdBlock *) {}
static OSTime OSGetSystemTime() { return 0; }
static long long OSTicksToMilliseconds(OSTime t) { return t; }
static long long OSTicksToSeconds(OSTime t) { return t; }
static int OSDynLoad_Acquire(const char *, OSDynLoad_Module *) { return 1; }  // no nn_save here: nothing is flushed
static int OSDynLoad_FindExport(OSDynLoad_Module, int, const char *, void **) { return 1; }
#define memalign(align, n) malloc(n)

#include "../port/wiiu/save_storage.cpp"

std::string g_save_base;
void rt_log(const char *, ...) {}
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

int main() {
    _fmode = _O_BINARY;  // as the console's files are
    const fs::path root = fs::temp_directory_path() / "3dii_u_save_from_card";
    fs::remove_all(root);
    const fs::path card = root / "card", save = root / "save" / "savedata";
    const auto run = [&] { wiiu::save_from_card(card.string(), save.string()); };
    const auto leftovers = [&] {
        for (const char *d : {"importing", "previous_new", "previous_old"}) assert(!fs::exists(card / d));
        for (const char *d : {"savedata_new", "savedata_old"}) assert(!fs::exists(save.parent_path() / d));
    };
    put(card / "import" / "main", "new" + BYTES);
    put(card / "import" / "sub" / "more.bin", BYTES);

    // no save folder: the game hasn't formatted on this console, so the import waits and nothing is made
    run();
    assert(fs::exists(card / "import" / "main") && !fs::exists(card / "previous") && !fs::exists(save));

    // an import that can't be copied in (here a locked file where its copy goes) is tried once: the save stays as it was, and the import is set aside
    put(save / "old.bin", "old" + BYTES);
    const fs::path in_the_way = save.parent_path() / "savedata_new";
    put(in_the_way, "locked");
    int locked = -1;
    _sopen_s(&locked, in_the_way.string().c_str(), _O_RDONLY, _SH_DENYRW, _S_IREAD);
    assert(locked >= 0);
    run();
    _close(locked);
    fs::remove(in_the_way);
    assert(get(save / "old.bin") == "old" + BYTES && !fs::exists(save / "main") && get(card / "previous" / "old.bin") == "old" + BYTES);
    assert(fs::exists(card / "import_failed" / "main") && !fs::exists(card / "import"));
    leftovers();
    run();  // and not again
    assert(get(save / "old.bin") == "old" + BYTES && fs::exists(card / "import_failed"));

    // taken in: the save is the import's files and only those, the one it had is on the card, the import kept as imported
    fs::rename(card / "import_failed", card / "import");
    run();
    assert(get(save / "main") == "new" + BYTES && get(save / "sub" / "more.bin") == BYTES && !fs::exists(save / "old.bin"));
    assert(get(card / "previous" / "old.bin") == "old" + BYTES && fs::exists(card / "imported" / "main") && !fs::exists(card / "import"));
    leftovers();

    // and never again: the game's progress since stays
    put(save / "main", "played" + BYTES);
    run();
    assert(get(save / "main") == "played" + BYTES && get(card / "previous" / "old.bin") == "old" + BYTES);

    // an export asked for with an empty folder gets the save, whole; one already there is left alone
    fs::create_directories(card / "export");
    run();
    assert(get(card / "export" / "main") == "played" + BYTES && get(card / "export" / "sub" / "more.bin") == BYTES && !fs::exists(card / "export_new"));
    put(save / "main", "later");
    run();
    assert(get(card / "export" / "main") == "played" + BYTES && wiiu::files_copied > 0);

    fs::remove_all(root);
    puts("save_from_card: all good");
    return 0;
}
