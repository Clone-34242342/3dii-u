"""Self-check for shaderlogs.py's round trips: python builder/test_shaderlogs.py
- A record whose memory starts with a program of the game's DVLB ships as a fingerprint and comes back with the dump's program.
- One laid out from the game's code.bin (plain words, and a swizzle table of plain words) and romfs.bin (a program and every other word of a DVLB's
  table) ships as a recipe and comes back the same from the dump, and not at all from another version of it; one whose words aren't in the dump
  gets neither. Synthetic files, so it runs anywhere."""
import os
import random
import shutil
import struct
import sys
import tempfile
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderlogs  # noqa: E402
from make_app import bundle_logs  # noqa: E402


def vs(entry, code, swz, tail):
    return b'VS  ' + struct.pack('<III', entry, len(code), len(swz)) + struct.pack(f'<{len(code)}I', *code) + struct.pack(f'<{len(swz)}I', *swz) + tail


prog = ([0x11, 0x22, 0x88000000], [0xA, 0xB])  # a program of the game's DVLB
other = ([0x33, 0x44], [0xC])  # one the DVLB scan never found
tail = bytes(range(56))
frag = struct.pack('<I', 3) + b'key' + b'FS  ' + struct.pack('<I', 2) + b'xy'
log = vs(1, prog[0] + [0x99, 0x99], prog[1] + [0x77], tail) + frag + vs(0, *other, tail)  # memory: the program, then another's leftovers

for pending in (b'VS  ' + bytes(7), vs(0, *other, tail)[:-1], frag[:6], b'FS  '):
    assert list(shaderlogs.records(log + pending)) == list(shaderlogs.records(log)), 'keep only complete records'

base = tempfile.mkdtemp()
try:
    shipped, vsf, vsr, left = shaderlogs.split(log, {(struct.pack('<3I', *prog[0]), struct.pack('<2I', *prog[1]))})
    assert shipped == frag, 'fragment records ship as they are'
    assert len(left) == 1 and not vsr and len(vsf) == shaderlogs.VSF and b'\x11\x00\x00\x00' not in vsf, 'one fingerprint, no code; the unknown program left out'
    with open(os.path.join(base, 'dvlb.bin'), 'wb') as f:
        f.write(vs(1, *prog, bytes(56)))
    with open(os.path.join(base, 'run.vsf'), 'wb') as f:
        f.write(vsf)
    assert shaderlogs.restore(base) == (1, 0)
    with open(os.path.join(base, 'run_vsf.bin'), 'rb') as f:
        assert f.read() == vs(1, *prog, tail), 'the record comes back with the dump\'s program and its own uniforms and output map'
    assert not os.path.exists(os.path.join(base, 'run.vsf'))
finally:
    shutil.rmtree(base)


def gsx(vprog, gprog, tail):  # a vertex shader and the geometry shader after it (tools/shadergen_main.cpp)
    words = vprog[0] + vprog[1] + gprog[0] + gprog[1]
    body = (struct.pack('<11I', 1, len(vprog[0]), len(vprog[1]), 2, len(gprog[0]), len(gprog[1]), 0xF, 4, 0x3210, 0, 1)
            + struct.pack(f'<{len(words)}I', *words) + tail)
    return b'GSX ' + struct.pack('<I', len(body)) + body


# a geometry shader record ships as both programs' fingerprints, after the vertex records, and comes back whole; one with a program the
# game's binaries lack is left out. The geometry program is its binary's whole code (dvlb_to_log.py's dvlb.gsl), which the vertex program's
# first 512 words only start: the memory is named by the longest program that starts it, or it came back cut short
gprog = (prog[0] + [0x55, 0x66, 0x88000000, 0x77], prog[1] + [0xD])
gtail = bytes(range(88))
base = tempfile.mkdtemp()
try:
    with open(os.path.join(base, 'dvlb.bin'), 'wb') as f:
        f.write(vs(1, *prog, bytes(56)))
    with open(os.path.join(base, 'dvlb.gsl'), 'wb') as f:
        f.write(shaderlogs._dvlb_tool().gs_record(2, gprog[0], gprog[1]))
    progs = shaderlogs.programs(os.path.join(base, 'dvlb.bin'))
    packed = lambda p: (struct.pack(f'<{len(p[0])}I', *p[0]), struct.pack(f'<{len(p[1])}I', *p[1]))  # noqa: E731
    assert progs == {packed(prog), packed(gprog)}, 'the vertex program and the whole geometry program'
    log = gsx(prog, gprog, gtail) + vs(1, *prog, tail) + gsx(prog, other, gtail)
    shipped, vsf, vsr, left = shaderlogs.split(log, progs)
    assert not shipped and not vsr and not left and len(vsf) == shaderlogs.VSF + shaderlogs.GSF, len(vsf)
    assert vsf.startswith(b'VSF ') and vsf[shaderlogs.VSF:].startswith(b'GSF ') and b'\x55\x00\x00\x00' not in vsf, 'fingerprints, no code'
    assert shaderlogs.fingerprint(*packed(gprog)) in vsf, 'the geometry memory named by the whole program'
    with open(os.path.join(base, 'run.vsf'), 'wb') as f:
        f.write(vsf)
    assert shaderlogs.restore(base) == (2, 0)
    with open(os.path.join(base, 'run_vsf.bin'), 'rb') as f:
        assert f.read() == vs(1, *prog, tail) + gsx(prog, gprog, gtail), 'both records back as they were logged'

    src, dst = Path(base) / 'source', Path(base) / 'bundled'
    logs = src / 'shader_logs'
    logs.mkdir(parents=True)
    for name in ('dvlb.bin', 'dvlb.gsl'):
        shutil.copyfile(Path(base) / name, logs / name)
    extra = gsx(prog, gprog, b'\x01' + gtail[1:])
    captured = shaderlogs.split(extra, progs)[1]
    (logs / 'run.bin').write_bytes(log)
    (logs / 'run.vsf').write_bytes(vsf + captured + b'GSF ')
    (logs / 'standalone.vsf').write_bytes(captured)
    assert bundle_logs(src, dst) == (0, 0)
    bundled = dst / 'shader_logs'
    assert (bundled / 'run.vsf').read_bytes() == vsf + captured
    assert (bundled / 'standalone.vsf').read_bytes() == captured
    assert not (bundled / 'dvlb.bin').exists()
    for name in ('dvlb.bin', 'dvlb.gsl'):
        shutil.copyfile(logs / name, bundled / name)
    assert shaderlogs.restore(bundled) == (4, 0)
    assert (bundled / 'run_vsf.bin').read_bytes() == vs(1, *prog, tail) + gsx(prog, gprog, gtail) + extra
    assert (bundled / 'standalone_vsf.bin').read_bytes() == extra
finally:
    shutil.rmtree(base)

rnd = random.Random(9)


def words(n):
    return b''.join(struct.pack('<I', rnd.getrandbits(32)) for _ in range(n))


with tempfile.TemporaryDirectory() as root:
    src, dst = Path(root) / 'local', Path(root) / 'bundle'
    logs = src / 'shader_logs'
    logs.mkdir(parents=True)
    first, second = (words(8), words(4)), (words(8), words(4))
    code, swz = first[0] + second[0][-16:], first[1] + second[1][-8:]
    captured = shaderlogs.vs_record(1, code, swz, tail)
    choices = {first, second}
    found = b''.join(shaderlogs.vs_record(0, *p, tail) for p in (first, second))
    (src / 'code.bin').write_bytes(words(8))
    (logs / 'dvlb.bin').write_bytes(found)
    (logs / 'capture.bin').write_bytes(captured)
    made, missed = shaderlogs.program_recipes(captured, choices)
    assert len(list(shaderlogs.recipes(made))) == 1 and missed == 0
    assert shaderlogs.program_recipes(captured, {first}) == (b'', 1)
    assert shaderlogs.write_recipes(src, logs) == (1, 0)
    assert (logs / 'recipes.vsr').read_bytes() == made
    assert bundle_logs(src, dst) == (1, 0)
    bundled = dst / 'shader_logs'
    assert not (bundled / 'capture.vsf').exists(), 'a shorter prefix would discard live instructions'
    (bundled / 'dvlb.bin').write_bytes(found)
    assert shaderlogs.restore(bundled) == (1, 0)
    assert (bundled / 'recipes_vsr.bin').read_bytes() == captured
    (bundled / 'recipes.vsr').write_bytes(made)
    (bundled / 'dvlb.bin').write_bytes(shaderlogs.vs_record(0, *first, tail))
    assert shaderlogs.restore(bundled) == (0, 1)


tmp = tempfile.mkdtemp(prefix='shaderlogs_test_')
try:
    game = os.path.join(tmp, 'game')
    os.makedirs(os.path.join(game, 'shader_logs'))
    first, gap, engine, table = words(1), words(1), words(40), words(6)  # the engine's shader in code.bin: a word, another, then the rest
    code_bin = bytearray(words(30000))
    code_bin[40000:40000 + 4 * 42] = first + gap + engine
    code_bin[40400:40400 + 28] = table[:4] + table[4:]  # its swizzles, plain
    program, dvle_swz = words(60), words(5)
    romfs = bytearray(words(250000))
    romfs[600000:600000 + 240] = program
    for j in range(5):  # a DVLB's table: each swizzle with a word after it
        romfs[600400 + 8 * j:600404 + 8 * j] = dvle_swz[4 * j:4 * j + 4]
    for name, data in (('code.bin', code_bin), ('romfs.bin', romfs)):
        with open(os.path.join(game, name), 'wb') as f:
            f.write(data)

    code, swz, tail = first + engine + program, table + dvle_swz, words(14)
    stranger = words(80)  # another game's memory, in a mixed log
    log = (b'VS  ' + struct.pack('<III', 3, len(code) // 4, len(swz) // 4) + code + swz + tail
           + b'VS  ' + struct.pack('<III', 0, 60, 20) + stranger + tail
           + b'FS  ' + struct.pack('<I', 8) + words(2))
    srcs = shaderlogs.sources(game)
    frag, vsf, vsr, left = shaderlogs.split(log, set(), srcs)
    shaderlogs.close(srcs)
    got = list(shaderlogs.recipes(vsr))
    assert left == [(0, stranger[:240], stranger[240:], tail)] and len(got) == 1 and not vsf and frag.startswith(b'FS  '), (len(left), len(got))
    entry, fp, t, pc, ps = got[0]
    assert entry == 3 and t == tail and fp == shaderlogs.fingerprint(code, swz)
    assert len(pc) <= 4 and len(ps) <= 4, (pc, ps)  # a few runs, not a word at a time
    assert all(at < 40000 + 4096 or at >= 600000 for _, _, _, at in pc + ps)  # each piece where that part really is

    with open(os.path.join(game, 'shader_logs', 'x.vsr'), 'wb') as f:
        f.write(vsr)
    assert shaderlogs.restore(os.path.join(game, 'shader_logs')) == (1, 0)
    back = open(os.path.join(game, 'shader_logs', 'x_vsr.bin'), 'rb').read()
    assert back == log[:len(back)] and len(back) == 16 + len(code) + len(swz) + len(tail), 'the record comes back as it was logged'

    code_bin[40000 + 4 * 10] ^= 1  # another version of the game: the record is left out, not made wrong
    with open(os.path.join(game, 'code.bin'), 'wb') as f:
        f.write(code_bin)
    with open(os.path.join(game, 'shader_logs', 'x.vsr'), 'wb') as f:
        f.write(vsr)
    assert shaderlogs.restore(os.path.join(game, 'shader_logs')) == (0, 1)
    assert list(shaderlogs.recipes(vsr[:-3])) == []  # a cut file gives nothing

    # a shader kept in a compressed romfs file (Zelda's SPBD3 in RegionBoot.szs): found unpacked by --recipes, unpacked again by the build
    packed_prog, packed_swz = words(50), words(6)
    inner = words(3000) + packed_prog + words(10) + packed_swz + words(100)
    yaz0 = bytearray(b'Yaz0' + struct.pack('>I', len(inner)) + bytes(8))
    for j in range(0, len(inner), 8):  # every byte a literal
        yaz0 += b'\xff' + inner[j:j + 8]
    head = struct.pack('<10I', 0x28, 0, 0, 0, 0, 0, 0, 0x28, 32, 0x80)
    entry_meta = struct.pack('<IIQQII', 0, 0xFFFFFFFF, 16, len(yaz0), 0xFFFFFFFF, 0)  # one file, no name, 16 bytes into the data
    romfs2 = head + entry_meta + bytes(0x80 - 0x28 - 32) + bytes(16) + yaz0
    game2 = os.path.join(tmp, 'game2')
    os.makedirs(os.path.join(game2, 'shader_logs'))
    with open(os.path.join(game2, 'romfs.bin'), 'wb') as f:
        f.write(romfs2)
    with open(os.path.join(game2, 'shader_logs', 'run.bin'), 'wb') as f:
        f.write(b'VS  ' + struct.pack('<III', 0, 50, 6) + packed_prog + packed_swz + tail)
    assert shaderlogs.write_recipes(game2, os.path.join(game2, 'shader_logs')) == (1, 0)
    assert shaderlogs.restore(os.path.join(game2, 'shader_logs')) == (1, 0)
    assert open(os.path.join(game2, 'shader_logs', 'recipes_vsr.bin'), 'rb').read()[16:16 + 224] == packed_prog + packed_swz

    # Harvest two overlapping logs: raw, compressed, mixed raw/compressed, and foreign memories.
    # The old two-pass algorithm is the output oracle, including record order and duplicates.
    raw_code, raw_swz = words(40), words(6)
    unknown_code, unknown_swz = words(40), words(6)
    with open(os.path.join(game2, 'code.bin'), 'wb') as f:
        f.write(raw_code + words(10) + raw_swz)
    memories = [(raw_code, raw_swz), (packed_prog, packed_swz),
                (raw_code + packed_prog, raw_swz + packed_swz), (unknown_code, unknown_swz)]
    harvest_log = b''.join(shaderlogs.vs_record(0, c, s, tail) for c, s in memories)
    logs = os.path.join(game2, 'shader_logs')
    # Ignore prior round-trip output by replacing it with this same known fixture.
    for name in ('run.bin', 'recipes_vsr.bin'):
        with open(os.path.join(logs, name), 'wb') as f:
            f.write(harvest_log)
    oracle = shaderlogs.sources(game2)
    oracle._raw_runs = None  # original algorithm searched rather than memoizing raw runs
    try:
        missed = shaderlogs.split(harvest_log, set(), oracle)[3]
        unpacked = oracle.holding([p for _, c, _, _ in missed for p in shaderlogs._probes(c)])
        expected = shaderlogs.split(harvest_log, set(), oracle, unpacked=unpacked)
        assert len(list(shaderlogs.recipes(expected[2]))) == 3 and len(expected[3]) == 1
    finally:
        shaderlogs.close(oracle)

    opened, layouts, searches, probe_calls, holdings = [], [], [], [], []
    real_sources, real_layout = shaderlogs.sources, shaderlogs._layout
    real_anywhere, real_probes, real_holding = shaderlogs._run_anywhere, shaderlogs._probes, shaderlogs.Sources.holding

    def track_sources(folder):
        src = real_sources(folder)
        opened.append(src)
        return src

    def track_layout(c, s, bufs, *args, **kwargs):
        layouts.append((c, s, len(bufs)))
        return real_layout(c, s, bufs, *args, **kwargs)

    def track_search(buf, ws, i):
        searches.append((id(buf), b''.join(ws), i))
        return real_anywhere(buf, ws, i)

    def track_probes(c):
        probe_calls.append(c)
        return real_probes(c)

    def track_holding(src, probes):
        holdings.append(list(probes))
        return real_holding(src, probes)

    with mock.patch.object(shaderlogs, 'sources', side_effect=track_sources), \
         mock.patch.object(shaderlogs, '_layout', side_effect=track_layout), \
         mock.patch.object(shaderlogs, '_run_anywhere', side_effect=track_search), \
         mock.patch.object(shaderlogs, '_probes', side_effect=track_probes), \
         mock.patch.object(shaderlogs.Sources, 'holding', track_holding):
        assert shaderlogs.write_recipes(game2, logs) == (6, 2)
    with open(os.path.join(logs, 'recipes.vsr'), 'rb') as f:
        assert f.read() == expected[2] * 2, 'same recipes, fingerprints, order, and duplicates as the old two passes'
    assert len(opened) == len(holdings) == 1, 'source mapping and unpacked-file search happen once'
    assert all(layouts.count((c, s, 2)) == 1 for c, s in memories), 'each unique memory searches raw layout once'
    assert len(layouts) == 6, 'only compressed and mixed memories need a combined layout; foreign probes match no file'
    assert all(probe_calls.count(c) == 2 for c, _ in memories[1:]) and raw_code not in probe_calls
    raw_ids = {id(buf) for buf in opened[0]}
    raw_searches = [key for key in searches if key[0] in raw_ids]
    assert len(raw_searches) == len(set(raw_searches)), 'combined retry reuses each raw whole-file search, including misses'
    assert shaderlogs.restore(logs) == (6, 0), 'all retained and newly resolved recipes still reconstruct exactly'

    # No match after unpacking must still remove an obsolete recipe, as before.
    for name in ('run.bin', 'recipes_vsr.bin'):
        with open(os.path.join(logs, name), 'wb') as f:
            f.write(shaderlogs.vs_record(0, unknown_code, unknown_swz, tail))
    with open(os.path.join(logs, 'recipes.vsr'), 'wb') as f:
        f.write(expected[2])
    assert shaderlogs.write_recipes(game2, logs) == (0, 2)
    assert not os.path.exists(os.path.join(logs, 'recipes.vsr'))

    # Many misses must not retain a separate near-full shader suffix for every word offset.
    missing_code = words(512)
    empty_sources = shaderlogs.Sources([bytes(64), bytes(64)])
    assert shaderlogs.recipe(missing_code, words(6), empty_sources) is None
    retained = {id(key[1]): key[1] for key in empty_sources._raw_runs}
    assert len(empty_sources._raw_runs) > 2 and sum(map(len, retained.values())) <= len(missing_code)

    # a finished game's list (confirm): a build's fragment states once each, an earlier list's first, without a miss list's tags, the
    # vertex records, a file with another extension, or the last record of a log cut short
    def state(name):
        return struct.pack('<I', len(name)) + name

    built, kept = os.path.join(tmp, 'built'), os.path.join(tmp, 'kept')
    os.makedirs(built)
    os.makedirs(kept)
    for name, data in (('a.bin', state(b'state one') + vs(0, *other, tail) + b'FS  ' + state(b'state two')),
                       ('confirmed.bin', state(b'state old') + state(b'state two')), ('variants.bin', state(b'state guessed')),
                       ('cut.bin', state(b'state three') + state(b'state cut short')[:-3]), ('notes.txt', state(b'state four'))):
        with open(os.path.join(built, name), 'wb') as f:
            f.write(data)
    assert shaderlogs.confirm(built, kept) == 5
    with open(os.path.join(kept, 'confirmed.bin'), 'rb') as f:
        made = f.read()
    first = state(b'state old') + state(b'state two')
    assert made.startswith(first), 'the earlier list comes first, in its own order'
    rest = {r[1] for r in shaderlogs.records(made[len(first):])}
    assert rest == {state(b'state one'), state(b'state guessed'), state(b'state three')} and len(made) == len(first) + sum(map(len, rest)), rest
    assert shaderlogs.confirm(built, built) == 5, 'in the build\'s own folder too'
    with open(os.path.join(built, 'confirmed.bin'), 'rb') as f:
        assert f.read() == made
    open(os.path.join(game2, 'shader_logs', 'variants.bin'), 'wb').close()
    for folder, why in ((kept, 'a folder no build left'), (os.path.join(game2, 'shader_logs'), 'logs with vertex records alone')):
        try:
            shaderlogs.confirm(folder, kept)
        except SystemExit:
            continue
        raise AssertionError(why + ' was confirmed')
finally:
    assert os.path.dirname(os.path.realpath(tmp)) == os.path.realpath(tempfile.gettempdir())
    assert os.path.basename(tmp).startswith('shaderlogs_test_')
    shutil.rmtree(tmp, ignore_errors=True)
print('shaderlogs ok: fingerprints, recipes and a finished game\'s list')
