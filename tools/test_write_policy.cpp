#include <cassert>
#include "write_policy.h"

int main() {
    for (const char *path : {"/vol/save/common", "/vol/save/common/log.txt", "/vol/save/80000001/game/main",
             "/vol/save/800000aF/game/extdata/00000042/", "/vol/external01/3dii-u",
             "/vol/external01/3dii-u/000500003d008630/start.txt",
             "/vol/external01/3ds/Checkpoint/saves/0x00863 Animal Crossing  New Leaf/20261117-074500/main"}) assert(wiiu::writable_path(path));
    for (const char *path : {"", "/", "/vol/save", "/vol/save/", "/vol/save/other/main", "/vol/save/8000000g/main",
             "/vol/storage_slc/rights/ticket/apps/00050000/10101f00.tik", "/vol/storage_mlc01/usr/title/game",
             "/vol/storage_mlc01/usr/save/00050010/1004a100/user/common/db/FFL_ODB.dat", "/vol/content/romfs.bin",
             "/vol/external01/install/game/title.tik", "/vol/external01/3dii-u-backup/file", "/vol/save/common/../system",
             "/vol/save/common/../../storage_slc/rights/ticket/x", "/vol/save/common//file", "/vol/save/common/./file",
             "/vol/save/common\\..\\..\\storage_slc", "fs:/vol/save/common/file", "relative/file", "/vol/save/common/file\n"}) {
        assert(!wiiu::writable_path(path));
    }
    assert(!wiiu::writable_path(nullptr));
    assert(wiiu::readonly_mode("r") && wiiu::readonly_mode("rb"));
    for (const char *mode : {"", "w", "a", "r+", "rb+", "r+b", "wb", "ab", "unknown"}) assert(!wiiu::readonly_mode(mode));
    assert(!wiiu::readonly_mode(nullptr));
    assert(wiiu::mkdir_path("/vol/external01/3ds"));
    assert(!wiiu::writable_path("/vol/external01/3ds"));
    assert(wiiu::mkdir_path("/vol/external01/3ds/Checkpoint"));
    assert(!wiiu::mkdir_path(nullptr));
    for (const char *path : {"/vol/external01/3ds/", "/vol/external01/3ds/other", "/vol/external01/3ds/file",
             "/vol/external01/3ds/../ticket", "/vol/external01/3ds-other", "/vol/storage_slc/rights/ticket"}) {
        assert(!wiiu::mkdir_path(path));
        assert(!wiiu::writable_path(path));
    }
}
