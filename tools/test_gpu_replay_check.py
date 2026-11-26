"""Negative controls for replay orchestration; these do not validate rendered graphics."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch

GPU_TEST = Path(__file__).resolve().parents[1] / 'third_party/3dsrecomp/tools/gpu_test'
sys.path.insert(0, str(GPU_TEST))
import compare as gate
import pngdiff
import gpu_replay_verify as raw_gate


class ReplayGate(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ref, self.actual = self.root / 'reference', self.root / 'actual'
        self.ref.mkdir(); self.actual.mkdir()
        self.rows = [bytes((1, 2, 3, 255, 9, 8, 7, 255))]
        pngdiff.save(self.ref / 'scene.png', 2, 1, self.rows)
        pngdiff.save(self.actual / 'scene.png', 2, 1, self.rows)

    def test_pixels_dimensions_and_frame_set(self):
        self.assertTrue(gate.compare_dirs(self.ref, self.actual)['match'])
        pngdiff.save(self.actual / 'scene.png', 2, 1, [bytes((2, 2, 3, 255, 9, 8, 7, 255))])
        result = gate.compare_dirs(self.ref, self.actual)
        self.assertFalse(result['match'])
        self.assertEqual(result['frames']['scene.png']['bbox'], [0, 0, 0, 0])
        self.assertTrue(gate.compare_dirs(self.ref, self.actual, tolerance=1)['match'])
        pngdiff.save(self.actual / 'scene.png', 1, 1, [self.rows[0][:4]])
        self.assertFalse(gate.compare_dirs(self.ref, self.actual)['match'])
        (self.actual / 'scene.png').unlink()
        pngdiff.save(self.actual / 'unexpected.PNG', 2, 1, self.rows)
        result = gate.compare_dirs(self.ref, self.actual)
        self.assertEqual(result['missing'], ['scene.png'])
        self.assertEqual(result['extra'], ['unexpected.PNG'])
        self.assertFalse(result['match'])

    def test_empty_and_invalid(self):
        (self.actual / 'scene.png').write_bytes(b'broken')
        self.assertFalse(gate.compare_dirs(self.ref, self.actual)['match'])
        (self.ref / 'scene.png').unlink()
        with self.assertRaises(ValueError):
            gate.compare_dirs(self.ref, self.actual)

    def setup_run(self):
        dump = self.root / 'scene'
        dump.mkdir()
        for name in ('fcram.bin', 'vram.bin', 'ops.txt'):
            (dump / name).write_bytes(b'fixture')
        replay = self.root / 'replay.exe'
        replay.write_bytes(b'placeholder for orchestration test')
        return [str(self.ref), str(self.root / 'run'), str(dump), '--replay', str(replay)]

    def invoke(self, argv, effect):
        with patch.object(gate.subprocess, 'run', side_effect=effect), contextlib.redirect_stdout(io.StringIO()):
            return gate.main(argv)

    def render(self, command, **kwargs):
        pngdiff.save(command[2], 2, 1, self.rows)
        return subprocess.CompletedProcess(command, 0)

    def test_success_hashes_and_existing_output_refused(self):
        argv = self.setup_run()
        self.assertEqual(self.invoke(argv, self.render), 0)
        report = json.loads((self.root / 'run/report.json').read_text())
        self.assertTrue(report['inputs_unchanged'])
        self.assertEqual(report['oracle'], 'regression')
        self.assertIn(str((self.root / 'scene/ops.txt').resolve()), report['input_sha256'])
        original = (self.root / 'run/report.json').read_bytes()
        self.assertEqual(self.invoke(argv, self.render), 2)
        self.assertEqual(original, (self.root / 'run/report.json').read_bytes())

    def test_nonzero_process_cannot_pass_with_matching_pixels(self):
        def failure(command, **kwargs):
            self.render(command, **kwargs)
            return subprocess.CompletedProcess(command, 9)
        self.assertEqual(self.invoke(self.setup_run(), failure), 1)

    def test_timeout_and_missing_output_fail(self):
        def timeout(command, **kwargs):
            raise subprocess.TimeoutExpired(command, kwargs['timeout'])
        self.assertEqual(self.invoke(self.setup_run(), timeout), 1)
        report = json.loads((self.root / 'run/report.json').read_text())
        self.assertEqual(report['runs'][0]['error'], 'replay timed out')
        self.assertEqual(report['comparison']['missing'], ['scene.png'])

    def test_input_mutation_fails(self):
        def mutate(command, **kwargs):
            self.render(command, **kwargs)
            (self.root / 'scene/ops.txt').write_bytes(b'changed during replay')
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(self.invoke(self.setup_run(), mutate), 1)
        self.assertFalse(json.loads((self.root / 'run/report.json').read_text())['inputs_unchanged'])

    def test_optional_state_appearance_fails(self):
        def create_state(command, **kwargs):
            self.render(command, **kwargs)
            (self.root / 'scene/gpustate.bin').write_bytes(b'appeared')
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(self.invoke(self.setup_run(), create_state), 1)
        self.assertFalse(json.loads((self.root / 'run/report.json').read_text())['inputs_unchanged'])

    def test_input_disappearance_keeps_failure_report(self):
        def remove_input(command, **kwargs):
            self.render(command, **kwargs)
            (self.root / 'scene/ops.txt').unlink()
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(self.invoke(self.setup_run(), remove_input), 2)
        report = json.loads((self.root / 'run/report.json').read_text())
        self.assertFalse(report['match'])
        self.assertIn('error', report)
        self.assertTrue(report['input_sha256'])

    def test_optional_state_appearance_during_initial_hashing_fails(self):
        argv = self.setup_run()
        original = gate.sha256
        def create_state_during_hash(path):
            if Path(path).name == 'ops.txt':
                (self.root / 'scene/gpustate.bin').write_bytes(b'appeared during hashing')
            return original(path)
        with patch.object(gate, 'sha256', side_effect=create_state_during_hash):
            self.assertEqual(self.invoke(argv, self.render), 1)
        self.assertFalse(json.loads((self.root / 'run/report.json').read_text())['inputs_unchanged'])

    def test_secondary_frame_cannot_be_overwritten_by_next_dump(self):
        argv = self.setup_run()
        second = self.root / 'scene_1'
        second.mkdir()
        for name in ('fcram.bin', 'vram.bin', 'ops.txt'):
            (second / name).write_bytes(b'fixture')
        argv.insert(3, str(second))
        pngdiff.save(self.ref / 'scene_1.png', 2, 1, self.rows)
        def collision(command, **kwargs):
            self.render(command, **kwargs)
            if Path(command[1]).name == 'scene':
                pngdiff.save(Path(command[2]).parent / 'scene_1.png', 2, 1,
                             [bytes((99, 2, 3, 255, 9, 8, 7, 255))])
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(self.invoke(argv, collision), 1)
        report = json.loads((self.root / 'run/report.json').read_text())
        self.assertIn('collision', report['runs'][1]['error'])

    def test_target_builder_rejects_recursive_source_output(self):
        root = Path(__file__).resolve().parents[1]
        for folder in ('port', 'third_party/3dsrecomp/runtime', 'third_party/3dsrecomp/tools/gpu_test'):
            output = root / folder / ('replay-guard-' + uuid.uuid4().hex)
            result = subprocess.run([sys.executable, '-B', str(root / 'tools/test_gpu_render_target.py'),
                                     '--out', str(output)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('outside scanned source roots', result.stderr)
            self.assertFalse(output.exists())

    def test_raw_adapter_rejects_fallback_and_inconsistent_resolution(self):
        raw = self.root / 'raw'
        raw.mkdir()
        for name in raw_gate.CASES:
            (raw / (name + '.rgba')).write_bytes(b'')
        counters = {'backend': 'productionGX2', 'native_draws': 6, 'immediate_native_draws': 1, 'immediate_cpu_runs': 0, 'cpu_batches': 0, 'cpu_draws': 0,
                    'cpu_vertices': 0, 'cpu_route': 0, 'pixel_misses': 0, 'shader_miss_bytes': 0,
                    'resolution': 480, 'diagnostic_1x': False, 'pass': True}
        def rejected(message):
            (raw / 'result.json').write_text(json.dumps({'version': 1, 'pass': True,
                'frames': [dict(name=name) for name in raw_gate.CASES], 'counters': counters}))
            with self.assertRaisesRegex(ValueError, message):
                raw_gate.verify(raw, 'productionGX2', self.ref, self.root / 'png')
        counters['immediate_native_draws'] = 0
        rejected('native immediate batch')
        counters['immediate_native_draws'] = 1
        counters['diagnostic_1x'] = True
        rejected('inconsistent diagnostic resolution')
        counters['diagnostic_1x'] = False
        for field in ('immediate_cpu_runs', 'cpu_batches', 'cpu_draws', 'cpu_vertices', 'cpu_route', 'pixel_misses', 'shader_miss_bytes'):
            counters[field] = 1
            rejected('fallback or incomplete counters')
            counters[field] = 0

    def test_raw_adapter_rejects_reversed_texture_v(self):
        reference = self.root / 'analytical'
        raw_gate.make_analytical(reference)
        raw = self.root / 'raw'
        raw.mkdir()
        frames = []
        for name in raw_gate.CASES:
            width, height, rows = pngdiff.load(reference / (name + '.png'))
            if name == 'texture':
                # The LCD rotation maps PICA V to the displayed horizontal axis.
                rows = [row[:200] + row[800:1400] + row[200:800] + row[1400:] for row in rows]
            (raw / (name + '.rgba')).write_bytes(b''.join(rows))
            frames.append(dict(name=name, width=width, height=height, wrong_pixels=0))
        (raw / 'result.json').write_text(json.dumps({'version': 1, 'pass': True,
            'frames': frames, 'counters': {'backend': 'software'}}))
        proof = raw_gate.verify(raw, 'software', reference, self.root / 'png')
        self.assertFalse(proof['match'])
        self.assertEqual(proof['comparison']['frames']['texture.png']['differing_pixels'], 54000)


if __name__ == '__main__':
    unittest.main()
