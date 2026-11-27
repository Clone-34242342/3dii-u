"""Verify six raw fixture frames against independent analytical PNGs, exactly."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'third_party/3dsrecomp/tools/gpu_test'))
import compare
import pngdiff

CASES = {'clear': (400, 240, None), 'triangle': (400, 240, (255, 0, 0, 255)),
         'depth': (400, 240, (0, 255, 0, 255)), 'texture': (400, 240, None),
         'blend': (400, 240, (80, 64, 64, 255)), 'bottom_clear': (320, 240, None)}
BACKGROUND = bytes((16, 32, 48, 255))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_analytical(folder):
    folder.mkdir(exist_ok=False)
    for name, (width, height, color) in CASES.items():
        # Clip coordinates Â±3/4 map to a centered rectangle occupying3/4 of both
        # axes. After LCD rotation its bounds are x=[50,350), y=[30,210).
        # Pixel centers never lie on its four outer boundaries; no masks/tolerance.
        if name == 'texture':
            # On the rotated LCD, U increases upward and V increases leftward.
            border = BACKGROUND * 50
            upper = border + bytes((0, 255, 0, 255)) * 150 + bytes((255, 255, 0, 255)) * 150 + border
            lower = border + bytes((255, 0, 0, 255)) * 150 + bytes((0, 0, 255, 255)) * 150 + border
            blank = [BACKGROUND * width] * 30
            rows = blank + [upper] * 90 + [lower] * 90 + blank
        else:
            rows = [b''.join(bytes(color) if color and 50 <= x < 350 and 30 <= y < 210
                             else BACKGROUND for x in range(width)) for y in range(height)]
        pngdiff.save(folder / (name + '.png'), width, height, rows)


def verify(folder, backend, reference, png_folder):
    result = json.loads((folder / 'result.json').read_text())
    if result.get('version') != 1 or result.get('pass') is not True:
        raise ValueError('missing/failed fixture result')
    frames = result.get('frames', [])
    if len(frames) != len(CASES) or {frame['name'] for frame in frames} != set(CASES):
        raise ValueError('missing, duplicate, or unsupported frame')
    if {path.stem for path in folder.glob('*.rgba')} != set(CASES):
        raise ValueError('raw frame set differs from the fixture')
    counters = result.get('counters', {})
    if counters.get('backend') != backend:
        raise ValueError('wrong backend')
    if backend == 'productionGX2':
        if counters.get('native_draws') != 6 or counters.get('pass') is not True:
            raise ValueError('fixture did not draw six native batches')
        if counters.get('immediate_native_draws') != 1:
            raise ValueError('fixture did not issue its native immediate batch')
        for field in ('immediate_cpu_runs', 'cpu_batches', 'cpu_draws', 'cpu_vertices', 'cpu_route', 'pixel_misses', 'shader_miss_bytes'):
            if counters.get(field) != 0:
                raise ValueError('fallback or incomplete counters: ' + field)
        if counters.get('resolution') not in (240, 480):
            raise ValueError('unsupported replay resolution')
        if counters.get('diagnostic_1x') is not (counters['resolution'] == 240):
            raise ValueError('inconsistent diagnostic resolution')
    png_folder.mkdir(exist_ok=False)
    for frame in frames:
        name = frame['name']
        width, height, _ = CASES[name]
        if (frame.get('width'), frame.get('height'), frame.get('wrong_pixels')) != (width, height, 0):
            raise ValueError('invalid frame metadata: ' + name)
        raw = (folder / (name + '.rgba')).read_bytes()
        if len(raw) != width * height * 4:
            raise ValueError('short/oversized raw frame: ' + name)
        pngdiff.save(png_folder / (name + '.png'), width, height,
                     [raw[row * width * 4:(row + 1) * width * 4] for row in range(height)])
    compared = compare.compare_dirs(reference, png_folder, tolerance=0)
    return {'match': compared['match'], 'backend': backend, 'counters': counters, 'comparison': compared,
            'raw_sha256': {name: sha(folder / (name + '.rgba')) for name in CASES},
            'result_sha256': sha(folder / 'result.json'), 'tolerance': 0,
            'reference_kind': 'Independent analytical fills, centered rectangles, and four texture quadrants.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--backend', required=True, choices=('software', 'productionGX2'))
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--png-output', type=Path, required=True)
    args = parser.parse_args()
    if not args.reference.exists():
        make_analytical(args.reference)
    proof = verify(args.output, args.backend, args.reference, args.png_output)
    (args.png_output / 'proof.json').write_text(json.dumps(proof, indent=2) + '\n')
    print(json.dumps({'match': proof['match'], 'backend': args.backend, 'frames': len(proof['raw_sha256'])}))
    raise SystemExit(0 if proof['match'] else 1)
