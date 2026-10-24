"""Save-backup tests use owned fixtures only; no Ubisoft files are changed."""
from pathlib import Path
import ctypes as c
import json
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import save_protection as sp


@unittest.skipUnless(sys.platform == 'win32', 'Windows locking and reparse semantics')
class SaveProtectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mw-save-test-')
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        (self.source / '1.save').write_bytes(b'original-profile')
        (self.source / '2.save').write_bytes(b'checkpoint\0binary\xff')
        (self.source / '3.save').write_bytes(b'')
        self.store = self.root / 'backups'

    def tearDown(self):
        self.temporary.cleanup()

    def snapshot(self):
        return sp.snapshot_save_group(self.source, self.store)

    def test_complete_snapshot_and_no_original_writes(self):
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.source.iterdir()}
        snapshot = self.snapshot()
        self.assertEqual(sp.validate_snapshot(snapshot)['schema'], 1)
        report = sp.verify_originals(snapshot)
        self.assertTrue(report['unchanged'])
        self.assertEqual(report['files'], 3)
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.source.iterdir()})
        self.assertFalse(any(p.name.startswith('.pending') for p in self.store.iterdir()))

    def test_changed_added_removed_are_reported(self):
        snapshot = self.snapshot()
        (self.source / '1.save').write_bytes(b'changed externally')
        (self.source / '3.save').unlink()
        (self.source / '4.save').write_bytes(b'new')
        result = sp.verify_originals(snapshot)
        self.assertFalse(result['unchanged'])
        self.assertEqual(result['changed'], ['1.save'])
        self.assertEqual(result['removed'], ['3.save'])
        self.assertEqual(result['added'], ['4.save'])

    def test_plan_refuses_external_change_and_never_restores(self):
        snapshot = self.snapshot()
        expected = sp.verify_originals(snapshot)['current_hashes']
        plan = sp.plan_restore(snapshot, expected)
        self.assertTrue(plan['ready'])
        self.assertTrue(plan['already_original'])
        self.assertFalse(plan['writes_performed'])
        (self.source / '1.save').write_bytes(b'external-change')
        with self.assertRaises(sp.SaveProtectionError):
            sp.plan_restore(snapshot, expected)
        current = sp.verify_originals(snapshot)['current_hashes']
        plan = sp.plan_restore(snapshot, current)
        self.assertFalse(plan['writes_performed'])
        self.assertEqual((self.source / '1.save').read_bytes(), b'external-change')

    def test_corrupt_backup_cannot_be_verified(self):
        snapshot = self.snapshot()
        manifest = sp.validate_snapshot(snapshot)
        digest = manifest['files']['1.save']['sha256']
        (snapshot / 'blobs' / digest).write_bytes(b'bad')
        with self.assertRaises(sp.SaveProtectionError):
            sp.verify_originals(snapshot)

    def test_unsafe_manifest_names_rejected(self):
        snapshot = self.snapshot()
        path = snapshot / 'manifest.json'
        original = json.loads(path.read_text())
        for name in ['../outside.save', 'A/B.save', 'A\\B.save', 'C:stream', '..']:
            with self.subTest(name=name):
                changed = dict(original)
                changed['files'] = {name: original['files']['1.save']}
                path.write_text(json.dumps(changed))
                with self.assertRaises(sp.SaveProtectionError):
                    sp.validate_snapshot(snapshot)

    def test_bad_schema_and_entry_rejected(self):
        snapshot = self.snapshot()
        path = snapshot / 'manifest.json'
        for value in [[], {'schema': 2}, {'schema': 1, 'created_utc': 'x', 'source': str(self.source), 'files': {'a': None}}]:
            with self.subTest(value=value):
                path.write_text(json.dumps(value))
                with self.assertRaises(sp.SaveProtectionError):
                    sp.validate_snapshot(snapshot)

    def test_subdirectory_and_empty_groups_refused(self):
        (self.source / 'nested').mkdir()
        with self.assertRaises(sp.SaveProtectionError):
            self.snapshot()
        (self.source / 'nested').rmdir()
        for p in self.source.iterdir():
            p.unlink()
        with self.assertRaises(sp.SaveProtectionError):
            self.snapshot()

    def test_backup_inside_source_refused(self):
        with self.assertRaises(sp.SaveProtectionError):
            sp.snapshot_save_group(self.source, self.source / 'backups')

    def test_source_read_lock_blocks_writer(self):
        source = self.source / '1.save'
        locked = sp.LockedSave(source)
        try:
            kernel = sp._kernel()
            writer = kernel.CreateFileW(sp._win(source), 0x40000000, 3, None, 3, 0, None)
            if writer != sp.INVALID:
                kernel.CloseHandle(writer)
            self.assertEqual(writer, sp.INVALID)
            self.assertEqual(locked.read(), b'original-profile')
        finally:
            locked.close()

    def test_busy_writer_prevents_partial_snapshot(self):
        source = self.source / '2.save'
        kernel = sp._kernel()
        writer = kernel.CreateFileW(sp._win(source), 0x40000000, 3, None, 3, 0, None)
        self.assertNotEqual(writer, sp.INVALID)
        try:
            with self.assertRaises(sp.SaveProtectionError):
                self.snapshot()
            self.assertFalse(any(self.store.iterdir()))
        finally:
            kernel.CloseHandle(writer)

    def test_count_bound_and_deduplication(self):
        (self.source / '2.save').write_bytes(b'original-profile')
        snapshot = self.snapshot()
        self.assertEqual(len(list((snapshot / 'blobs').iterdir())), 2)
        for index in range(sp.MAX_FILES):
            (self.source / f'added-{index}').write_bytes(b'x')
        with self.assertRaises(sp.SaveProtectionError):
            self.snapshot()

    def test_private_marker_requires_unchanged_product_group(self):
        account = self.root / 'savegames' / 'fixture-account'
        account.mkdir(parents=True)
        self.source.rename(account / '2')
        self.source = account / '2'
        snapshot = self.snapshot()
        state = self.root / 'mod-state'
        state.mkdir()
        marker = sp.prepare_practice_source(snapshot, state)
        text = marker.read_text(encoding='utf-16')
        self.assertIn('Schema=1', text)
        self.assertIn('Directory=' + str(self.source), text)
        self.assertIn('Files=3', text)
        self.assertEqual(len(list(state.iterdir())), 1)
        (self.source / '1.save').write_bytes(b'external-update')
        before = marker.read_bytes()
        with self.assertRaises(sp.SaveProtectionError):
            sp.prepare_practice_source(snapshot, state)
        self.assertEqual(marker.read_bytes(), before)

    def test_marker_refuses_other_product_group(self):
        snapshot = self.snapshot()
        with self.assertRaises(sp.SaveProtectionError):
            sp.prepare_practice_source(snapshot, self.root)


if __name__ == '__main__':
    unittest.main()
