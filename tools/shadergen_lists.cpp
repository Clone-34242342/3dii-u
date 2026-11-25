// shadergen --lists <out.bin> [--dvlb <dvlb-stream>] [--materials <material-stream>] <romfs.bin>...
//
// Game Freak's model files (Pokémon Sun and Moon on) hold a shader not as a shader binary (DVLB) but as the register writes that load it:
// the code, the swizzles, the uniforms, the entry point and the output map, ending in the jump back out of the list. The files lie packed
// (LZ11) in GARC archives, 7 GB unpacked for Ultra Sun, so the search is native code here; tools/dvlb_to_log.py, which finds the shader
// binaries, runs it and keeps what it finds with them. Each list is read the way runtime/gpu/gpu.cpp runs one, and what a draw after it
// would log (port/pc/vs_log.cpp) is written as a "VS  " record, each different one once.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/pica.h"

using namespace gpu;

namespace {
constexpr size_t kMostUnpacked = 64 << 20;  // of one file in an archive, packed or unpacked (tools/dvlb_to_log.py's MAX_UNPACKED)
constexpr size_t kMostTable = 64 << 20;     // of a romfs's file table, or an archive's
constexpr size_t kMostDvlb = 1 << 18;      // tools/dvlb_to_log.py's MAX_BINARY
constexpr char kCriMagic[] = "CRILAYLA";
constexpr size_t kCriMagicSize = sizeof kCriMagic - 1;
constexpr size_t kCriPrefix = 256;

u32 le16at(const u8 *p) { return p[0] | p[1] << 8; }
u32 le32at(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
u64 le64at(const u8 *p) { return le32at(p) | (u64)le32at(p + 4) << 32; }

// a file too big to load whole
struct Image {
    std::ifstream f;
    u64 size = 0;
    bool open(const char *path) {
        f.open(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        size = (u64)f.tellg();
        return true;
    }
    bool read(u64 at, void *to, size_t n) {
        if (at > size || n > size - at) return false;
        f.clear();
        f.seekg((std::streamoff)at);
        return (bool)f.read((char *)to, (std::streamsize)n);
    }
    bool read(u64 at, std::vector<u8> &to, size_t n) {
        to.resize(n);
        return read(at, to.data(), n);
    }
};

// An LZ11 stream: 0x11, the unpacked size in three bytes (in four more if those are zero), then flag bytes, each for eight literals or
// copies. False if it isn't a whole one.
bool unlz11(const u8 *d, size_t n, std::vector<u8> &out) {
    if (n < 8 || d[0] != 0x11) return false;
    size_t size = le32at(d) >> 8, p = 4;
    if (!size) size = le32at(d + 4), p = 8;
    if (size < 4 || size > kMostUnpacked) return false;
    out.resize(size);
    size_t o = 0;
    while (o < size) {
        if (p >= n) return false;
        const u32 flags = d[p++];
        for (u32 bit = 0x80; bit && o < size; bit >>= 1) {
            if (!(flags & bit)) {
                if (p >= n) return false;
                out[o++] = d[p++];
                continue;
            }
            if (p >= n) return false;
            const u32 top = d[p] >> 4;
            if ((top == 0 ? 3u : top == 1 ? 4u : 2u) > n - p) return false;
            size_t length, back;
            if (top == 0) {
                length = ((d[p] & 15) << 4 | d[p + 1] >> 4) + 0x11;
                back = ((d[p + 1] & 15) << 8 | d[p + 2]) + 1;
                p += 3;
            } else if (top == 1) {
                length = ((d[p] & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111;
                back = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1;
                p += 4;
            } else {
                length = top + 1;
                back = ((d[p] & 15) << 8 | d[p + 1]) + 1;
                p += 2;
            }
            if (back > o || length > size - o) return false;
            for (; length; length--, o++) out[o] = out[o - back];  // a byte at a time: a copy may run into what it writes
        }
    }
    return true;
}

// The record a draw would log once the command list at d[at] had run. The list is followed from there, the start of its code transfer,
// to the jump that ends it, to another program's code transfer, or to words that are no command. Where a list starts isn't known here, so
// what it wrote before the transfer isn't either: Game Freak's lists set everything after it, and one that set the entry point or the
// outputs first gives no record. Bool and int uniforms the list doesn't write are zero.
// Empty if that leaves no whole vertex program: code, swizzles, an entry point inside the code, and the outputs.
std::string list_record(const u8 *d, size_t n, size_t at) {
    constexpr u32 kCodeIdx = R::VS + R::SH_CODE_IDX, kCode = R::VS + R::SH_CODE_DATA, kSwzIdx = R::VS + R::SH_SWZ_IDX, kSwz = R::VS + R::SH_SWZ_DATA;
    constexpr u32 kEntry = R::VS + R::SH_ENTRY, kOutMask = R::VS + R::SH_OUTMASK;
    u32 regs[0x300] = {}, code[512], swz[128], nc = 0, ns = 0;  // (128 swizzles: all an instruction can name)
    bool written[0x300] = {};
    bool done = false;
    for (size_t p = at; !done && p + 8 <= n;) {
        const u32 value = le32at(d + p), head = le32at(d + p + 4);
        const u32 id = head & 0xffff, mask = (head >> 16) & 0xf, count = (head >> 20) & 0xff;
        const bool group = head >> 31;
        if (id >= 0x300 || (head & 0x70000000)) break;  // (bits no header has)
        p += 8;
        if (count > (n - p) / 4) break;
        u32 m = 0;
        for (u32 i = 0; i < 4; i++)
            if (mask & (1u << i)) m |= 0xffu << (8 * i);
        for (u32 k = 0; k <= count; k++) {
            const u32 r = group ? id + k : id, v = k ? le32at(d + p + 4 * (k - 1)) : value;
            if (r >= 0x300) continue;
            if (r == kCodeIdx && nc && !(v & m)) done = true;  // the next program's
            if (done) break;
            regs[r] = (regs[r] & ~m) | (v & m);
            written[r] = true;
            if (r >= kCode && r < kCode + 8) {
                u32 &off = regs[kCodeIdx];
                if (off < 512) {
                    code[off++] = v;
                    if (off > nc) nc = off;
                }
            } else if (r >= kSwz && r < kSwz + 8) {
                u32 &off = regs[kSwzIdx];
                if (off < 128) {
                    swz[off++] = v;
                    if (off > ns) ns = off;
                }
            } else if (r == R::CMDBUF_JUMP0 || r == R::CMDBUF_JUMP1) {
                done = true;
            }
        }
        p += 4 * (count + (count & 1));  // a command is a whole number of word pairs
    }
    // Without the zeros at its end: the memories start as zeros, so a run's copy of the first program it loads ends before them, and
    // builder/shaderlogs.py knows a run's program by how its memory starts. (A later copy goes on with a longer program's tail; no key reads that.)
    while (nc && !code[nc - 1]) nc--;
    while (ns && !swz[ns - 1]) ns--;
    const u32 entry = regs[kEntry] & 0xffff, total = regs[R::VS_OUT_TOTAL] & 7;
    bool whole = ns && entry < nc && written[kEntry] && written[kOutMask] && written[R::VS_OUT_TOTAL];
    for (u32 a = 0; a < total; a++) whole = whole && written[R::VS_OUT_MAP + a];
    if (!whole) return {};

    // which output register's component feeds each slot, as runtime/gpu/vertex.cpp works it out for a draw: setup_outmap, then the
    // registers the mask enables, in order
    s8 slot[24];
    memset(slot, -1, sizeof slot);
    for (u32 a = 0; a < total; a++)
        for (u32 c = 0; c < 4; c++) {
            const u32 sem = (regs[R::VS_OUT_MAP + a] >> (8 * c)) & 0x1f;
            if (sem < 24) slot[sem] = (s8)(a * 4 + c);
        }
    u32 out_reg[16], nout = 0;
    for (u32 r = 0; r < 16; r++)
        if (regs[kOutMask] & (1u << r)) out_reg[nout++] = r;
    for (s8 &s : slot) s = s >= 0 && (u32)(s >> 2) < nout ? (s8)(out_reg[s >> 2] * 4 + (s & 3)) : -1;

    std::string rec("VS  ", 4);
    const auto word = [&](u32 w) {
        for (u32 i = 0; i < 4; i++) rec += (char)(w >> (8 * i));
    };
    word(entry);
    word(nc);
    word(ns);
    for (u32 i = 0; i < nc; i++) word(code[i]);
    for (u32 i = 0; i < ns; i++) word(swz[i]);
    for (u32 i = 0; i < 16; i++) rec += (char)((regs[R::VS + R::SH_BOOL] >> i) & 1);
    for (u32 i = 0; i < 4; i++) word(regs[R::VS + R::SH_INT + i]);
    rec.append((const char *)slot, sizeof slot);
    return rec;
}

struct Found {
    std::set<std::string> seen;
    std::string records;  // each different one, in the order met
    std::ofstream dvlb, materials;
    u64 archives = 0, entries = 0, bytes = 0, lists = 0;
};

// every code transfer in this memory: the vertex shader's code index set to 0, all four bytes of it, on a word boundary
void scan(const u8 *d, size_t n, Found &found) {
    found.bytes += n;
    for (size_t at = 0; at + 8 <= n; at += 4) {
        if (le32at(d + at + 4) != (0x000f0000 | (R::VS + R::SH_CODE_IDX)) || le32at(d + at)) continue;
        const std::string rec = list_record(d, n, at);
        if (rec.empty()) continue;
        found.lists++;
        if (found.seen.insert(rec).second) found.records += rec;
    }
}

// CRILAYLA stores the first 256 decoded bytes after its bitstream. The remaining bytes decode backwards.
bool uncrilayla(const std::vector<u8> &packed, size_t body, std::vector<u8> &out) {
    size_t disk = packed.size() - kCriPrefix;
    out.resize(kCriPrefix + body);
    memcpy(out.data(), packed.data() + disk, kCriPrefix);
    size_t remaining = body;
    u32 value = 0, bits = 0;
    const auto read_bits = [&](u32 n) -> int {
        while (bits < n) {
            if (!disk) return -1;
            value = (value << 8) | packed[--disk];
            bits += 8;
        }
        bits -= n;
        return (value >> bits) & ((1u << n) - 1);
    };
    while (remaining) {
        const int token = read_bits(1);
        if (token < 0) return false;
        const size_t pos = kCriPrefix + remaining - 1;
        if (!token) {
            const int byte = read_bits(8);
            if (byte < 0) return false;
            out[pos] = (u8)byte;
            --remaining;
            continue;
        }
        const int offset = read_bits(13);
        if (offset < 0 || (size_t)offset + 3 > body - remaining) return false;
        const size_t back = (size_t)offset + 3;
        size_t count = 3;
        for (u32 width = 2;; width = width == 2 ? 3 : width == 3 ? 5 : 8) {
            const int step = read_bits(width);
            if (step < 0) return false;
            count += step;
            if (count > remaining) return false;
            if ((u32)step != (1u << width) - 1) break;
        }
        for (; count; --count, --remaining) {
            const size_t dst = kCriPrefix + remaining - 1;
            out[dst] = out[dst + back];
        }
    }
    return true;
}

void put_le32(u8 *dest, u32 value) {
    for (unsigned i = 0; i < 4; ++i) dest[i] = (u8)(value >> (i * 8));
}

std::ostream &write_frame(std::ostream &out, const u8 *data, size_t size) {
    u8 length[4];
    put_le32(length, (u32)size);
    return out.write((const char *)length, sizeof length).write((const char *)data, size);
}

void write_materials(const std::vector<u8> &body, std::ofstream &out) {
    if (!out.is_open() || !out || body.size() < 4) return;
    const std::string_view data((const char *)body.data(), body.size());
    if (data.compare(0, 4, "darc") == 0 || data.compare(0, 4, "BG4\0", 4) == 0) {
        write_frame(out, body.data(), body.size());
        return;
    }

    // Match the complete six-stage list and bound its trailing register writes as tev_to_log.py does.
    constexpr std::string_view first("\xc0\x00\x4f\x80", 4);
    constexpr u32 headers[] = {0x804f00c0, 0x804f00c8, 0x804f00d0, 0x804f00d8, 0x804f00f0, 0x804f00f8};
    size_t mtob = data.npos, next_mtob = data.npos;
    std::vector<u8> record;
    for (size_t at = data.find(first); at != data.npos; at = data.find(first, at + 4)) {
        if (at < 4 || at % 4) continue;
        size_t end = 0;
        for (size_t stride : {0x1c, 0x18, 0x20}) {
            if (at + 5 * stride + 20 > data.size()) continue;
            size_t i = 1;
            while (i < std::size(headers) && le32at(body.data() + at + i * stride) == headers[i]) ++i;
            if (i < std::size(headers)) continue;
            end = at + 5 * stride + 20;
            break;
        }
        if (!end) continue;
        for (size_t q = end, i = 0; i < 12 && q + 8 <= data.size(); ++i) {
            const u32 header = le32at(body.data() + q + 4);
            if ((header & 0xffff) > 0x2ff || !(header & 0xf0000)) break;
            end = q + 8;
            const u32 n = (header >> 20) & 255;
            q = end + 4 * (n + (n & 1));
        }

        if (record.empty()) next_mtob = data.find("MTOB");
        while (next_mtob != data.npos && next_mtob + 4 <= at) {
            mtob = next_mtob;
            next_mtob = data.find("MTOB", mtob + 4);
        }
        const size_t start = at >= 0x20 ? at - 0x20 : 0;
        const bool copy_mtob = mtob != data.npos && mtob < start;
        constexpr size_t mtob_size = 24;
        const u32 material_offset = (u32)(at - start + (copy_mtob ? mtob_size : 0));
        const u32 mtob_offset = mtob == data.npos ? 0xffffffff : copy_mtob ? 0 : (u32)(mtob - start);
        record.resize(12);
        memcpy(record.data(), "MTEV", 4);
        put_le32(record.data() + 4, material_offset);
        put_le32(record.data() + 8, mtob_offset);
        if (copy_mtob) record.insert(record.end(), body.data() + mtob, body.data() + mtob + mtob_size);
        record.insert(record.end(), body.data() + start, body.data() + end);
        if (!write_frame(out, record.data(), record.size())) return;
    }
}

void write_dvlbs(const std::vector<u8> &body, std::ofstream &out) {
    if (!out.is_open() || !out) return;
    for (size_t at = 0; at + 16 <= body.size(); ++at) {
        const u8 *d = body.data() + at;
        if (memcmp(d, "DVLB", 4)) continue;
        const u32 count = le32at(d + 4);
        if (!count || count > 256) continue;
        const size_t size = std::min(body.size() - at, kMostDvlb), dvlp = 8 + 4 * count;
        if (dvlp + 4 > size || memcmp(d + dvlp, "DVLP", 4)) continue;
        if (!write_frame(out, d, size)) return;
    }
}

size_t scan_crilayla(Image &img, u64 at, u64 available, std::vector<u8> &packed, std::vector<u8> &body, Found &found) {
    u8 head[16];
    if (available < sizeof head + kCriPrefix || !img.read(at, head, sizeof head) || memcmp(head, kCriMagic, kCriMagicSize)) return 0;
    const size_t unpacked = le32at(head + 8), stored = le32at(head + 12);
    if (unpacked > kMostUnpacked - kCriPrefix || stored > kMostUnpacked - sizeof head - kCriPrefix) return 0;
    const size_t frame_size = sizeof head + stored + kCriPrefix;
    if (frame_size > available || !img.read(at + sizeof head, packed, frame_size - sizeof head) || !uncrilayla(packed, unpacked, body)) return 0;
    ++found.entries;
    found.bytes += body.size();
    write_dvlbs(body, found.dvlb);
    write_materials(body, found.materials);
    return frame_size;
}

void scan_cpk(Image &img, u64 at, u64 size, std::vector<u8> &packed, std::vector<u8> &body, Found &found) {
    constexpr size_t kChunk = 1 << 20;
    std::vector<u8> chunk;
    ++found.archives;
    for (u64 pos = 0; size - pos >= kCriMagicSize;) {
        const size_t read_size = (size_t)std::min<u64>(size - pos, kChunk + kCriMagicSize - 1);
        if (!img.read(at + pos, chunk, read_size)) return;
        size_t i = 0;
        while (i + kCriMagicSize <= read_size) {
            const auto *first = (const u8 *)memchr(chunk.data() + i, kCriMagic[0], read_size - i - kCriMagicSize + 1);
            if (!first) {
                i = read_size - kCriMagicSize + 1;
                break;
            }
            i = first - chunk.data();
            size_t frame_size = 0;
            if (!memcmp(first, kCriMagic, kCriMagicSize))
                frame_size = scan_crilayla(img, at + pos + i, size - pos - i, packed, body, found);
            i += frame_size ? frame_size : 1;
        }
        pos += i;
    }
}

// A GARC archive, of `size` bytes at `at`: "CRAG" and the header's length, where the data starts (0x10), then "OTAF" with an offset for
// each entry into "BTAF", where an entry has a bit for each of its files and (start, end, length) for each bit set. Each file is looked
// through unpacked if it is an LZ11 stream, and as it lies if not.
void scan_archive(Image &img, u64 at, u64 size, Found &found) {
    u8 head[0x18], h[12];
    if (size < sizeof head || !img.read(at, head, sizeof head)) return;
    const u64 fato = le32at(head + 4), data = le32at(head + 0x10);
    if (fato + sizeof h > size || data > size || !img.read(at + fato, h, sizeof h) || memcmp(h, "OTAF", 4)) return;
    const u64 fato_size = le32at(h + 4), count = le16at(h + 8), fatb = fato + fato_size;
    std::vector<u8> offsets, table, packed, body;
    if (fato_size < 12 + 4 * count || fatb + sizeof h > size || !img.read(at + fato + 12, offsets, 4 * count)) return;
    if (!img.read(at + fatb, h, sizeof h) || memcmp(h, "BTAF", 4)) return;
    const u64 fatb_size = le32at(h + 4);
    if (fatb_size < 12 || fatb_size > kMostTable || fatb + fatb_size > size || !img.read(at + fatb + 12, table, fatb_size - 12)) return;
    found.archives++;
    for (u64 i = 0; i < count; i++) {
        u64 p = le32at(&offsets[4 * i]);
        if (p + 4 > table.size()) continue;
        const u32 bits = le32at(&table[p]);
        p += 4;
        for (u32 bit = 0; bit < 32; bit++) {
            if (!(bits & (1u << bit))) continue;
            if (p + 12 > table.size()) break;
            const u64 start = le32at(&table[p]), end = le32at(&table[p + 4]);
            p += 12;
            if (end <= start || data + end > size || end - start > kMostUnpacked || !img.read(at + data + start, packed, end - start)) continue;
            found.entries++;
            const bool expanded = unlz11(packed.data(), packed.size(), body);
            const auto &payload = expanded ? body : packed;
            scan(payload.data(), payload.size(), found);
            if (expanded) write_dvlbs(payload, found.dvlb);
            write_materials(payload, found.materials);
        }
    }
}

// A romfs with no hash layers, as the build extracts one: its header gives the file table (words 7 and 8) and where file data starts
// (word 9); an entry has its file's offset and size at 8 and 16 and its name's length at 28, the name after it.
bool scan_romfs(const char *path, Found &found) {
    Image img;
    u8 head[40];
    if (!img.open(path) || !img.read(0, head, sizeof head) || le32at(head) != sizeof head) return false;
    const u64 files = le32at(head + 28), files_size = le32at(head + 32), base = le32at(head + 36);
    std::vector<u8> table, packed, body;
    if (files_size > kMostTable || !img.read(files, table, files_size)) return false;
    const bool scan_cri = found.dvlb.is_open() || found.materials.is_open();
    for (u64 p = 0; p + 32 <= table.size();) {
        const u64 off = le64at(&table[p + 8]), size = le64at(&table[p + 16]);
        p += 32 + (((u64)le32at(&table[p + 28]) + 3) & ~3ull);
        if (base > img.size || off > img.size - base || size > img.size - base - off) continue;
        const u64 at = base + off;
        u8 magic[4];
        if (size < sizeof magic || !img.read(at, magic, sizeof magic)) continue;
        if (!memcmp(magic, "CRAG", 4)) scan_archive(img, at, size, found);
        else if (scan_cri && !memcmp(magic, "CPK ", 4)) scan_cpk(img, at, size, packed, body, found);
        else if (scan_cri && !memcmp(magic, kCriMagic, sizeof magic)) scan_crilayla(img, at, size, packed, body, found);
    }
    return true;
}
}  // namespace

int lists_main(int argc, char **argv) {
    Found found;
    struct Output {
        const char *flag;
        std::ofstream &stream;
        const char *path = nullptr;
    } outputs[] = {{"--dvlb", found.dvlb}, {"--materials", found.materials}};
    int first_romfs = 3;
    while (first_romfs < argc) {
        auto *output = std::find_if(std::begin(outputs), std::end(outputs), [&](const Output &candidate) {
            return !strcmp(argv[first_romfs], candidate.flag);
        });
        if (output == std::end(outputs)) break;
        if (output->path || first_romfs + 1 >= argc) {
            first_romfs = argc;
            break;
        }
        output->path = argv[first_romfs + 1];
        first_romfs += 2;
    }
    if (argc <= first_romfs) {
        fprintf(stderr, "usage: shadergen --lists <out.bin> [--dvlb <dvlb-stream>] [--materials <material-stream>] <romfs.bin>...\n");
        return 2;
    }
    for (auto &output : outputs) {
        if (!output.path) continue;
        output.stream.open(output.path, std::ios::binary);
        if (!output.stream) {
            fprintf(stderr, "could not write %s\n", output.path);
            return 1;
        }
    }
    for (int i = first_romfs; i < argc; i++)
        if (!scan_romfs(argv[i], found)) fprintf(stderr, "%s: not a romfs, nothing to look through\n", argv[i]);
    for (auto &output : outputs) {
        if (!output.path) continue;
        output.stream.close();
        if (!output.stream) {
            fprintf(stderr, "could not write %s\n", output.path);
            return 1;
        }
    }
    FILE *f = fopen(argv[2], "wb");
    if (!f || fwrite(found.records.data(), 1, found.records.size(), f) != found.records.size() || fclose(f)) {
        fprintf(stderr, "could not write %s\n", argv[2]);
        return 1;
    }
    printf("%llu archives, %llu files in them (%llu MB): %llu command lists set a vertex program up, %zu different\n", (unsigned long long)found.archives,
           (unsigned long long)found.entries, (unsigned long long)(found.bytes >> 20), (unsigned long long)found.lists, found.seen.size());
    return 0;
}
