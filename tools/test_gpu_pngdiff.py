"""Dependency-free PNG comparison fixtures; run directly or with unittest."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / 'third_party/3dsrecomp/tools/gpu_test/pngdiff.py'
SIGNATURE = b'\x89PNG\r\n\x1a\n'


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def png(w, h, rows, color=6, filters=(0,), level=6, interlace=0):
    """Independent fixture encoder, including all five PNG prediction filters."""
    bpp = 3 if color == 2 else 4
    prior = bytes(w * bpp)
    raw = bytearray()
    for y, row in enumerate(rows):
        kind = filters[y % len(filters)]
        raw.append(kind)
        for x, value in enumerate(row):
            a = row[x - bpp] if x >= bpp else 0
            b = prior[x]
            c = prior[x - bpp] if x >= bpp else 0
            p = a + b - c
            paeth = min((abs(p - a), 0, a), (abs(p - b), 1, b), (abs(p - c), 2, c))[2]
            predictor = (0, a, b, (a + b) // 2, paeth)[kind]
            raw.append((value - predictor) & 255)
        prior = row
    header = struct.pack('>IIBBBBB', w, h, 8, color, 0, 0, interlace)
    return SIGNATURE + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw, level)) + chunk(b'IEND', b'')


def module():
    spec = importlib.util.spec_from_file_location('pngdiff_under_test', SCRIPT)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class PngDiffTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def file(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def cli(self, *args):
        return subprocess.run([sys.executable, '-B', str(SCRIPT), *map(str, args)], capture_output=True, text=True)

    def test_cli_reports_pixel_and_size_mismatch(self):
        a = self.file('a.png', png(1, 1, [bytes((1, 2, 3, 255))]))
        b = self.file('b.png', png(1, 1, [bytes((2, 2, 3, 255))]))
        self.assertEqual(self.cli(a, b).returncode, 1, 'pixel mismatch must fail the gate')
        wide = self.file('wide.png', png(2, 1, [bytes((1, 2, 3, 255)) * 2]))
        self.assertEqual(self.cli(a, wide).returncode, 1, 'dimension mismatch must fail the gate')

    def test_import_has_no_cli_or_output(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            m = module()
        self.assertEqual(output.getvalue(), '')
        self.assertTrue(callable(m.main) and callable(m.compare))

    def test_all_filters_and_compression_levels_compare_by_pixels(self):
        m = module()
        rows = [bytes((x * 47 + y * 89) & 255 for x in range(20)) for y in range(5)]
        plain = self.file('plain.png', png(5, 5, rows, level=0))
        for color, source in ((6, rows), (2, [bytes(x for i, x in enumerate(r) if i % 4 != 3) for r in rows])):
            for kind in range(5):
                with self.subTest(color=color, filter=kind):
                    encoded = self.file('filtered.png', png(5, 5, source, color=color, filters=(kind,), level=9))
                    expected = self.file('expected.png', png(5, 5, source, color=color, level=0))
                    decoded = m.load(encoded)
                    self.assertEqual(decoded, m.load(expected))
                    self.assertTrue(m.compare(decoded, m.load(expected))['match'])
        self.assertEqual(m.load(plain), (5, 5, rows))

    def test_rgb_is_opaque_and_matches_rgba(self):
        m = module()
        a = self.file('rgb.png', png(2, 1, [bytes((2, 3, 4, 5, 6, 7))], color=2, filters=(1,)))
        b = self.file('rgba.png', png(2, 1, [bytes((2, 3, 4, 255, 5, 6, 7, 255))]))
        self.assertTrue(m.compare(m.load(a), m.load(b))['match'])
        self.assertEqual(self.cli(a, b).returncode, 0)

    def test_alpha_tolerance_metrics_and_diff_output(self):
        m = module()
        a = (2, 2, [bytes((10, 20, 30, 255)) * 2] * 2)
        rows = list(a[2]); rows[1] = bytes((10, 20, 30, 255, 10, 20, 30, 250))
        b = (2, 2, rows)
        result = m.compare(a, b, diff_path=self.root / 'diff.png')
        self.assertFalse(result['match'])
        self.assertEqual(result['differing_pixels'], 1)
        self.assertEqual(result['pixels_over_tolerance'], 1)
        self.assertEqual(result['channel_max_delta'], [0, 0, 0, 5])
        self.assertEqual(result['max_channel_delta'], 5)
        self.assertEqual(result['bbox'], [1, 1, 1, 1])
        self.assertEqual(m.load(self.root / 'diff.png')[2][1][7], 255)
        self.assertTrue(m.compare(a, b, tolerance=5)['match'])
        self.assertFalse(m.compare(a, b, tolerance=4)['match'])
        self.assertFalse(m.compare(a, (1, 2, [r[:4] for r in rows]))['dimensions_match'])
        for bad in (-1, 256, 1.5, True):
            with self.assertRaises(ValueError):m.compare(a, b, tolerance=bad)

    def test_save_roundtrip_and_cli_tolerance(self):
        m = module()
        a = self.root / 'a.png'; b = self.root / 'b.png'; diff = self.root / 'diff.png'
        m.save(a, 1, 1, [bytes((1, 2, 3, 255))])
        m.save(b, 1, 1, [bytes((1, 4, 3, 255))])
        run = self.cli(a, b, diff, 2)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)['max_channel_delta'], 2)
        self.assertEqual(m.load(diff)[:2], (1, 1))
        self.assertEqual(self.cli(a, b, diff, -1).returncode, 2)
        self.assertEqual(self.cli(a, b, diff, 'bad').returncode, 2)
        with self.assertRaises(ValueError):m.save(a, 2, 1, [b'1234'])

    def test_invalid_signature_crc_truncation_and_structure(self):
        m = module(); valid = png(1, 1, [b'\x01\x02\x03\xff'])
        header = chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
        idat = chunk(b'IDAT', zlib.compress(b'\x00\x01\x02\x03\xff'))
        end = chunk(b'IEND', b'')
        broken_crc = bytearray(valid); broken_crc[29] ^= 1
        cases = [b'not png', valid[:12], valid[:-1], bytes(broken_crc), valid + b'trailing',
                 SIGNATURE + idat + header + end, SIGNATURE + header + header + idat + end,
                 SIGNATURE + header + end, SIGNATURE + header + idat,
                 SIGNATURE + header + chunk(b'WHAT', b'') + idat + end,
                 SIGNATURE + header + chunk(b'IDAT', b'broken') + end,
                 SIGNATURE + header + chunk(b'IDAT', zlib.compress(b'\x00\x01')) + end,
                 SIGNATURE + header + chunk(b'IDAT', zlib.compress(b'\x00' * 6)) + end,
                 SIGNATURE + header + chunk(b'IDAT', zlib.compress(b'\x00' * 5)[:-1]) + end,
                 SIGNATURE + header + chunk(b'IDAT', zlib.compress(b'\x00' * 5) + b'junk') + end,
                 SIGNATURE + header + chunk(b'IDAT', zlib.compress(b'\x05' + b'\0' * 4)) + end]
        for i, data in enumerate(cases):
            with self.subTest(case=i), self.assertRaises(ValueError):m.load(self.file('bad.png', data))

    def test_unsupported_formats_and_bounds(self):
        m = module()
        for w, h, depth, color, compression, method, interlace in (
                (1, 1, 8, 3, 0, 0, 0), (1, 1, 16, 6, 0, 0, 0), (1, 1, 8, 6, 0, 0, 1),
                (0, 1, 8, 6, 0, 0, 0), (2**31, 1, 8, 6, 0, 0, 0), (100000, 100000, 8, 6, 0, 0, 0),
                (1, 1, 8, 6, 1, 0, 0), (1, 1, 8, 6, 0, 1, 0)):
            data = SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, color, compression, method, interlace))
            with self.subTest(header=(w, h, depth, color, compression, method, interlace)), self.assertRaises(ValueError):m.load(self.file('bad.png', data))
        valid = png(1, 1, [b'\0\0\0\xff'])
        self.assertEqual(self.cli(self.file('bad.png', valid[:-1]), self.file('valid.png', valid)).returncode, 2)
        self.assertEqual(self.cli(self.root / 'missing.png', self.root / 'also-missing.png').returncode, 2)

    def test_split_idat_ancillary_chunks_and_transparency_rejection(self):
        m = module()
        header = chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
        compressed = zlib.compress(b'\x00\x01\x02\x03')
        first, last = chunk(b'IDAT', compressed[:3]), chunk(b'IDAT', compressed[3:])
        text, end = chunk(b'tEXt', b'note\0fixture'), chunk(b'IEND', b'')
        image = self.file('split.png', SIGNATURE + header + text + first + last + end)
        self.assertEqual(m.load(image), (1, 1, [b'\x01\x02\x03\xff']))
        for middle in (chunk(b'tRNS', b'\0' * 6), chunk(b'acTL', b'\0' * 8),
                       chunk(b'PLTE', b'\0'), chunk(b'abcd', b'')):
            with self.subTest(chunk=middle[4:8]), self.assertRaises(ValueError):
                m.load(self.file('bad.png', SIGNATURE + header + middle + first + last + end))
        with self.assertRaises(ValueError):
            m.load(self.file('gap.png', SIGNATURE + header + first + text + last + end))

    def test_small_image_rejects_large_inflated_payload_and_file_limit(self):
        m = module()
        header = chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
        image = SIGNATURE + header + chunk(b'IDAT', zlib.compress(bytes(1024 * 1024))) + chunk(b'IEND', b'')
        with self.assertRaises(ValueError):m.load(self.file('overlong.png', image))
        m.MAX_FILE_BYTES = 32
        with self.assertRaises(ValueError):m.load(self.file('file-limit.png', image))


if __name__ == '__main__':
    unittest.main()
