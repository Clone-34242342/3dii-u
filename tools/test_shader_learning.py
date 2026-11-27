"""Checker learning uses the actual converter: combined programs plus shareable fragment-only states, never from cold checks."""
from pathlib import Path
import struct
import tempfile
from unittest import mock

import check_ports as check


def main():
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder).resolve()
        target = check.Target('synthetic', '0', str(root / 'out'), (root / 'game').as_posix())
        logs = Path(target.data) / 'shader_logs'
        logs.mkdir(parents=True)
        previous = logs / 'previous_fs.bin'
        previous.write_bytes(b'previous learned states')
        fs_a, fs_b = struct.pack('<3I', 1, 2, 3), struct.pack('<3I', 4, 5, 6)
        fs_record = lambda body: struct.pack('<I', len(body)) + body
        vertex = b'VS  ' + struct.pack('<III', 9, 2, 1) + struct.pack('<3I', 7, 8, 9) + bytes(range(56))
        source = root / 'shader_misses.bin'
        source.write_bytes(b'FS  ' + fs_record(fs_a) + vertex + b'FS  ' + fs_record(fs_b) + b'FS  ' + fs_record(fs_a))
        unchanged = {p: (p.read_bytes(), p.stat().st_mtime_ns, p.stat().st_nlink) for p in (source, previous)}
        with mock.patch.object(check.time, 'strftime', return_value='synthetic'), mock.patch.object(check, 'run', wraps=check.run) as runner:
            check.learn_shaders(target, str(source))
            assert runner.call_count == 1
        combined, fragments = logs / 'cemu_synthetic.bin', logs / 'cemu_synthetic_fs.bin'
        assert combined.read_bytes() == fs_record(fs_a) + vertex + fs_record(fs_b), 'combined log lost or changed programs'
        assert fragments.is_file(), 'checker did not publish the *_fs.bin companion used by cross-title sharing'
        assert fragments.read_bytes() == fs_record(fs_a) + fs_record(fs_b), 'shared log must contain fragment states only'
        for p, state in unchanged.items():
            assert (p.read_bytes(), p.stat().st_mtime_ns, p.stat().st_nlink) == state, f'learning altered {p.name}'

        before = {p.relative_to(root): (p.read_bytes(), p.stat().st_mtime_ns) for p in root.rglob('*') if p.is_file()}
        cold = check.Target('synthetic_cold', '0', str(root / 'cold_out'), (root / 'cold_game').as_posix(), like='synthetic')
        with mock.patch.object(check, 'run') as runner:
            check.learn_shaders(cold, str(source))
            runner.assert_not_called()
        assert not Path(cold.data).exists(), 'cold learning created a data directory'
        assert before == {p.relative_to(root): (p.read_bytes(), p.stat().st_mtime_ns) for p in root.rglob('*') if p.is_file()}, 'cold check changed learned files'
    print('shader learning: combined and fragment-only outputs; cold isolation and originals preserved')


if __name__ == '__main__':
    main()
