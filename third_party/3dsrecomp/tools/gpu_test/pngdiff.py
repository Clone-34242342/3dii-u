#!/usr/bin/env python3
"""Compare raw PNG samples: RGB8/RGBA8, noninterlaced, filters 0–4, no color management.
CLI: ref.png actual.png [diff.png [threshold]]; exits 0 match, 1 mismatch, 2 invalid.
load returns (width, height, RGBA byte rows); compare returns JSON-compatible metrics.
"""
import argparse
import json
import struct
import sys
import zlib

SIGNATURE = b'\x89PNG\r\n\x1a\n'
MAX_BYTES = 64 * 1024 * 1024  # decoded RGBA including row filter bytes
MAX_FILE_BYTES = 2 * MAX_BYTES


class PNGError(ValueError):
    """Malformed, unsupported or excessively large PNG."""


def _dimensions(w, h):
    if type(w) is not int or type(h) is not int or not (0 < w < 2**31 and 0 < h < 2**31):
        raise PNGError('invalid PNG dimensions')
    if (w * 4 + 1) * h > MAX_BYTES:
        raise PNGError('PNG exceeds decoded image limit (64 MiB)')


def load(path):
    with open(path, 'rb') as f:
        data = f.read(MAX_FILE_BYTES + 1)
    if len(data) > MAX_FILE_BYTES or not data.startswith(SIGNATURE):
        raise PNGError('invalid PNG signature or file exceeds 128 MiB')
    pos, header, idat, idat_done, palette = 8, None, [], False, False
    while pos < len(data):
        if len(data) - pos < 12:
            raise PNGError('truncated PNG chunk')
        n, kind = struct.unpack_from('>I4s', data, pos)
        end = pos + 12 + n
        if n >= 2**31 or end > len(data):
            raise PNGError('invalid or truncated PNG chunk length')
        body = data[pos + 8:pos + 8 + n]
        if not all(65 <= c <= 90 or 97 <= c <= 122 for c in kind) or kind[2] & 32:
            raise PNGError('invalid PNG chunk type')
        if zlib.crc32(kind + body) != struct.unpack_from('>I', data, end - 4)[0]:
            raise PNGError(f'CRC mismatch in {kind.decode("ascii")}')
        if header is None and kind != b'IHDR':
            raise PNGError('IHDR must be first')
        if kind == b'IHDR':
            if header is not None or n != 13:
                raise PNGError('invalid or duplicate IHDR')
            w, h, depth, color, compression, method, interlace = struct.unpack('>IIBBBBB', body)
            _dimensions(w, h)
            if (depth, color) not in ((8, 2), (8, 6)) or compression or method or interlace:
                raise PNGError('only noninterlaced RGB8/RGBA8 with standard PNG compression/filtering is supported')
            header = w, h, 3 if color == 2 else 4
        elif kind == b'IDAT':
            if idat_done:
                raise PNGError('IDAT chunks must be consecutive')
            idat.append(body)
        elif kind == b'IEND':
            if n or not idat or end != len(data):
                raise PNGError('invalid IEND, missing IDAT or trailing data')
            break
        else:
            if idat:
                idat_done = True
            if kind == b'PLTE':
                if palette or idat or not n or n > 768 or n % 3:
                    raise PNGError('invalid PLTE')
                palette = True  # optional suggested palette; truecolor pixels remain unchanged
            elif kind in (b'tRNS', b'acTL', b'fcTL', b'fdAT') or not kind[0] & 32:
                raise PNGError(f'unsupported PNG chunk {kind.decode("ascii")}')
        pos = end
    else:
        raise PNGError('missing IEND')
    w, h, bpp = header
    stride, expected = w * bpp, (w * bpp + 1) * h
    decoder = zlib.decompressobj()
    try:
        raw = decoder.decompress(b''.join(idat), expected + 1)
    except zlib.error as e:
        raise PNGError(f'invalid PNG compressed data: {e}') from e
    if len(raw) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise PNGError('PNG decompressed size or zlib stream does not match IHDR')
    rows, previous = [], bytes(stride)
    for y in range(h):
        offset = y * (stride + 1)
        kind, row = raw[offset], bytearray(raw[offset + 1:offset + stride + 1])
        if kind > 4:
            raise PNGError(f'unsupported PNG row filter {kind}')
        if kind:
            for x in range(stride):
                a, b, c = row[x - bpp] if x >= bpp else 0, previous[x], previous[x - bpp] if x >= bpp else 0
                if kind == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
                else:
                    predictor = a if kind == 1 else b if kind == 2 else (a + b) // 2
                row[x] = (row[x] + predictor) & 255
        previous = row
        if bpp == 3:
            rgba = bytearray(b'\xff' * (w * 4))
            for x in range(w):
                rgba[x * 4:x * 4 + 3] = row[x * 3:x * 3 + 3]
            row = rgba
        rows.append(bytes(row))
    return w, h, rows


def save(path, w, h, rows):
    """Write the replay writer's RGBA8/filter-0 subset."""
    _dimensions(w, h)
    if len(rows) != h or any(len(r) != w * 4 for r in rows):
        raise PNGError('RGBA rows do not match image dimensions')
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    png = SIGNATURE + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    with open(path, 'wb') as f:
        f.write(png + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def compare(reference, actual, tolerance=0, diff_path=None):
    """Each RGBA channel may differ by at most tolerance; dimensions must match exactly.
    bbox encloses all unequal pixels, including differences accepted by tolerance.
    A dimension mismatch has no pixel metrics and writes no difference image.
    """
    if type(tolerance) is not int or not 0 <= tolerance <= 255:
        raise PNGError('tolerance must be an integer from 0 to 255')
    w, h, a = reference
    wb, hb, b = actual
    for width, height, rows in (reference, actual):
        _dimensions(width, height)
        if len(rows) != height or any(len(r) != width * 4 for r in rows):
            raise PNGError('RGBA rows do not match image dimensions')
    result = dict(match=False, dimensions_match=(w, h) == (wb, hb), reference_size=[w, h], actual_size=[wb, hb],
                  tolerance=tolerance, differing_pixels=None, pixels_over_tolerance=None, max_channel_delta=None, channel_max_delta=None, bbox=None)
    if not result['dimensions_match']:
        return result
    different, over, maxima, box, visual = 0, 0, [0] * 4, None, []
    for y, (ra, rb) in enumerate(zip(a, b)):
        row = bytearray(ra) if diff_path is not None else None
        for x in range(w):
            start = x * 4
            delta = [abs(ra[start + ch] - rb[start + ch]) for ch in range(4)]
            maxima = [max(old, new) for old, new in zip(maxima, delta)]
            d = max(delta)
            if row is not None:
                row[start:start + 3] = bytes(v // 4 for v in ra[start:start + 3])
            if d:
                different += 1
                box = [x, y, x, y] if box is None else [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)]
            if d > tolerance:
                over += 1
                if row is not None:
                    row[start:start + 4] = bytes((255, max(0, 255 - d * 2), 0, 255))
        if row is not None:
            visual.append(row)
    if diff_path is not None:
        save(diff_path, w, h, visual)
    result.update(match=over == 0, differing_pixels=different, pixels_over_tolerance=over,
                  max_channel_delta=max(maxima), channel_max_delta=maxima, bbox=box)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference'); parser.add_argument('actual')
    parser.add_argument('diff', nargs='?'); parser.add_argument('threshold', nargs='?', type=int, default=0)
    args = parser.parse_args(argv)
    try:
        result = compare(load(args.reference), load(args.actual), args.threshold, args.diff)
    except (OSError, PNGError) as e:
        print(f'PNG comparison error: {e}', file=sys.stderr)
        return 2
    print(json.dumps(result, sort_keys=True))
    return 0 if result['match'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
