#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fsys = std::filesystem;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using Mii = std::array<u8, 0x60>;
static std::string g_save_base = "save";
static u32 le32(u32 value) { return value; }
static u16 le16(u16 value) { return value; }
#include "mii_id.inc"


static std::vector<Mii> host;
static int personal;
static std::string fault;
static int main_reads;
static bool coin_file;
static std::vector<u8> arriving_database;
static const std::vector<Mii> &console_miis(int *mine) { *mine = personal; return host; }

// Polynomial division with the implicit 16 zero bits appended to the message.
static u16 mii_crc16(const u8 *data, size_t size) {
    u32 remainder = 0;
    for (size_t i = 0; i < size + 2; ++i) {
        const u8 byte = i < size ? data[i] : 0;
        for (int bit = 7; bit >= 0; --bit) {
            remainder = (remainder << 1) | (byte >> bit & 1);
            if (remainder & 0x10000) remainder ^= 0x11021;
        }
    }
    return static_cast<u16>(remainder);
}

enum class File { MainRead, NewRead, Write };
static File file_role;

static FILE *fault_open(const char *path, const char *mode) {
    coin_file = std::string(path).find("gamecoin.dat") != std::string::npos;
    if (coin_file && fault == "coin-open") {
        errno = EACCES;
        return nullptr;
    }
    if (mode[0] == 'w') file_role = File::Write;
    else file_role = std::string(path).ends_with(".3diiu-new") ? File::NewRead : File::MainRead;
    if (file_role == File::MainRead) {
        ++main_reads;
        if (fault == "open-denied" || fault == "open-missing") {
            errno = fault == "open-denied" ? EACCES : ENOENT;
            return nullptr;
        }
    }
    return std::fopen(path, mode);
}

static size_t fault_read(void *buf, size_t size, size_t count, FILE *f) {
    const auto want = count;
    if (count && ((file_role == File::MainRead && fault == "short-read") ||
                  (file_role == File::NewRead && fault == "readback-short"))) --count;
    const size_t n = std::fread(buf, size, count, f);
    if (n == want && n && file_role == File::NewRead && fault == "readback-corrupt")
        static_cast<u8 *>(buf)[0] ^= 1;
    return n;
}

static size_t fault_write(const void *buf, size_t size, size_t count, FILE *f) {
    if (fault == "coin-write" && coin_file) {
        std::fwrite(buf, 1, size / 2, f);
        return 0;
    }
    return std::fwrite(buf, size, fault == "short-write" ? count / 2 : count, f);
}

static int fault_flush(FILE *f) {
    const int result = std::fflush(f);
    return fault == "flush" || (coin_file && fault == "coin-flush") ? EOF : result;
}

static int fault_close(FILE *f) {
    const int result = std::fclose(f);
    if (file_role == File::NewRead && fault == "late-arrival") {
        std::ofstream out("CFL_DB.dat", std::ios::binary);
        out.write(reinterpret_cast<const char *>(arriving_database.data()), arriving_database.size());
        out.close();
        if (!out.good()) std::abort();
    }
    if (file_role == File::MainRead && fault == "slow-retry") {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        return EOF;
    }
    if ((coin_file && fault == "coin-close") ||
        (file_role == File::MainRead && fault == "read-close") ||
        (file_role == File::NewRead && fault == "readback-close") ||
        (file_role == File::Write && fault == "write-close")) return EOF;
    return result;
}

static int fault_rename(const char *from, const char *to) {
    const std::string src(from), dst(to);
    if ((dst.ends_with("gamecoin.dat") && fault == "coin-rename") ||
        (dst.ends_with(".3diiu-old") && fault == "backup-rename") ||
        (src.ends_with(".3diiu-new") && (fault == "publish-rename" || fault == "rollback-rename")) ||
        (src.ends_with(".3diiu-old") && fault == "rollback-rename")) {
        errno = EACCES;
        return -1;
    }
    return std::rename(from, to);
}

#define fopen fault_open
#define fread fault_read
#define fwrite fault_write
#define fflush fault_flush
#define fclose fault_close
#define rename fault_rename
#define LOG(...) ((void)0)
#include "mii_database_under_test.inc"
#undef LOG
#undef rename
#undef fclose
#undef fflush
#undef fwrite
#undef fread
#undef fopen

#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "L%d: %s\n", __LINE__, #test); return 1; } } while (false)

static constexpr size_t DB_SIZE = 0x4BD20, RECORD_SIZE = 0x5C, RECORDS = 8, CRC = 0xC81E;
static constexpr int PERSONAL_SLOT = 7, FOREIGN_SLOT = 41;
static size_t record_at(int slot) { return RECORDS + slot * RECORD_SIZE; }

static int empty_slots(const std::vector<u8> &db) {
    int count = 0;
    for (int slot = 0; slot < 100; ++slot)
        if (mii_id_empty(db.data() + record_at(slot) + 0x0C)) ++count;
    return count;
}

static void guest_record(std::vector<u8> &db, int slot) {
    auto *record = db.data() + record_at(slot);
    std::fill_n(record, RECORD_SIZE, u8{0});
    record[0] = 3;
    record[2] = slot / 10 | slot % 10 << 4;
    record[0x0C] = 0xA0;
    record[0x0F] = slot;
    record[0x1A] = 'G';
}

static std::vector<u8> bytes(const fsys::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

static bool write(const fsys::path &path, const std::vector<u8> &contents) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(contents.data()), contents.size());
    out.close();
    return out.good();
}

static void crc(std::vector<u8> &db, size_t start, size_t offset) {
    const u16 c = mii_crc16(db.data() + start, offset - start);
    db[offset] = c >> 8;
    db[offset + 1] = c;
}

static std::vector<u8> fixture() {
    std::vector<u8> db(DB_SIZE);
    std::memcpy(db.data(), "CFOG\0\x01\0\0", 8);
    std::memcpy(db.data() + 0x23F8, "CFHE\xff\xff\xff\xff", 8);
    for (int i = 0; i < 3000; ++i) std::memcpy(db.data() + 0x2400 + i * 0xE + 10, "\xff\x7f\xff\x7f", 4);
    std::memcpy(db.data() + 0xC820, "CFRA", 4);
    auto *own = db.data() + record_at(PERSONAL_SLOT);
    std::copy_n(host[personal].begin(), RECORD_SIZE, own);
    own[0x02] = 0x70;
    own[0x1A] = 'B'; // needs an update without changing its slot
    auto *foreign = db.data() + record_at(FOREIGN_SLOT);
    foreign[0] = 3;
    foreign[0x02] = 0x14;
    foreign[0x11] = 1; // a permanent special Mii whose ID starts with four zero bytes
    foreign[0x1A] = 'F';
    db.back() = 0xA9;
    crc(db, 0xC820, 0xE4BE);
    crc(db, 0, CRC);
    return db;
}

static int verify_update(const fsys::path &path, const std::vector<u8> &prior) {
    const auto db = bytes(path);
    CHECK(db.size() == DB_SIZE && db != prior);
    CHECK(std::equal(db.begin() + CRC + 2, db.end(), prior.begin() + CRC + 2));
    CHECK(mii_crc16(db.data(), CRC + 2) == 0);
    CHECK(mii_crc16(db.data() + 0xC820, 0xE4BE - 0xC820 + 2) == 0);
    CHECK(std::equal(db.begin() + record_at(FOREIGN_SLOT), db.begin() + record_at(FOREIGN_SLOT) + RECORD_SIZE,
                     prior.begin() + record_at(FOREIGN_SLOT)));
    CHECK(mii_places.size() == host.size());
    CHECK(mii_places[personal] == PERSONAL_SLOT);
    CHECK(db[record_at(PERSONAL_SLOT) + 0x1A] == 'A');
    std::array<bool, 100> occupied{};
    int placed = 0;
    for (size_t i = 0; i < host.size(); ++i) {
        const int slot = mii_places[i];
        if (slot == -1) continue;
        CHECK(slot >= 0 && slot < 100 && slot != FOREIGN_SLOT && !occupied[slot]);
        occupied[slot] = true;
        ++placed;
        CHECK(std::equal(host[i].begin() + 0x0C, host[i].begin() + 0x16, db.begin() + record_at(slot) + 0x0C));
        CHECK(db[record_at(slot) + 0x02] == (slot / 10 | slot % 10 << 4));
    }
    CHECK(placed == 89 && empty_slots(db) == 10);
    CHECK(!fsys::exists(path.string() + ".3diiu-old") && !fsys::exists(path.string() + ".3diiu-new"));
    return 0;
}

static int verify_create(const std::string &scenario) {
    const fsys::path dir = "save/shared/extdata/00000000f000000b";
    const auto coin = dir / "gamecoin.dat", database = dir / "CFL_DB.dat";
    if (scenario == "guest-reopen") {
        const auto before = bytes(database);
        CHECK(before.size() == DB_SIZE && empty_slots(before) == 9);
        CHECK(before[record_at(99) + 0x1A] == 'G');
        CHECK(mii_maker_place(personal) == 0 && bytes(database) == before);
        return 0;
    }
    if (scenario == "directory-retry") {
        fsys::create_directories(dir.parent_path());
        CHECK(write(dir, {0x39}));
        CHECK(mii_maker_place(personal) == -1 && bytes(dir) == std::vector<u8>{0x39});
        fsys::remove(dir);
    } else if (scenario.starts_with("coin-")) {
        fault = scenario;
        CHECK(mii_maker_place(personal) == -1);
        CHECK(!fsys::exists(coin) && !fsys::exists(database));
        CHECK(!fsys::exists(dir / "gamecoin.dat.3diiu-new"));
        fault.clear();
    }
    if (scenario != "create" && scenario != "guest-save") {
        CHECK(mii_maker_place(personal) == -1 && !fsys::exists(database));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    CHECK(mii_maker_place(personal) == 0);
    CHECK(mii_maker_place(host.size()) == -1);
    const auto gc = bytes(coin);
    CHECK(gc.size() == 20 && gc[1] == 0x4f && gc[4] == 10);
    auto db = bytes(database);
    CHECK(db.size() == DB_SIZE && mii_crc16(db.data(), CRC + 2) == 0);
    CHECK(empty_slots(db) == 10);
    CHECK(mii_places.size() == host.size() && mii_places[personal] == 0);
    if (scenario == "guest-save") {
        CHECK(mii_id_empty(db.data() + record_at(99) + 0x0C));
        guest_record(db, 99);
        crc(db, 0, CRC);
        CHECK(write(database, db));
    }
    CHECK(write(coin, {0x39, 0x72}) && create_gamecoin(dir.string()));
    CHECK(bytes(coin) == (std::vector<u8>{0x39, 0x72}));
    return 0;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    const std::string scenario = argv[1];
    const fsys::path path = "CFL_DB.dat", old = "CFL_DB.dat.3diiu-old", fresh = "CFL_DB.dat.3diiu-new";
    CHECK(!fsys::exists(path) && !fsys::exists(old) && !fsys::exists(fresh));
    host.resize(3012);
    personal = 150;
    for (size_t i = 0; i < host.size(); ++i) {
        host[i][0] = 3;
        host[i][0x0C] = 0x98;
        host[i][0x0E] = i >> 8;
        host[i][0x0F] = i;
        host[i][0x1A] = 'A';
    }
    if (scenario == "create" || scenario == "directory-retry" || scenario.starts_with("coin-") ||
        scenario == "guest-save" || scenario == "guest-reopen")
        return verify_create(scenario);
    CHECK(write("gamecoin.dat", {0x39, 0x72}));
    auto prior = fixture();
    if (scenario == "full") {
        for (int slot = 0; slot < 100; ++slot) guest_record(prior, slot);
        crc(prior, 0, CRC);
        CHECK(write(path, prior));
        sync_mii_db(".");
        CHECK(bytes(path) == prior && mii_maker_place(personal) == -1);
        return 0;
    }
    if (scenario == "extra-byte") prior.push_back(0x05);
    if (scenario == "late-arrival") arriving_database = prior;
    else CHECK(write(scenario == "recovery" ? old : path, prior));
    if (scenario == "recovery") CHECK(write(fresh, {1, 2, 3}));
    if (scenario == "retry") fault = "short-write";
    else fault = scenario;
    sync_mii_db(".");
    if (scenario == "success" || scenario == "recovery") return verify_update(path, prior);
    if (scenario == "rollback-rename") {
        CHECK(!fsys::exists(path) && bytes(old) == prior);
        const auto staged = bytes(fresh);
        CHECK(staged.size() == DB_SIZE && staged != prior && mii_crc16(staged.data(), CRC + 2) == 0);
    } else {
        CHECK(bytes(path) == prior);
        CHECK(!fsys::exists(old) && !fsys::exists(fresh));
    }
    CHECK(mii_places.empty());
    CHECK(bytes("gamecoin.dat") == (std::vector<u8>{0x39, 0x72}));
    if (scenario == "retry" || scenario == "slow-retry") {
        fault.clear();
        for (int i = 0; i < 3000; ++i) sync_mii_db(".");
        CHECK(main_reads == 1 && mii_places.empty() && bytes(path) == prior);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        sync_mii_db(".");
        CHECK(main_reads == 2);
        return verify_update(path, prior);
    }
    return 0;
}
