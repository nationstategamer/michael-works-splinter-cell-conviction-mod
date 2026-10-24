"""Clean public setup tests use owned temporary games and save groups only."""
from pathlib import Path
import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import public_setup as setup
import overlay_bridge as bridge
import package as release_package


def fake_dll():
    data = bytearray(320)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x14c, 1)
    struct.pack_into('<HH', data, 148, 96, 0x2102)
    struct.pack_into('<H', data, 152, 0x10b)
    return bytes(data)


@unittest.skipUnless(sys.platform == 'win32', 'Windows plain-path and save-lock semantics')
class PublicSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='mw-public-setup-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.game = self.root / 'game'
        self.game.mkdir()
        self.pkg = self.root / 'source-checkout' / 'ConvictionEnhanced'
        self.pkg.mkdir(parents=True)
        self.launcher = self.root / 'Ubisoft Game Launcher'
        self.source = self.launcher / 'savegames' / 'account-one' / '2'
        self.source.mkdir(parents=True)
        (self.source / '1.save').write_bytes(b'original-profile')
        (self.source / '2.save').write_bytes(b'original-checkpoint')
        (self.launcher / 'ubiorbitapi_r2.dll').write_bytes(b'owned-sdk-marker-fixture')
        self.write('src/system/conviction_game.exe', b'owned-game-fixture')
        self.write('src/system/scripts/SplinterCellConviction.FusionFix.asi', b'owned-fusion-fixture')
        self.write(setup.enhanced.FUSION, b'[GAMEPLAY]\r\nBlacklistControlScheme=0\r\n')
        self.write('src/system/Xidi.ini', b'; personal controller mapping, preserve exactly\r\n')
        requirements = {}
        for name, (label, _) in setup.PREREQUISITES.items():
            data = ('owned prerequisite ' + name).encode('ascii')
            self.write(name, data)
            requirements[name] = (label, hashlib.sha256(data).hexdigest())
        for target, name, value in (
            (setup, 'PACKAGE', self.pkg),
            (setup.overlay_install, 'PACKAGE', self.pkg),
            (setup, 'PREREQUISITES', requirements),
            (setup.enhanced, 'EXE_HASH', hashlib.sha256(b'owned-game-fixture').hexdigest()),
            (setup.enhanced, 'FUSION_HASH', hashlib.sha256(b'owned-fusion-fixture').hexdigest()),
        ):
            patch = mock.patch.object(target, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        for target, name, options in (
            (setup.enhanced, 'game_running', {'return_value': False}),
            (setup, 'launcher_roots', {'return_value': [self.launcher]}),
            (release_package, 'production_exports', {'return_value': ['InitializeASI']}),
        ):
            patch = mock.patch.object(target, name, **options)
            patch.start()
            self.addCleanup(patch.stop)
        self.dll = fake_dll()
        self.artifact = self.pkg / 'native/build/ConvictionEnhanced.asi'
        self.artifact.parent.mkdir(parents=True)
        self.artifact.write_bytes(self.dll)
        self.build = {'schema': 1, 'overlay_version': '0.8.2',
                      'sha256': hashlib.sha256(self.dll).hexdigest(),
                      'production_exports': ['InitializeASI']}
        (self.pkg / 'BUILD.json').write_text(json.dumps(self.build))

    def write(self, relative, data):
        path = self.game / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def source_bytes(self):
        return {p.name: p.read_bytes() for p in self.source.iterdir()}

    def test_source_checkout_requires_explicit_game(self):
        with self.assertRaisesRegex(setup.SetupError, '--game'):
            setup.resolve_game()
        self.assertEqual(setup.resolve_game(self.game), self.game)
        self.assertFalse((self.pkg.parent / 'ConvictionEnhanced-state').exists())

    def test_unsupported_game_refuses_before_backup_or_install(self):
        self.write('src/system/conviction_game.exe', b'different-game')
        with self.assertRaisesRegex(setup.enhanced.Conflict, 'Unsupported game'):
            setup.install(self.game, self.source)
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())
        self.assertFalse((self.game / setup.overlay_install.TARGET).exists())

    def test_missing_or_changed_prerequisite_preserves_user_file(self):
        relative = next(iter(setup.PREREQUISITES))
        self.write(relative, b'user-changed-dependency')
        with self.assertRaisesRegex(setup.SetupError, 'prerequisite'):
            setup.install(self.game, self.source)
        self.assertEqual((self.game / relative).read_bytes(), b'user-changed-dependency')
        self.assertFalse((self.game / setup.overlay_install.TARGET).exists())

    def test_core_install_needs_no_baselines_or_presets(self):
        original = self.source_bytes()
        controller = (self.game / 'src/system/Xidi.ini').read_bytes()
        with mock.patch.object(setup.enhanced.Manager, 'install') as optional:
            result = setup.install(self.game, self.source)
        optional.assert_not_called()
        self.assertTrue(result['installed'])
        self.assertEqual((self.game / setup.overlay_install.TARGET).read_bytes(), self.dll)
        self.assertEqual(self.source_bytes(), original)
        self.assertEqual((self.game / 'src/system/Xidi.ini').read_bytes(), controller)
        self.assertFalse((self.game / 'ConvictionEnhanced-state/active.json').exists())
        status = bridge.refresh(self.game)
        self.assertEqual((status['Installed'], status['Integrity'], status['CanQueue']), ('1', 'ok', '0'))
        self.assertIn('baseline', status['Message'])

    def test_verified_build_hash_and_exports_required(self):
        self.artifact.write_bytes(self.dll + b'tampered')
        with self.assertRaisesRegex(setup.SetupError, 'BUILD.json'):
            setup.install(self.game, self.source)
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())
        self.artifact.write_bytes(self.dll)
        with mock.patch.object(release_package, 'production_exports', return_value=['InitializeASI', 'CETestMove']):
            with self.assertRaisesRegex(setup.SetupError, 'exports'):
                setup.install(self.game, self.source)

    def test_source_only_checkout_reports_missing_reviewed_build(self):
        (self.pkg / 'BUILD.json').unlink()
        with self.assertRaisesRegex(setup.SetupError, 'Download the complete release'):
            setup.reviewed_build()

    def test_multiple_accounts_require_explicit_selection(self):
        another = self.launcher / 'savegames/account-two/2'
        another.mkdir(parents=True)
        (another / '1.save').write_bytes(b'other owned account')
        with self.assertRaisesRegex(setup.SetupError, 'More than one'):
            setup.choose_save_source()
        self.assertEqual(setup.choose_save_source(self.source), self.source)
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())

    def test_explicit_save_source_requires_ubisoft_product_hierarchy(self):
        other = self.root / 'unrelated/2'
        other.mkdir(parents=True)
        (other / '1.save').write_bytes(b'not a supported save group')
        with self.assertRaisesRegex(setup.SetupError, 'savegames'):
            setup.choose_save_source(other)

    def test_save_preparation_commits_marker_and_coherent_backup_without_writes(self):
        before = self.source_bytes()
        result = setup.prepare_saves(self.game, self.source)
        self.assertTrue(result['ready'])
        self.assertEqual(result['files'], 2)
        self.assertTrue(setup.save_protection.verify_originals(Path(result['snapshot']))['unchanged'])
        marker = self.game / 'ConvictionEnhanced-state/practice-originals.ini'
        self.assertIn(str(self.source), marker.read_text('utf-16'))
        self.assertEqual(self.source_bytes(), before)

    def test_running_game_blocks_preparation_and_install(self):
        with mock.patch.object(setup.enhanced, 'game_running', return_value=True):
            for action in (setup.prepare_saves, setup.install):
                with self.subTest(action=action.__name__), self.assertRaisesRegex(setup.SetupError, 'Exit Conviction'):
                    action(self.game, self.source)
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())

    def test_backup_failure_prevents_plugin_install(self):
        with mock.patch.object(setup.save_protection, 'snapshot_save_group',
                               side_effect=setup.save_protection.SaveProtectionError('owned fixture failure')):
            with self.assertRaises(setup.save_protection.SaveProtectionError):
                setup.install(self.game, self.source)
        self.assertFalse((self.game / setup.overlay_install.TARGET).exists())
        self.assertFalse((self.game / 'ConvictionEnhanced-state/practice-originals.ini').exists())

    def test_fresh_save_preparation_precedes_every_core_only_launch(self):
        setup.install(self.game, self.source)
        marker = self.game / 'ConvictionEnhanced-state/practice-originals.ini'
        snapshots = []
        def start(uri, *, show_cmd):
            self.assertEqual(show_cmd, 1)
            self.assertEqual(uri, 'steam://rungameid/33220')
            self.assertTrue(marker.exists())
            receipt = json.loads((self.game / 'ConvictionEnhanced-state/public-save-backup.json').read_text())
            snapshots.append(receipt['snapshot'])
            self.assertTrue((self.game / 'ConvictionEnhanced-state/operation.lock').exists())
        with mock.patch.object(bridge.os, 'startfile', start, create=True):
            bridge.launch(self.game, self.source)
            bridge.launch(self.game, self.source)
        self.assertEqual(len(snapshots), 2)
        self.assertNotEqual(snapshots[0], snapshots[1])
        self.assertEqual(self.source_bytes(), {'1.save': b'original-profile', '2.save': b'original-checkpoint'})

    def test_preparation_failure_blocks_steam_launch(self):
        setup.install(self.game, self.source)
        with mock.patch.object(bridge, 'prepare_launch_saves', side_effect=setup.SetupError('ambiguous account')):
            with mock.patch.object(bridge.os, 'startfile', create=True) as start:
                with self.assertRaisesRegex(setup.SetupError, 'ambiguous'):
                    bridge.launch(self.game)
                start.assert_not_called()

    def test_core_uninstall_restores_previous_file_and_keeps_backups(self):
        self.write(setup.overlay_install.TARGET, b'owned previous plugin fixture')
        setup.install(self.game, self.source)
        before = self.source_bytes()
        result = setup.uninstall(self.game)
        self.assertTrue(result['uninstalled'])
        self.assertEqual((self.game / setup.overlay_install.TARGET).read_bytes(), b'owned previous plugin fixture')
        self.assertTrue((self.game / 'ConvictionEnhanced-state/save-backups').is_dir())
        self.assertEqual(self.source_bytes(), before)

    def test_changed_plugin_blocks_uninstall_without_overwriting_external_edit(self):
        setup.install(self.game, self.source)
        self.write(setup.overlay_install.TARGET, b'external plugin change')
        with self.assertRaisesRegex(setup.enhanced.Conflict, 'changed since installation'):
            setup.uninstall(self.game)
        self.assertEqual((self.game / setup.overlay_install.TARGET).read_bytes(), b'external plugin change')

    def test_core_only_stale_queue_is_retained_and_never_applied(self):
        setup.install(self.game, self.source)
        pending = self.game / 'ConvictionEnhanced-state/overlay-pending.ini'
        pending.write_bytes(b'unsupported queue fixture')
        with mock.patch.object(bridge.os, 'startfile', create=True) as start:
            with self.assertRaises(setup.enhanced.Conflict):
                bridge.launch(self.game, self.source)
            start.assert_not_called()
        self.assertEqual(pending.read_bytes(), b'unsupported queue fixture')

    def test_optional_presets_import_local_baselines_and_choose_standard_mouse(self):
        import local_baselines
        with mock.patch.object(local_baselines, 'import_baselines', return_value={'ready': True}) as importer:
            with mock.patch.object(setup.enhanced.Manager, 'plan') as plan:
                with mock.patch.object(setup.enhanced.Manager, 'install') as install:
                    setup.install(self.game, self.source, self.root / 'owned-unpacked', ['comfort', 'tactical'])
        importer.assert_called_once_with(self.game, self.root / 'owned-unpacked')
        plan.assert_called_once_with({'tactical'}, mouse_input='standard')
        install.assert_called_once_with({'tactical'}, mouse_input='standard')

    def test_missing_optional_baselines_refuse_before_core_install(self):
        import local_baselines
        with mock.patch.object(local_baselines, 'import_baselines',
                               side_effect=local_baselines.BaselineError('Supply verified local unpacked INIs')):
            with self.assertRaisesRegex(setup.SetupError, 'verified local'):
                setup.install(self.game, self.source, presets=['comfort'])
        self.assertFalse((self.game / setup.overlay_install.TARGET).exists())
        self.assertFalse((self.game / 'ConvictionEnhanced-state/practice-originals.ini').exists())

    def test_malformed_reviewed_manifest_refuses_cleanly(self):
        self.build['sha256'] = 42
        (self.pkg / 'BUILD.json').write_text(json.dumps(self.build))
        with self.assertRaisesRegex(setup.SetupError, 'malformed'):
            setup.reviewed_build()

    def test_diagnose_does_not_expose_account_paths_by_default_or_write_state(self):
        report = setup.diagnose(self.game)
        self.assertEqual(report['save_account_count'], 1)
        self.assertNotIn(str(self.source), json.dumps(report))
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())
        shown = setup.diagnose(self.game, show_save_paths=True)
        self.assertEqual(shown['private_local_save_sources'], [str(self.source)])

    def test_diagnose_permission_failure_omits_private_exception_path(self):
        with mock.patch.object(setup, 'reviewed_build',
                               side_effect=PermissionError('private-user/account-path/BUILD.json')):
            report = setup.diagnose(self.game)
        self.assertFalse(report['ready_for_install'])
        self.assertNotIn('private-user', json.dumps(report))
