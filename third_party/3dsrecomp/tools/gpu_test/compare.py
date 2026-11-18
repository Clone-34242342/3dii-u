#!/usr/bin/env python3
"""Run isolated GPU replays and compare decoded pixels, with a hashed JSON report.

References must be reviewed separately. Regression references prove stability,
not correctness; use analytical or independent references for correctness claims.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

import pngdiff


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def compare_dirs(reference, actual, tolerance=0, diff_dir=None):
    """Compare the exact set of PNGs; missing, extra or invalid images fail."""
    reference, actual = Path(reference), Path(actual)
    if not reference.is_dir() or not actual.is_dir():
        raise ValueError('reference and actual must be directories')
    if type(tolerance) is not int or not 0 <= tolerance <= 255:
        raise ValueError('tolerance must be 0..255')
    refs = {p.name: p for p in reference.iterdir() if p.suffix.lower() == '.png'}
    outputs = {p.name: p for p in actual.iterdir() if p.suffix.lower() == '.png'}
    if not refs:
        raise ValueError('reference contains no PNG frames')
    result = dict(match=False, missing=sorted(refs.keys() - outputs.keys()),
                  extra=sorted(outputs.keys() - refs.keys()), frames={})
    if diff_dir is not None:
        diff_dir = Path(diff_dir)
        diff_dir.mkdir(parents=True, exist_ok=False)
    for name in sorted(refs.keys() & outputs.keys()):
        frame = dict(reference_sha256=sha256(refs[name]), actual_sha256=sha256(outputs[name]))
        try:
            frame.update(pngdiff.compare(pngdiff.load(refs[name]), pngdiff.load(outputs[name]),
                                        tolerance, diff_dir / name if diff_dir else None))
        except (OSError, pngdiff.PNGError) as error:
            frame.update(match=False, error=str(error))
        result['frames'][name] = frame
    result['match'] = (not result['missing'] and not result['extra'] and
                       all(f['match'] for f in result['frames'].values()))
    return result


def snapshot(paths):
    return {str(p.resolve()): sha256(p) for p in paths}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('output', type=Path, help='new directory; existing output is refused')
    parser.add_argument('dumps', type=Path, nargs='+')
    parser.add_argument('--replay', type=Path, default=Path(__file__).parent / 'build/gpu_replay')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--tolerance', type=int, default=0, help='maximum RGBA channel error, default exact')
    parser.add_argument('--oracle', choices=('regression', 'analytical', 'independent'), default='regression')
    parser.add_argument('--provenance', default='Unverified same-renderer regression references')
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not 0 <= args.tolerance <= 255:
        parser.error('timeout must be finite and positive; tolerance must be 0..255')
    names = [p.name for p in args.dumps]
    if len({name.casefold() for name in names}) != len(names):
        parser.error('dump basenames must be unique')
    report = dict(match=False, oracle=args.oracle, provenance=args.provenance, runs=[])
    output_created = False
    try:
        reference = args.reference.resolve(strict=True)
        replay = args.replay.resolve(strict=True)
        dumps = [p.resolve(strict=True) for p in args.dumps]
        if len(set(dumps)) != len(dumps):
            raise ValueError('resolved dump directories must be unique')
        references = [p for p in reference.iterdir() if p.suffix.lower() == '.png']
        if not references:
            raise ValueError('reference contains no PNG frames')
        inputs = [replay, Path(__file__), Path(pngdiff.__file__)] + references
        state_presence = {str(dump): (dump / 'gpustate.bin').exists() for dump in dumps}
        for dump in dumps:
            inputs += [dump / name for name in ('fcram.bin', 'vram.bin', 'ops.txt')]
            if state_presence[str(dump)]:
                inputs.append(dump / 'gpustate.bin')
        before = snapshot(inputs)
        report['input_sha256'] = before
        args.output.mkdir(parents=True, exist_ok=False)
        output_created = True
        frames = args.output / 'frames'
        frames.mkdir()
        for dump in dumps:
            run_dir = args.output / 'runs' / dump.name
            run_dir.mkdir(parents=True)
            primary = run_dir / (dump.name + '.png')
            command = [str(replay), str(dump), str(primary.resolve()), '-t']
            run = dict(input=str(dump), command=command, returncode=None)
            started = time.monotonic()
            with (args.output / (dump.name + '.log')).open('wb') as log:
                try:
                    run['returncode'] = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                                       timeout=args.timeout, check=False).returncode
                except subprocess.TimeoutExpired:
                    run['error'] = 'replay timed out'
                except OSError as error:
                    run['error'] = str(error)
            run['outputs'] = sorted(p.name for p in run_dir.iterdir() if p.suffix.lower() == '.png')
            if not primary.is_file():
                run.setdefault('error', 'replay produced no primary frame')
            for name in run['outputs']:
                if (frames / name).exists():
                    run['error'] = 'replay frame name collision: ' + name
                else:
                    shutil.copyfile(run_dir / name, frames / name)
            run['seconds'] = round(time.monotonic() - started, 3)
            report['runs'].append(run)
        report['comparison'] = compare_dirs(reference, frames, args.tolerance, args.output / 'diff')
        report['inputs_unchanged'] = (snapshot(inputs) == before and
            {p for p in reference.iterdir() if p.suffix.lower() == '.png'} == set(references) and
            {str(dump): (dump / 'gpustate.bin').exists() for dump in dumps} == state_presence)
        report['match'] = (report['inputs_unchanged'] and report['comparison']['match'] and
                           all(r['returncode'] == 0 and 'error' not in r for r in report['runs']))
    except (OSError, ValueError) as error:
        report['error'] = str(error)
        # Never write into an existing output directory after refusing it.
        if output_created:
            try:
                (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            except OSError:
                pass  # The JSON printed below still reports an unwritable output.
        print(json.dumps(report, sort_keys=True))
        return 2
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(match=report['match'], report=str(args.output / 'report.json'))))
    return 0 if report['match'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
