"""Overlay deployment safety tests against disposable game and PE fixtures."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

import test_enhanced as fixtures

enhanced = fixtures.enhanced
SPEC = importlib.util.spec_from_file_location('overlay_install_under_test', fixtures.PACKAGE / 'overlay_install.py')
overlay = importlib.util.module_from_spec(SPEC)
with mock.patch.dict(sys.modules, {'enhanced': enhanced}):
    SPEC.loader.exec_module(overlay)


def fake_dll(payload=b'fixture'):
    data = bytearray(300)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\x00\x00'
    struct.pack_into('<HH', data, 132, 0x14c, 1)
    struct.pack_into('<HH', data, 148, 96, 0x2102)
    struct.pack_into('<H', data, 152, 0x10b)
    return bytes(data) + payload


class OverlayInstallTests(unittest.TestCase):
    write = fixtures.IsolatedGame.write
    read = fixtures.IsolatedGame.read
    snapshot = fixtures.IsolatedGame.snapshot

    def setUp(self):
        fixtures.IsolatedGame.setUp(self)
        self.manager.install({'tactical', 'deniable'}, mouse_input='standard')
        # Explicit disposable source, independent of both production PACKAGE globals.
        # A fake PE fixture must never be written to the real native build folder.
        fixture_package = (Path(self.temp.name) / 'overlay-build-fixture').resolve()
        self.assertTrue(fixture_package.is_relative_to(Path(self.temp.name).resolve()))
        self.assertNotEqual(fixture_package, fixtures.PACKAGE.resolve())
        patch = mock.patch.object(overlay, 'PACKAGE', fixture_package)
        patch.start()
        self.addCleanup(patch.stop)
        self.source = fixture_package / 'native/build/ConvictionEnhanced.asi'
        self.source.parent.mkdir(parents=True)
        self.data = fake_dll()
        self.source.write_bytes(self.data)
        self.sha = enhanced.digest(self.data)
        self.installer = overlay.OverlayInstaller(self.root)

    def test_install_and_uninstall_restore_absence_and_preserve_settings(self):
        before = self.snapshot()
        active = self.manager.state_path('active.json').read_bytes()
        self.installer.install(self.sha)
        self.assertEqual(self.read(overlay.TARGET), self.data)
        self.assertEqual(self.installer.status()['integrity'], 'ok')
        self.assertEqual(self.manager.state_path('active.json').read_bytes(), active)
        self.installer.uninstall()
        self.assertEqual(self.snapshot(), before)
        self.assertFalse(self.manager.state_path(overlay.RECORD).exists())
        self.assertFalse(self.manager.state_path(overlay.JOURNAL).exists())
        self.assertEqual(self.manager.state_path('active.json').read_bytes(), active)

    def test_existing_destination_and_upgrade_restore_original_exactly(self):
        original = b'preexisting overlay file\x00\xff'
        self.write(overlay.TARGET, original)
        self.installer.install(self.sha)
        first_record = self.installer.record()
        self.assertEqual(self.manager.blob(first_record['original']), original)
        replacement = fake_dll(b'next build')
        self.source.write_bytes(replacement)
        self.installer.install(enhanced.digest(replacement))
        self.assertEqual(self.installer.record()['original'], first_record['original'])
        self.assertEqual(self.read(overlay.TARGET), replacement)
        self.installer.uninstall()
        self.assertEqual(self.read(overlay.TARGET), original)

    def test_repeated_install_is_noop_without_transaction(self):
        self.installer.install(self.sha)
        before = self.snapshot()
        with mock.patch.object(self.installer, 'transaction') as transaction:
            self.assertIn('already installed', self.installer.install(self.sha.upper()))
            transaction.assert_not_called()
        self.assertEqual(self.snapshot(), before)

    def test_foreign_destination_is_never_removed_without_ownership(self):
        self.write(overlay.TARGET, b'foreign file')
        with self.assertRaisesRegex(enhanced.Conflict, 'No owned'):
            self.installer.uninstall()
        self.assertEqual(self.read(overlay.TARGET), b'foreign file')
        self.assertEqual(self.installer.status()['integrity'], 'unowned')

    def test_changed_owned_file_blocks_upgrade_and_uninstall(self):
        self.installer.install(self.sha)
        self.write(overlay.TARGET, b'external replacement')
        record = self.manager.state_path(overlay.RECORD).read_bytes()
        for action in (lambda: self.installer.install(self.sha), self.installer.uninstall):
            with self.assertRaisesRegex(enhanced.Conflict, 'changed since installation'):
                action()
        self.assertEqual(self.read(overlay.TARGET), b'external replacement')
        self.assertEqual(self.manager.state_path(overlay.RECORD).read_bytes(), record)

    def test_expected_hash_and_pe_architecture_are_verified(self):
        before = self.snapshot()
        for sha in (None, '', 'abcd', '0' * 64):
            with self.subTest(sha=sha), self.assertRaises(enhanced.Conflict):
                self.installer.install(sha)
        invalid = [b'not a binary', self.data[:180]]
        x64 = bytearray(self.data)
        struct.pack_into('<H', x64, 132, 0x8664)
        invalid.append(bytes(x64))
        exe = bytearray(self.data)
        struct.pack_into('<H', exe, 150, 0x102)
        invalid.append(bytes(exe))
        for data in invalid:
            with self.subTest(data=data[:20]):
                self.source.write_bytes(data)
                with self.assertRaisesRegex(enhanced.Conflict, '32-bit PE DLL'):
                    self.installer.install(enhanced.digest(data))
        self.assertEqual(self.snapshot(), before)
        self.assertIsNone(self.installer.record())

    def test_game_and_fusion_fingerprint_mismatch_prevents_install(self):
        before = self.snapshot()
        with mock.patch.object(enhanced.Manager, 'inspect', side_effect=enhanced.Conflict('wrong build')):
            with self.assertRaisesRegex(enhanced.Conflict, 'wrong build'):
                self.installer.install(self.sha)
        self.assertEqual(self.snapshot(), before)

    def test_uninstall_remains_available_after_fusion_update(self):
        self.installer.install(self.sha)
        with mock.patch.object(enhanced.Manager, 'inspect', side_effect=enhanced.Conflict('changed Fusion')):
            self.installer.uninstall()
        self.assertIsNone(self.read(overlay.TARGET))

    def test_running_game_refuses_install_and_uninstall(self):
        self.installer.install(self.sha)
        before = self.snapshot()
        with mock.patch.object(enhanced, 'game_running', return_value=True):
            for action in (lambda: self.installer.install(self.sha), self.installer.uninstall):
                with self.assertRaisesRegex(enhanced.Conflict, 'Close Conviction'):
                    action()
        self.assertEqual(self.snapshot(), before)

    def test_existing_manager_lock_is_not_removed(self):
        lock = self.manager.state_path('operation.lock')
        lock.write_bytes(b'another operation')
        try:
            with self.assertRaises(enhanced.Conflict):
                self.installer.install(self.sha)
            self.assertEqual(lock.read_bytes(), b'another operation')
        finally:
            lock.unlink()

    def test_settings_recovery_is_not_modified(self):
        pending = self.manager.state_path('pending.json')
        pending.write_bytes(b'original pending settings')
        before = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, 'settings transaction'):
            self.installer.install(self.sha)
        self.assertEqual(pending.read_bytes(), b'original pending settings')
        self.assertEqual(self.snapshot(), before)

    def test_backup_hash_collision_prevents_overwrite(self):
        original = b'preexisting binary'
        self.write(overlay.TARGET, original)
        self.manager.state_path('blobs/' + enhanced.digest(original)).write_bytes(b'incorrect backup')
        with self.assertRaisesRegex(enhanced.Conflict, 'integrity'):
            self.installer.install(self.sha)
        self.assertEqual(self.read(overlay.TARGET), original)
        self.assertIsNone(self.installer.record())

    def test_corrupted_original_backup_prevents_uninstall(self):
        self.write(overlay.TARGET, b'original')
        self.installer.install(self.sha)
        original_hash = self.installer.record()['original']
        self.manager.state_path('blobs/' + original_hash).write_bytes(b'corrupt')
        with self.assertRaisesRegex(enhanced.Conflict, 'integrity'):
            self.installer.uninstall()
        self.assertEqual(self.read(overlay.TARGET), self.data)

    def test_failed_receipt_write_rolls_back_binary_and_receipt(self):
        self.write(overlay.TARGET, b'original')
        original = self.installer.write_record
        calls = []
        def fail_once(record):
            calls.append(record)
            if len(calls) == 1:
                raise OSError('simulated receipt failure')
            return original(record)
        with mock.patch.object(self.installer, 'write_record', fail_once):
            with self.assertRaisesRegex(OSError, 'receipt failure'):
                self.installer.install(self.sha)
        self.assertEqual(self.read(overlay.TARGET), b'original')
        self.assertIsNone(self.installer.record())
        self.assertFalse(self.manager.state_path(overlay.JOURNAL).exists())

    def test_interrupted_install_recovers_original_before_retry(self):
        self.write(overlay.TARGET, b'original before crash')
        with mock.patch.object(self.installer, 'write_record', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.installer.install(self.sha)
        self.assertTrue(self.manager.state_path(overlay.JOURNAL).exists())
        self.assertEqual(self.read(overlay.TARGET), self.data)
        self.assertEqual(self.installer.status()['integrity'], 'recovery-required')
        self.installer.install(self.sha)
        self.installer.uninstall()
        self.assertEqual(self.read(overlay.TARGET), b'original before crash')

    def test_external_edit_after_interruption_blocks_recovery(self):
        with mock.patch.object(self.installer, 'write_record', side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.installer.install(self.sha)
        self.write(overlay.TARGET, b'new external edit')
        with self.assertRaisesRegex(enhanced.Conflict, 'external edit'):
            self.installer.install(self.sha)
        self.assertEqual(self.read(overlay.TARGET), b'new external edit')
        self.assertTrue(self.manager.state_path(overlay.JOURNAL).exists())

    def test_untrusted_receipt_paths_cannot_redirect_uninstall(self):
        self.installer.install(self.sha)
        record = self.installer.record()
        record['target'] = '../../unrelated.dll'
        self.manager.state_path(overlay.RECORD).write_text(json.dumps(record), 'utf-8')
        with self.assertRaises(enhanced.Conflict):
            self.installer.uninstall()
        self.assertEqual(self.read(overlay.TARGET), self.data)

    def test_redirected_target_and_build_source_are_rejected(self):
        original_resolve = Path.resolve
        other = Path(self.temp.name) / 'outside.dll'
        other.write_bytes(b'untouched')
        for redirected in (self.root / overlay.TARGET, self.source):
            def changed_resolve(path, *args, **kwargs):
                return other if path == redirected else original_resolve(path, *args, **kwargs)
            with mock.patch.object(Path, 'resolve', changed_resolve):
                with self.assertRaises(enhanced.Conflict):
                    self.installer.install(self.sha)
            self.assertEqual(other.read_bytes(), b'untouched')

    def test_status_does_not_create_state_in_uninstalled_root(self):
        root = Path(self.temp.name) / 'empty-game'
        root.mkdir()
        result = overlay.OverlayInstaller(root).status()
        self.assertEqual(result['integrity'], 'absent')
        self.assertEqual(list(root.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
