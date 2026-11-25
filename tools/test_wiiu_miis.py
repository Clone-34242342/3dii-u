"""Host regressions for the Wii U Mii database, selector and 3DS database writes."""
import argparse
import binascii
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent


def block(text, first, after):
    start = text.index(first)
    return text[start:text.index(after, start)]


def fixtures(work, root):
    sys.path.insert(0, str(root / "tools"))
    from check_ports import mii_maker_db

    def finish(db):
        db[-2:] = binascii.crc_hqx(db[:-2], 0).to_bytes(2, "big")
        return bytes(db)

    valid = mii_maker_db()
    record = bytearray(valid[8:8 + 0x5C])
    # FFLiCreateID: 0x80 marks a normal Mii and 0x20 marks a temporary Mii.
    # https://github.com/aboood40091/ffl/blob/master/src/FFLiCreateID.cpp
    for slot, flag in ((1, 0x00), (3, 0x20)):
        record[0x0C] = (record[0x0C] & ~0xA0) | flag
        record[0x0F] = slot + 10
        at = 8 + slot * 0x5C
        valid[at:at + 0x5C] = record
    special = 8 + 0x5C
    valid[special + 0x0C:special + 0x10] = bytes(4)
    valid[special + 0x11] = 1
    valid[special + 0x30] |= 1  # a Special Mii must be local-only
    valid[special + 0x2E:special + 0x30] = bytes([128, 128])
    too_tall = 8 + 4 * 0x5C
    valid[too_tall:too_tall + 0x5C] = valid[8:8 + 0x5C]
    valid[too_tall + 0x2E] = 129
    valid = finish(valid)

    empty = bytearray(len(valid))
    empty[:4] = b"FFOC"
    full = bytearray(empty)
    record = bytearray(valid[8:8 + 0x5C])
    record[0x18:0x1A] = ((12 << 1) | (31 << 5) | (11 << 10)).to_bytes(2, "big")
    record[0x1A:0x2E] = "\u00c5\u5730\U0001f600".encode("utf-16-be").ljust(20, b"\0")
    record[0x2E:0x30] = bytes([17, 42])
    for slot in range(3000):
        record[0x0D:0x10] = (slot + 100).to_bytes(3, "big")
        at = 8 + slot * 0x5C
        full[at:at + 0x5C] = record

    (work / "fixtures").mkdir()
    for name, data in {
        "valid": valid,
        "truncated": valid[:-1],
        "extra": valid + b"\0",
        "bad-crc": valid[:-1] + bytes([valid[-1] ^ 1]),
        "bad-magic": finish(bytearray(b"\0" + valid[1:])),
        "empty": finish(empty),
        "full": finish(full),
    }.items():
        (work / "fixtures" / (name + ".dat")).write_bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=HERE.parent)
    parser.add_argument("--fs-source", type=Path, help="override fs.cpp to check a prior implementation")
    parser.add_argument("--frontend-source", type=Path, help="override frontend to check a prior implementation")
    parser.add_argument("--case", help="run one database I/O case")
    parser.add_argument("--only", choices=("database", "picker", "io"))
    args = parser.parse_args()
    root = args.root.resolve()
    runtime = root / "third_party/3dsrecomp/runtime"
    backend = root / "port/wiiu"
    if "CXX" in os.environ:
        cxx = shlex.split(os.environ["CXX"])
    elif os.name == "nt":
        sys.path.insert(0, str(root / "tools/win"))
        from build3ds import HOST_CXX
        cxx = HOST_CXX + ["-static"]
    else:
        cxx = ["c++"]

    with tempfile.TemporaryDirectory(prefix="mii-tests-") as temp:
        work = Path(temp)

        def build(name):
            binary = work / (name + (".exe" if os.name == "nt" else ""))
            subprocess.run([*cxx, "-std=c++20", "-O1", "-pthread", "-I" + str(runtime),
                            "-I" + str(work), str(HERE / (name + ".cpp")), "-o", str(binary)],
                           check=True, timeout=60)
            return binary

        services = (runtime / "services/services.h").read_text(encoding="utf-8")
        (work / "mii_id.inc").write_text(block(services, "inline bool mii_id_empty(", "void register_apt();"), encoding="utf-8")

        if args.only in (None, "database"):
            frontend = (args.frontend_source or backend / "frontend_wiiu.cpp").read_text(encoding="utf-8")
            misc = (runtime / "services/misc.cpp").read_text(encoding="utf-8")
            end = "\nstatic void read_console_mii_maker()" if "static bool read_mii_maker(" in frontend else "\nvoid frontend_run()"
            reader = block(frontend, "static bool looks_like_mii(", end)
            if "static void read_mii_maker(" in reader:
                reader += "\nbool read_db(FILE *f) { read_mii_maker(f); return true; }\n"
            else:
                reader += "\nbool read_db(FILE *f) { return read_mii_maker(f); }\n"
            (work / "mii_reader.inc").write_text(reader, encoding="utf-8")
            (work / "mii_console.inc").write_text(block(
                misc, "std::vector<std::array<u8, 0x60>> g_host_miis;", "// The player's Mii:"), encoding="utf-8")
            fixtures(work, root)
            subprocess.run([str(build("test_wiiu_mii_database"))], cwd=work, check=True, timeout=10)

        if args.only in (None, "picker"):
            apt = (runtime / "services/apt.cpp").read_text(encoding="utf-8")
            gx2 = (backend / "gx2_backend.cpp").read_text(encoding="utf-8")
            (work / "mii_applet.inc").write_text("".join((
                block(apt, "static std::vector<u8> mii_selector_result(", "// Software keyboard"),
                block(apt, "static std::string utf8(", "static std::u16string utf16("),
                block(apt, "// The game's MiiSelectorConf", "static void applet_run("),
            )), encoding="utf-8")
            (work / "mii_requests.inc").write_text(block(
                apt, "std::shared_ptr<const mii_select::Request> mii_select::pending()", "namespace kernel {\n\nvoid apt_notify"), encoding="utf-8")
            (work / "mii_input.inc").write_text(block(
                gx2, "static constexpr float MS_X", "static bool draw_mii_selector("), encoding="utf-8")
            subprocess.run([str(build("test_wiiu_mii_picker"))], cwd=work, check=True, timeout=10)

        if args.only in (None, "io"):
            fs = (args.fs_source or runtime / "services/fs.cpp").read_text(encoding="utf-8")
            (work / "mii_database_under_test.inc").write_text(block(
                fs, "static bool create_gamecoin(", "// ---- fs:USER"), encoding="utf-8")
            binary = build("test_mii_database_io")
            cases = (
                "success", "short-read", "open-denied", "open-missing", "read-close",
                "short-write", "flush", "write-close", "readback-short", "readback-corrupt",
                "readback-close", "backup-rename", "publish-rename", "rollback-rename",
                "late-arrival", "recovery", "extra-byte", "retry", "slow-retry",
                "directory-retry", "create", "guest-save", "full", "coin-open", "coin-write", "coin-flush", "coin-close", "coin-rename",
            )
            for case in (args.case,) if args.case else cases:
                folder = work / case
                folder.mkdir()
                print("Mii database I/O", case, flush=True)
                subprocess.run([str(binary), case], cwd=folder, check=True, timeout=10)
                if case == "guest-save":
                    subprocess.run([str(binary), "guest-reopen"], cwd=folder, check=True, timeout=10)


if __name__ == "__main__":
    main()
