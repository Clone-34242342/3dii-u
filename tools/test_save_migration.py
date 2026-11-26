"""Run the Wii U save migration fault cases on temporary Windows folders."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'tools/win/build3ds.py').is_file())
PORT = ROOT / 'port/wiiu'
CASES = (
    'locked-common', 'shorter', 'merge', 'account-existing', 'write', 'input-close', 'output-close', 'rename', 'rollback',
    'too-large', 'negative-size', 'probe-open', 'probe-no-account', 'probe-increment', 'probe-root-status', 'probe-entry',
    'probe-savedata-status', 'probe-account-status', 'probe-empty', 'copy-increment', 'flush-unavailable', 'flush-negative',
    'flush-confirmed', 'flush-rollback-stuck', 'packed', 'new-account',
)


def replace_calls(source, calls):
    for pattern, replacement in calls:
        source, count = re.subn(pattern, replacement, source)
        if not count:
            raise RuntimeError(f'Save migration fixture could not intercept {replacement}')
    return source


def harness(work):
    storage = (PORT / 'save_storage.cpp').read_text(encoding='utf-8')
    storage = replace_calls(storage, (
        (r'(?<![\w:])pwrite(?=\()', 'migration_pwrite'), (r'(?<![\w:])close(?=\()', 'migration_close'),
        (r'(?<![\w:])fstat(?=\()', 'migration_fstat'), (r'\bfs::rename(?=\()', 'migration_rename'),
        (r'(?:std::filesystem|fs)::directory_iterator\b', 'MigrationDirectory'),
    ))
    (work / 'save_storage_under_test.inc').write_text(storage, encoding='utf-8')

    # Keep all of save_folder's control flow. Replace only filesystem operations that the fixture can fail.
    net = (PORT / 'net_console.cpp').read_text(encoding='utf-8')
    start, end = 'std::string save_folder() {', '\n\nbool test_run() {'
    body = net[net.index(start):net.index(end)]
    if net.count(start) != 1 or net.count(end) != 1 or not body.endswith('\n}'):
        raise RuntimeError('Save migration fixture could not locate save_folder()')
    body = replace_calls(body, (
        (r'(?:std::filesystem|fs)::directory_iterator\b', 'MigrationDirectory'),
        (r'(?:std::filesystem|fs)::status\b', 'migration_status'),
        (r'(?:std::filesystem|fs)::exists\b', 'migration_exists'),
        (r'(?:std::filesystem|fs)::is_empty\b', 'migration_empty'),
    ))

    packed_test = (ROOT / 'tools/test_save_from_content.cpp').read_text(encoding='utf-8')
    include = '#include "../port/wiiu/save_storage.cpp"'
    if packed_test.count(include) != 1:
        raise RuntimeError('Save migration fixture could not locate the packed-save harness include')
    packed_test = packed_test.replace(include, '#include "test_save_migration.h"\n#include "save_storage_under_test.inc"')
    source = ('#define SAVE_MIGRATION_FIXTURE\n#define main packed_save_checks\n' + packed_test + '\n#undef main\n'
              'const std::string &wiiu::data_dir() { return common_data; }\n'
              'namespace wiiu {\n' + body + '\n}\n#define SAVE_MIGRATION_CASES\n#include "test_save_migration.h"\n')
    output = work / 'test_save_migration.cpp'
    output.write_text(source, encoding='utf-8')
    return output


def main():
    sys.path.insert(0, str(ROOT / 'tools/win'))
    import build3ds

    with tempfile.TemporaryDirectory(prefix='3dii-save-migration-') as directory:
        work = Path(directory)
        fake = work / 'fake_wut/coreinit'
        fake.mkdir(parents=True)
        for header in ('dynload.h', 'filesystem.h', 'time.h'):
            (fake / header).touch()
        source = harness(work)
        exe = work / 'test_save_migration.exe'
        subprocess.run([*build3ds.HOST_CXX, '-std=c++20', '-O1', '-w', '-D_FILE_OFFSET_BITS=64', '-static',
                        f'-I{Path(__file__).resolve().parent}', f'-I{fake.parent}', f'-I{PORT}', f'-I{build3ds.PORT}/pc/win',
                        f'-I{build3ds.PORT}/pc', f'-I{build3ds.RT}', '-include', f'{build3ds.PORT}/pc/shim.h', str(source),
                        f'{build3ds.PORT}/pc/win/win_compat.cpp', '-lpthread', '-lwinmm', '-o', str(exe)], check=True, timeout=90)
        for case in CASES:
            folder = work / case
            folder.mkdir()
            result = subprocess.run([str(exe), case, str(folder)], env=dict(os.environ, TEMP=str(folder), TMP=str(folder)), timeout=15)
            if result.returncode:
                raise SystemExit(f'Save migration case {case} failed with exit code {result.returncode}')
        print(f'Save migration passed all {len(CASES)} cases')


if __name__ == '__main__':
    main()
