"""Synthetic tickets and real junctions/hard links: object-cache operations must leave aliases alone."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent / 'win'))
import objcache

KEY = 'ab' + '1' * 62
OTHER = 'cd' + '2' * 62


class ObjectCacheSafety(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='3dii-object-safety-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cache = self.root / 'cache'
        self.build = self.root / 'build'
        self.build.mkdir()
        self.source = self.build / 'compiled.o'
        self.source.write_bytes(b'synthetic compiled object')
        self.output = self.build / 'fetched.o'
        self.ticket = self.root / 'NaNd' / 'rights' / 'ticket' / 'fake.tik'
        self.ticket.parent.mkdir(parents=True)
        self.ticket.write_bytes(b'synthetic ticket: never overwrite or touch its metadata')
        os.utime(self.ticket, ns=(1600000000000000000, 1600000000000000000))

    def snapshot(self, path):
        info = path.stat()
        return path.read_bytes(), info.st_mtime_ns, info.st_nlink

    def entry(self, key=KEY, cache=None):
        return (cache or self.cache) / key[:2] / (key + '.o')

    def junction(self, alias, target):
        if sys.platform == 'win32':
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(alias), str(target)], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.addCleanup(alias.rmdir)
        else:
            alias.symlink_to(target, target_is_directory=True)
            self.addCleanup(alias.unlink)

    def test_normal_fetch_store_and_lru_eviction(self):
        self.assertFalse(objcache.fetch(self.cache, KEY, self.output))
        objcache.store(self.cache, KEY, self.source)
        first = self.entry()
        self.assertEqual(first.read_bytes(), self.source.read_bytes())
        self.source.write_bytes(b'new object')
        objcache.store(self.cache, OTHER, self.source)
        second = self.entry(OTHER)
        os.utime(first, ns=(1000000000, 1000000000))
        os.utime(second, ns=(2000000000, 2000000000))
        self.output.write_bytes(b'old destination')
        self.assertTrue(objcache.fetch(self.cache, KEY, self.output))
        self.assertEqual(self.output.read_bytes(), b'synthetic compiled object')
        self.assertGreater(first.stat().st_mtime_ns, second.stat().st_mtime_ns)
        objcache.trim(self.cache, first.stat().st_size)
        self.assertTrue(first.exists())
        self.assertFalse(second.exists())
        self.assertFalse(list(self.root.rglob('*.part')))

    def test_bad_keys_and_parent_traversal_do_not_create_or_overwrite(self):
        before = self.snapshot(self.ticket)
        for key in ('', None, 'a' * 63, 'A' * 64, '../fake.tik', KEY + '/../../fake.tik', 'z' * 64):
            with self.subTest(key=key):
                self.assertFalse(objcache.fetch(self.cache, key, self.output))
                objcache.store(self.cache, key, self.source)
                self.assertFalse(self.cache.exists())
        objcache.store(self.root / '..' / 'unexpected-cache', KEY, self.source)
        self.assertFalse(objcache.fetch(self.root / '..' / 'unexpected-cache', KEY, self.output))
        with self.assertRaises(ValueError):
            objcache.fetch(self.cache, KEY, self.build / '..' / 'fetched.o')
        self.assertEqual(self.snapshot(self.ticket), before)

    def test_protected_paths_are_rejected_before_cache_miss_fallback(self):
        before = self.snapshot(self.ticket)
        for dirname in ('NaNd', 'TiCkEtS', 'MLC01', 'storage_slc', 'ticket.'):
            cache = self.root / dirname / 'objcache'
            objcache.store(cache, KEY, self.source)
            objcache.trim(cache, 0)
            self.assertFalse(objcache.fetch(cache, KEY, self.output))
            with self.assertRaises(ValueError):
                objcache.fetch(self.cache, KEY, self.root / dirname / 'compiled.o')
        for output in (self.ticket, self.build / 'x.o:stream'):
            with self.assertRaises(ValueError):
                objcache.fetch(self.cache, KEY, output)
        self.assertEqual(self.snapshot(self.ticket), before)
        self.assertFalse(self.cache.exists())

    def test_destination_hard_link_preserves_ticket_and_link_count(self):
        objcache.store(self.cache, KEY, self.source)
        os.link(self.ticket, self.output)
        before = self.snapshot(self.ticket)
        for key in (KEY, OTHER, 'bad key'):
            with self.assertRaises(ValueError):
                objcache.fetch(self.cache, key, self.output)
            self.assertEqual(self.snapshot(self.ticket), before)
            self.assertEqual(self.snapshot(self.output), before)

    def test_dependency_sidecar_is_checked_before_invoking_preprocessor(self):
        dependency = self.build / 'compiled.o.d'
        os.link(self.ticket, dependency)
        before = self.snapshot(self.ticket)
        with mock.patch.object(objcache.subprocess, 'run', side_effect=AssertionError('must reject before preprocessing')):
            for path in (dependency, self.ticket):
                with self.assertRaises(ValueError):
                    objcache.key(['compiler'], 'synthetic.c', path)
        self.assertEqual(self.snapshot(self.ticket), before)

    def test_cached_hard_link_is_not_fetched_touched_replaced_or_trimmed(self):
        entry = self.entry()
        entry.parent.mkdir(parents=True)
        os.link(self.ticket, entry)
        before = self.snapshot(self.ticket)
        self.assertFalse(objcache.fetch(self.cache, KEY, self.output))
        objcache.store(self.cache, KEY, self.source)
        objcache.trim(self.cache, 0)
        self.assertEqual(self.snapshot(self.ticket), before)
        self.assertEqual(self.snapshot(entry), before)
        self.assertFalse(self.output.exists())
        os.link(self.ticket, self.build / 'source-alias.o')
        before = self.snapshot(self.ticket)
        objcache.store(self.cache, OTHER, self.build / 'source-alias.o')
        self.assertEqual(self.snapshot(self.ticket), before)
        self.assertFalse(self.entry(OTHER).exists())

    def test_cache_root_and_ancestor_junctions_are_not_followed(self):
        outside = self.root / 'ordinary-external'
        external = self.entry(cache=outside)
        external.parent.mkdir(parents=True)
        external.write_bytes(b'synthetic external object')
        os.utime(external, ns=(1600000000000000000, 1600000000000000000))
        before = self.snapshot(external)
        alias = self.root / 'alias-cache'
        self.junction(alias, outside)
        for cache in (alias, alias / 'nested'):
            self.assertFalse(objcache.fetch(cache, KEY, self.output))
            objcache.store(cache, KEY, self.source)
            objcache.trim(cache, 0)
            self.assertEqual(self.snapshot(external), before)
        self.assertFalse((outside / 'nested').exists())

    def test_bucket_junction_does_not_touch_external_ticket(self):
        self.cache.mkdir()
        outside = self.root / 'ordinary-external'
        outside.mkdir()
        external = outside / (KEY + '.o')
        os.link(self.ticket, external)
        self.junction(self.cache / KEY[:2], outside)
        before = self.snapshot(self.ticket)
        self.assertFalse(objcache.fetch(self.cache, KEY, self.output))
        objcache.store(self.cache, KEY, self.source)
        objcache.trim(self.cache, 0)
        self.assertEqual(self.snapshot(self.ticket), before)
        self.assertEqual(self.snapshot(external), before)

    def test_destination_parent_junction_raises_even_without_cached_entry(self):
        outside = self.root / 'ordinary-output'
        outside.mkdir()
        external = outside / 'fetched.o'
        os.link(self.ticket, external)
        alias = self.root / 'alias-output'
        self.junction(alias, outside)
        before = self.snapshot(self.ticket)
        for key in (KEY, OTHER):
            with self.assertRaises(ValueError):
                objcache.fetch(self.cache, key, alias / 'fetched.o')
        objcache.store(self.cache, KEY, alias / 'fetched.o')
        self.assertEqual(self.snapshot(self.ticket), before)
        self.assertFalse(self.cache.exists())

    def test_trim_only_owns_two_hex_buckets_and_matching_64_hex_objects(self):
        objcache.store(self.cache, KEY, self.source)
        unrelated = [self.cache / 'unrelated.o', self.cache / 'other' / (OTHER + '.o'),
                     self.cache / 'ab' / ('cd' + '3' * 62 + '.o'), self.cache / 'ab' / 'notes.o',
                     self.cache / 'ab' / 'deep' / (KEY + '.o'), self.cache / 'ab' / (KEY + '.o.part')]
        for path in unrelated:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'not a cache-owned object')
        before = {path: self.snapshot(path) for path in unrelated}
        objcache.trim(self.cache, 0)
        self.assertFalse(self.entry().exists())
        for path in unrelated:
            self.assertEqual(self.snapshot(path), before[path])

    def test_copy_error_preserves_existing_destination_and_cleans_owned_temporary(self):
        objcache.store(self.cache, KEY, self.source)
        self.output.write_bytes(b'previous safe object')
        before = self.snapshot(self.output)
        with mock.patch.object(objcache.shutil, 'copyfileobj', side_effect=OSError('synthetic full disk')):
            self.assertFalse(objcache.fetch(self.cache, KEY, self.output))
            objcache.store(self.cache, OTHER, self.source)
        self.assertEqual(self.snapshot(self.output), before)
        self.assertFalse(self.entry(OTHER).exists())
        self.assertFalse(list(self.root.rglob('*.part')))


if __name__ == '__main__':
    unittest.main()
