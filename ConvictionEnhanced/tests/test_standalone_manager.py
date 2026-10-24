"""Standalone ownership tests use synthetic local games and save groups only."""
from pathlib import Path
import hashlib
import json
import msvcrt
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import enhanced
import overlay_install
import public_setup
import standalone_manager as manager


def executable(tag=b'one', dll=False):
    data = bytearray(384)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x14c, 1)
    struct.pack_into('<HH', data, 148, 96, 0x2102 if dll else 0x102)
    struct.pack_into('<H', data, 152, 0x10b)
    data[320:320 + len(tag)] = tag
    return bytes(data)


@unittest.skipUnless(sys.platform == 'win32', 'Windows plain-file/save-lock semantics')
class StandaloneManagerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mw-standalone-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.game = self.root / 'game'
        self.game.mkdir()
        self.package = self.root / 'payload'
        (self.package / 'native/build').mkdir(parents=True)
        self.source = self.root / 'Downloaded Setup.exe'
        self.source.write_bytes(executable())
        self.launcher = self.root / 'Ubisoft Game Launcher'
        self.saves = self.launcher / 'savegames/account-one/2'
        self.saves.mkdir(parents=True)
        (self.saves / '1.save').write_bytes(b'original-profile')
        (self.saves / '2.save').write_bytes(b'original-campaign')
        self.write('src/system/conviction_game.exe', b'owned-game-fixture')
        self.write('src/system/scripts/SplinterCellConviction.FusionFix.asi', b'owned-fusion-fixture')
        self.write(public_setup.enhanced.FUSION, b'[GAMEPLAY]\r\nBlacklistControlScheme=0\r\n')
        self.write('src/system/Xidi.ini', b'; preserve personal input configuration\r\n')
        self.asi = executable(b'core-plugin', dll=True)
        (self.package / 'native/build/ConvictionEnhanced.asi').write_bytes(self.asi)
        (self.package / 'BUILD.json').write_text(json.dumps(dict(
            schema=1, overlay_version='0.8.3', sha256=enhanced.digest(self.asi),
            production_exports=['InitializeASI'])))
        import package
        for target, name, value in (
            (public_setup, 'PACKAGE', self.package),
            (overlay_install, 'PACKAGE', self.package),
            (public_setup, 'PREREQUISITES', {}),
            (enhanced, 'EXE_HASH', enhanced.digest(b'owned-game-fixture')),
            (enhanced, 'FUSION_HASH', enhanced.digest(b'owned-fusion-fixture')),
        ):
            patch = mock.patch.object(target, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        for target, name, options in (
            (enhanced, 'game_running', {'return_value': False}),
            (public_setup, 'launcher_roots', {'return_value': [self.launcher]}),
            (package, 'production_exports', {'return_value': ['InitializeASI']}),
        ):
            patch = mock.patch.object(target, name, **options)
            patch.start()
            self.addCleanup(patch.stop)

    def write(self, relative, data):
        path = self.game / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def state(self):
        return enhanced.Manager(self.game)

    def install(self, **kwargs):
        return manager.install(self.game, self.source, **kwargs)

    def inventory(self):
        return {path.relative_to(self.game).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.game.rglob('*') if path.is_file()
                and path.relative_to(self.game).as_posix() != 'ConvictionEnhanced-state/' + manager.LOCK}

    def originals(self):
        return {path.name: path.read_bytes() for path in self.saves.iterdir()}

    def test_install_uses_real_backend_and_preserves_originals_and_input(self):
        original = self.originals()
        configuration = (self.game / 'src/system/Xidi.ini').read_bytes()
        result = self.install()
        self.assertTrue(result['installed'])
        self.assertTrue(result['manager_installed'])
        self.assertEqual((self.game / manager.TARGET).read_bytes(), self.source.read_bytes())
        self.assertEqual((self.game / overlay_install.TARGET).read_bytes(), self.asi)
        self.assertEqual(self.originals(), original)
        self.assertEqual((self.game / 'src/system/Xidi.ini').read_bytes(), configuration)
        self.assertTrue(self.state().state_path('practice-originals.ini').is_file())
        self.assertEqual(manager._record(self.state())['save_source'], str(self.saves))

    def test_unsupported_game_refuses_without_manager_or_state_changes(self):
        self.write('src/system/conviction_game.exe', b'unknown-game')
        before = self.inventory()
        with self.assertRaisesRegex(enhanced.Conflict, 'Unsupported game'):
            self.install()
        self.assertEqual(self.inventory(), before)
        self.assertFalse(self.state().store.exists())

    def test_unowned_collision_is_refused_before_any_core_or_save_operation(self):
        self.write(manager.TARGET, b'personal-unowned-file')
        before = self.inventory()
        with mock.patch.object(public_setup, 'install') as core:
            with self.assertRaisesRegex(enhanced.Conflict, 'unowned'):
                self.install()
            core.assert_not_called()
        self.assertEqual(self.inventory(), before)

    def test_changed_owned_manager_is_never_overwritten(self):
        self.install()
        self.write(manager.TARGET, executable(b'external-change'))
        before = self.inventory()
        with mock.patch.object(public_setup, 'install') as core:
            with self.assertRaisesRegex(enhanced.Conflict, 'changed'):
                self.install()
            core.assert_not_called()
        self.assertEqual(self.inventory(), before)

    def test_source_is_the_installed_manager_without_replacing_running_file(self):
        self.install()
        installed = self.game / manager.TARGET
        with mock.patch.object(manager, '_write_target', side_effect=AssertionError('must not rewrite')):
            result = manager.install(self.game, installed)
        self.assertTrue(result['manager_installed'])

    def test_core_failure_does_not_place_or_adopt_a_local_manager(self):
        before = self.inventory()
        with mock.patch.object(public_setup, 'install', side_effect=enhanced.Conflict('core conflict')):
            with self.assertRaisesRegex(enhanced.Conflict, 'core conflict'):
                self.install()
        self.assertEqual(self.inventory(), before)
        self.assertFalse((self.game / manager.TARGET).exists())
        self.assertIsNone(manager._record(self.state()))

    def test_successful_owned_update_changes_only_owned_helper_and_keeps_saves(self):
        self.install()
        original = self.originals()
        self.source.write_bytes(executable(b'version-two'))
        result = self.install()
        self.assertTrue(result['manager_installed'])
        self.assertEqual((self.game / manager.TARGET).read_bytes(), self.source.read_bytes())
        self.assertEqual(manager._record(self.state())['installed'], enhanced.digest(self.source.read_bytes()))
        self.assertEqual(self.originals(), original)

    def test_helper_failure_rolls_back_absence_and_reports_core_partial_success(self):
        original = manager._write_record
        def fail_once(store, record):
            if record is not None:
                raise OSError('synthetic ownership-write failure')
            return original(store, record)
        with mock.patch.object(manager, '_write_record', side_effect=fail_once):
            with self.assertRaises(manager.PartialInstallError) as caught:
                self.install()
        self.assertTrue(caught.exception.core_installed)
        self.assertFalse(caught.exception.recovery_required)
        self.assertTrue((self.game / overlay_install.TARGET).is_file())
        self.assertFalse((self.game / manager.TARGET).exists())
        self.assertIsNone(manager._record(self.state()))
        self.assertFalse(self.state().state_path(manager.JOURNAL).exists())

    def test_update_helper_failure_restores_exact_prior_executable_and_record(self):
        self.install()
        previous = manager._record(self.state())
        original_data = (self.game / manager.TARGET).read_bytes()
        self.source.write_bytes(executable(b'version-two'))
        original = manager._write_record
        def fail_new_record(store, record):
            if record is not None and record['installed'] != previous['installed']:
                raise OSError('synthetic update record failure')
            return original(store, record)
        with mock.patch.object(manager, '_write_record', side_effect=fail_new_record):
            with self.assertRaises(manager.PartialInstallError):
                self.install()
        self.assertEqual((self.game / manager.TARGET).read_bytes(), original_data)
        self.assertEqual(manager._record(self.state()), previous)

    def test_remove_retains_manager_preferences_backups_and_original_saves(self):
        self.install()
        original = self.originals()
        helper = (self.game / manager.TARGET).read_bytes()
        preferences = self.write('ConvictionEnhanced-state/interface-preferences.ini', b'private-hud-choice')
        result = manager.remove(self.game)
        self.assertTrue(result['uninstalled'])
        self.assertTrue(result['manager_retained'])
        self.assertEqual((self.game / manager.TARGET).read_bytes(), helper)
        self.assertEqual(preferences.read_bytes(), b'private-hud-choice')
        self.assertEqual(self.originals(), original)
        self.assertFalse((self.game / overlay_install.TARGET).exists())
        self.assertTrue(self.state().state_path('save-backups').is_dir())

    def test_explicit_second_account_is_remembered_and_revalidated(self):
        second = self.launcher / 'savegames/account-two/2'
        second.mkdir(parents=True)
        (second / '1.save').write_bytes(b'second-account-progress')
        with self.assertRaisesRegex(enhanced.Conflict, 'More than one'):
            self.install()
        self.install(save_source=second)
        report = manager.inspect(self.game)
        self.assertTrue(report['ready_for_install'], report['issues'])
        self.assertEqual(report['save_source'], str(second))
        with mock.patch.object(public_setup, 'launch', return_value='launched') as launch:
            self.assertEqual(manager.launch(self.game), 'launched')
            launch.assert_called_once_with(self.game, second)
        (second / '1.save').unlink()
        with mock.patch.object(public_setup, 'launch') as launch:
            with self.assertRaises(enhanced.Conflict):
                manager.launch(self.game)
            launch.assert_not_called()

    def test_inspect_is_read_only_and_accepts_explicit_account(self):
        second = self.launcher / 'savegames/account-two/2'
        second.mkdir(parents=True)
        (second / '1.save').write_bytes(b'second-account-progress')
        before = self.inventory()
        report = manager.inspect(self.game, save_source=second)
        self.assertTrue(report['ready'], report['issues'])
        self.assertEqual(report['save_account_count'], 2)
        self.assertEqual(report['manager']['integrity'], 'absent')
        self.assertEqual(self.inventory(), before)
        self.assertFalse(self.state().store.exists())

    def test_running_game_refuses_every_mutating_action(self):
        with mock.patch.object(enhanced, 'game_running', return_value=True):
            for operation in (lambda: self.install(), lambda: manager.launch(self.game),
                              lambda: manager.remove(self.game), lambda: manager.recover(self.game)):
                with self.subTest(operation=operation):
                    with self.assertRaisesRegex(enhanced.Conflict, 'Exit Conviction'):
                        operation()
        self.assertFalse(self.state().store.exists())

    def test_non_executable_source_refused_before_any_installation(self):
        self.source.write_bytes(b'not a Windows executable')
        before = self.inventory()
        with self.assertRaisesRegex(enhanced.Conflict, 'Windows standalone'):
            self.install()
        self.assertEqual(self.inventory(), before)

    def test_wrong_root_record_refuses_without_changes(self):
        self.install()
        path = self.state().state_path(manager.RECORD)
        record = json.loads(path.read_text('utf-8'))
        record['root'] = str(self.root / 'different-game')
        path.write_text(json.dumps(record))
        before = self.inventory()
        with self.assertRaisesRegex(enhanced.Conflict, 'another game folder'):
            self.install()
        self.assertEqual(self.inventory(), before)

    def interrupted_helper(self):
        self.install()
        store = self.state()
        before = (self.game / manager.TARGET).read_bytes()
        after = executable(b'interrupted-new-manager')
        previous = manager._record(store)
        next_record = dict(previous, installed=enhanced.digest(after))
        journal = dict(schema=1, root=str(store.root), target=manager.TARGET,
                       before=store.save_blob(before), after=store.save_blob(after),
                       previous=previous, next=next_record)
        enhanced.atomic(store.state_path(manager.JOURNAL), enhanced.json_bytes(journal))
        self.write(manager.TARGET, after)
        return before, after, previous

    def test_recover_interrupted_helper_restores_only_exact_owned_manager(self):
        before, _, previous = self.interrupted_helper()
        original = self.originals()
        result = manager.recover(self.game)
        self.assertEqual(result['recovered'], ['manager'])
        self.assertEqual((self.game / manager.TARGET).read_bytes(), before)
        self.assertEqual(manager._record(self.state()), previous)
        self.assertEqual(self.originals(), original)
        self.assertFalse(self.state().state_path(manager.JOURNAL).exists())

    def test_recover_refuses_external_helper_edit_and_keeps_journal(self):
        self.interrupted_helper()
        external = executable(b'external-change-after-crash')
        self.write(manager.TARGET, external)
        with self.assertRaisesRegex(enhanced.Conflict, 'external file edit'):
            manager.recover(self.game)
        self.assertEqual((self.game / manager.TARGET).read_bytes(), external)
        self.assertTrue(self.state().state_path(manager.JOURNAL).exists())

    def test_recover_refuses_corrupt_helper_backup_before_writing(self):
        before, after, _ = self.interrupted_helper()
        self.state().state_path('blobs/' + enhanced.digest(before)).write_bytes(b'corrupt-backup')
        with self.assertRaisesRegex(enhanced.Conflict, 'integrity'):
            manager.recover(self.game)
        self.assertEqual((self.game / manager.TARGET).read_bytes(), after)
        self.assertTrue(self.state().state_path(manager.JOURNAL).exists())

    def test_interrupted_helper_prevents_new_core_install_until_recovered(self):
        self.interrupted_helper()
        with mock.patch.object(public_setup, 'install') as core:
            with self.assertRaises(enhanced.Conflict):
                self.install()
            core.assert_not_called()

    def test_busy_standalone_operation_refuses_and_preserves_other_lock(self):
        store = self.state()
        store.store.mkdir()
        with manager._operation(store):
            with mock.patch.object(public_setup, 'install') as core:
                with self.assertRaisesRegex(enhanced.Conflict, 'Another standalone'):
                    self.install()
                core.assert_not_called()
        self.assertEqual(store.state_path(manager.LOCK).read_bytes(), b'')

    def test_stale_empty_lock_file_does_not_block_recovery_or_next_operation(self):
        store = self.state()
        store.store.mkdir()
        lock = store.state_path(manager.LOCK)
        lock.write_bytes(b'')
        self.assertEqual(manager.recover(self.game)['recovered'], [])
        with manager._operation(store):
            self.assertEqual(lock.stat().st_size, 0)
        with manager._operation(store):
            self.assertEqual(lock.stat().st_size, 0)
        self.assertTrue(lock.is_file())

    def test_os_releases_lock_after_another_process_exits_without_cleanup(self):
        store = self.state()
        store.store.mkdir()
        lock = store.state_path(manager.LOCK)
        child_code = ('import msvcrt,os,sys; '
                      'fd=os.open(sys.argv[1],os.O_CREAT|os.O_RDWR|os.O_BINARY); '
                      'msvcrt.locking(fd,msvcrt.LK_NBLCK,1); '
                      'print("locked",flush=True); '
                      'sys.stdin.read(1); os._exit(0)')
        child = subprocess.Popen([sys.executable, '-c', child_code, str(lock)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        def finish_child():
            if child.poll() is None:
                child.kill()
                child.wait(timeout=10)
            for stream in (child.stdin, child.stdout, child.stderr):
                stream.close()
        self.addCleanup(finish_child)
        self.assertEqual(child.stdout.readline().strip(), 'locked')
        with self.assertRaisesRegex(enhanced.Conflict, 'Another standalone'):
            with manager._operation(store):
                self.fail('The second process acquired a held lock')
        child.stdin.write('x')
        child.stdin.flush()
        self.assertEqual(child.wait(timeout=10), 0)
        with manager._operation(store):
            self.assertEqual(lock.stat().st_size, 0)
        self.assertEqual(lock.read_bytes(), b'')

    def test_remove_refuses_pending_helper_journal_before_core_removal(self):
        self.interrupted_helper()
        with mock.patch.object(public_setup, 'uninstall') as uninstall:
            with self.assertRaisesRegex(enhanced.Conflict, 'Recover interrupted'):
                manager.remove(self.game)
            uninstall.assert_not_called()

    def test_recover_routes_existing_core_and_preset_journals_under_lock(self):
        store = self.state()
        store.store.mkdir()
        store.state_path('pending.json').write_text('synthetic-journal-presence')
        store.state_path(overlay_install.JOURNAL).write_text('synthetic-core-journal-presence')
        def recover_presets(current):
            self.assertTrue(current.state_path('operation.lock').is_file())
            current.state_path('pending.json').unlink()
        def recover_core(current):
            self.assertTrue(current.manager.state_path('operation.lock').is_file())
            current.manager.state_path(overlay_install.JOURNAL).unlink()
        with mock.patch.object(enhanced.Manager, 'recover_unlocked', recover_presets), \
             mock.patch.object(overlay_install.OverlayInstaller, 'recover_unlocked', recover_core):
            result = manager.recover(self.game)
        self.assertEqual(result['recovered'], ['presets', 'core'])
        self.assertFalse(store.state_path('operation.lock').exists())

    def test_recover_without_journal_does_not_change_game_files(self):
        before = self.inventory()
        result = manager.recover(self.game)
        self.assertEqual(result['recovered'], [])
        self.assertEqual(self.inventory(), before)


if __name__ == '__main__':
    unittest.main()
