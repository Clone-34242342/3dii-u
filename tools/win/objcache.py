"""Compiled objects kept by what went into them, as ccache does: the compile line without its paths, the compiler, and the source after the
preprocessor (every header it read). A game built again, by a new builder app or with an update, then compiles only the files whose code changed:
the app deletes a game's work files after each build, and a new app's engine starts its work folder over (app.fresh_cache), so without this every
build compiled all of a game's code again (Zelda: 110 files, 90 s of its 254 s).

The cache lives in <work>/../objcache, outside what fresh_cache wipes, and is trimmed to MAX_BYTES, least recently used first. Trouble with the
cache (a file another build trimmed or holds open, a full disk) never fails a build: the object is then just compiled, or not kept.
An unsafe output path is rejected before a cache miss can send the compiler there."""
from pathlib import Path
from contextlib import ExitStack, contextmanager
import hashlib
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from wup_safety import check_output, NAND_DIRS

MAX_BYTES = 2 << 30  # disk space is tight on the PCs this runs on


def workspace_lock(*folders):
    """Hold the shared build folders until the process finishes or exits."""
    return file_locks(*(Path(folder) / '.3diiu-build.lock' for folder in folders))


@contextmanager
def file_locks(*lock_paths):
    """Acquire the named files in canonical order, releasing all if any is busy."""
    paths = {}
    for entry in lock_paths:
        path, _ = _checked(entry)
        paths[os.path.normcase(path)] = path
    with ExitStack() as files:
        for lock in (paths[k] for k in sorted(paths)):
            lock.parent.mkdir(parents=True, exist_ok=True)
            _checked(lock)
            fd = os.open(lock, os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0), 0o600)
            files.callback(os.close, fd)
            _, present = _checked(lock)
            if present is None or not os.path.samestat(present, os.fstat(fd)):
                raise OSError(f'The Builder lock changed while opening {lock}.')
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as e:
                raise OSError(f'Another build is using {lock.parent}. Wait for it to finish and try again.') from e
        yield


def _compiler_id(exe):
    st = os.stat(exe)
    return f'{os.path.basename(exe)} {st.st_size} {int(st.st_mtime)}'


def key(argv, src, deps):
    """the cache key for compiling src with argv (the compiler first, no -c/-o), or None if the preprocessor fails (the compile reports it).
    The same pass writes the dependency file the compile would (deps), so a fetched object needs no compile at all. Line markers stay in when the
    flags ask for debug information, whose line tables come from them."""
    _checked(deps)  # the preprocessor writes this sidecar before a cache lookup
    flags = [a for a in argv[1:] if not a.startswith(('-I', '-isystem', '-MF', '-MMD')) and not os.path.isabs(a)]
    marks = [] if '-g' in argv else ['-P']
    pp = subprocess.run([argv[0]] + [a for a in argv[1:] if a != '-MMD'] + ['-E'] + marks + ['-MMD', '-MF', deps, src], stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL)
    if pp.returncode:
        return None
    h = hashlib.sha256()
    h.update(_compiler_id(argv[0]).encode())
    h.update('\0'.join(flags).encode())
    h.update(pp.stdout)
    return h.hexdigest()


def _path(cache, k):
    if not isinstance(k, str) or not re.fullmatch(r'[0-9a-f]{64}', k):
        raise ValueError('Invalid compiled-object cache key.')
    root, _ = _checked(cache, directory=True)
    bucket, _ = _checked(root / k[:2], directory=True)
    return bucket / (k + '.o')


def _checked(path, directory=False):
    """Check existing ancestors too: resolve() alone hides a junction or symlink on the way in."""
    raw = Path(path)
    if '..' in raw.parts or (raw.drive and not raw.is_absolute()):
        raise ValueError('Refusing a parent traversal in an object-cache path.')
    path = raw.absolute()
    if any(part.rstrip(' .').casefold() in NAND_DIRS for part in path.parts):
        raise ValueError('Object-cache paths must stay outside NAND and ticket storage.')
    if any(':' in part for part in path.parts[1:]):
        raise ValueError('Object-cache paths must not name alternate data streams.')
    found = None
    for entry in reversed((path, *path.parents)):
        try:
            info = entry.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Object-cache paths must not traverse reparse points or symbolic links.')
        is_directory = entry != path or directory
        if is_directory and not stat.S_ISDIR(info.st_mode):
            raise ValueError('Object-cache parents must be ordinary folders.')
        if not is_directory and (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1):
            raise ValueError('Object-cache files must be ordinary files without shared hard links.')
        if entry == path:
            found = info
    check_output(path)
    return path, found


def _object(path):
    path, info = _checked(path)
    if path.suffix != '.o' or ':' in path.name:
        raise ValueError('An object-cache output must be an ordinary .o file.')
    return path, info


def _copy(source, destination):
    """An exclusive temporary avoids truncating an existing output or following a shared file alias."""
    source, before = _object(source)
    destination, _ = _object(destination)
    if before is None:
        raise FileNotFoundError(source)
    temporary = None
    created = None
    try:
        with source.open('rb') as reader:
            opened = os.fstat(reader.fileno())
            if not os.path.samestat(before, opened) or opened.st_nlink != 1:
                raise ValueError('The cached object changed while opening it.')
            fd, name = tempfile.mkstemp(prefix=destination.name + '.', suffix='.part', dir=destination.parent)
            temporary = Path(name)
            with os.fdopen(fd, 'wb') as writer:
                created = os.fstat(writer.fileno())
                shutil.copyfileobj(reader, writer)
        _object(destination)
        _, current = _checked(temporary)
        if current is None or not os.path.samestat(created, current):
            raise ValueError('The temporary object changed before publication.')
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            try:
                _, remaining = _checked(temporary)
                if created is not None and remaining is not None and os.path.samestat(created, remaining):
                    temporary.unlink()
            except (OSError, ValueError):
                pass


def fetch(cache, k, obj):
    """the cached object for key k at obj; False if there's none, or it went while being read"""
    obj, _ = _object(obj)  # unsafe outputs must not fall through to an overwriting compiler
    try:
        p = _path(cache, k)
        _copy(p, obj)
    except (OSError, ValueError):
        _object(obj)
        return False
    try:
        _object(p)
        os.utime(p)  # recently used, after checking reparse paths and shared-file aliases again
    except (OSError, ValueError):
        pass
    return True


def store(cache, k, obj):
    try:
        _object(obj)
        p = _path(cache, k)
        _object(p)
        p.parent.mkdir(parents=True, exist_ok=True)
        _copy(obj, p)
    except (OSError, ValueError):
        pass


def trim(cache, max_bytes=MAX_BYTES):
    """drops the least recently used objects until the cache is under max_bytes"""
    try:
        root, existing = _checked(cache, directory=True)
        if existing is None:
            return
        buckets = list(root.iterdir())
    except (OSError, ValueError):
        return
    files = []
    for bucket in buckets:
        if not re.fullmatch(r'[0-9a-f]{2}', bucket.name):
            continue
        try:
            _checked(bucket, directory=True)
            names = list(bucket.iterdir())
        except (OSError, ValueError):
            continue
        for entry in names:
            if not re.fullmatch(bucket.name + r'[0-9a-f]{62}\.o', entry.name):
                continue
            try:
                p = _path(root, entry.stem)
                _, info = _object(p)
                if info is not None:
                    files.append((info.st_mtime_ns, info.st_size, p, info))
            except (OSError, ValueError):
                continue
    total = sum(size for _, size, _, _ in files)
    for _, size, p, before in sorted(files):
        if total <= max_bytes:
            break
        try:
            _, current = _object(p)
            if current is not None and os.path.samestat(before, current):
                p.unlink()
                total -= size
        except (OSError, ValueError):
            pass
