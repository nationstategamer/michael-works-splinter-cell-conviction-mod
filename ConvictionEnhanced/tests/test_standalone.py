"""Standalone payload, discovery and protected-entry tests use synthetic data."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import standalone


class PayloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mw-standalone-payload-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = b'owned synthetic production plugin'
        sha = hashlib.sha256(self.binary).hexdigest()
        data = {'BUILD.json': json.dumps({'schema': 1, 'overlay_version': '0.8.3', 'sha256': sha,
                                         'production_exports': ['InitializeASI']}).encode(),
                'compatibility.json': b'{}', 'baselines/hashes.json': b'{}',
                'native/build/ConvictionEnhanced.asi': self.binary, 'LICENSE.txt': b'Owned fixture license'}
        self.manifest = {'schema': 1, 'distribution_version': '0.8.4', 'native_sha256': sha,
                         'files': {name: hashlib.sha256(value).hexdigest() for name, value in data.items()}}
        for name, value in data.items():
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(value)
        self.write_manifest()
        for target, name, options in (
                (standalone.public_setup.overlay_install, 'validate_pe', {'return_value': None}),
                (standalone.release_package, 'production_exports', {'return_value': ['InitializeASI']})):
            patch = mock.patch.object(target, name, **options)
            patch.start()
            self.addCleanup(patch.stop)

    def write_manifest(self):
        (self.root / 'STANDALONE-PAYLOAD.json').write_text(json.dumps(self.manifest), 'utf-8')

    def test_valid_owned_payload(self):
        report = standalone.verify_payload(self.root)
        self.assertTrue(report['ok'])
        self.assertEqual(report['payload_files'], 5)
        self.assertEqual(report['production_sha256'], hashlib.sha256(self.binary).hexdigest())

    def test_changed_resource_refuses(self):
        (self.root / 'compatibility.json').write_bytes(b'{"changed":true}')
        with self.assertRaisesRegex(standalone.public_setup.SetupError, 'integrity'):
            standalone.verify_payload(self.root)

    def test_traversal_never_reads_outside(self):
        for name in ('../outside.txt', '/outside.txt', 'C:/outside.txt', 'folder\\outside.txt'):
            with self.subTest(name=name):
                self.manifest['files'][name] = '0' * 64
                self.write_manifest()
                with self.assertRaisesRegex(standalone.public_setup.SetupError, 'unsafe'):
                    standalone.verify_payload(self.root)
                self.manifest['files'].pop(name)

    def test_missing_required_resource_refuses(self):
        self.manifest['files'].pop('compatibility.json')
        self.write_manifest()
        with self.assertRaisesRegex(standalone.public_setup.SetupError, 'missing required'):
            standalone.verify_payload(self.root)

    def test_changed_release_version_refuses(self):
        self.manifest['distribution_version'] = 'other'
        self.write_manifest()
        with self.assertRaisesRegex(standalone.public_setup.SetupError, 'invalid'):
            standalone.verify_payload(self.root)

    def test_mismatched_native_identity_refuses(self):
        self.manifest['native_sha256'] = '0' * 64
        self.write_manifest()
        with self.assertRaisesRegex(standalone.public_setup.SetupError, 'identities'):
            standalone.verify_payload(self.root)

    def test_test_exports_refuse(self):
        with mock.patch.object(standalone.release_package, 'production_exports', return_value=['InitializeASI', 'CETest']):
            with self.assertRaisesRegex(standalone.public_setup.SetupError, 'production'):
                standalone.verify_payload(self.root)

    def test_self_check_report_never_overwrites(self):
        report = self.root / 'result.json'
        report.write_bytes(b'keep existing report')
        with self.assertRaises(FileExistsError):
            standalone.write_check(report, {'ok': True})
        self.assertEqual(report.read_bytes(), b'keep existing report')

    def test_self_check_report_requires_json(self):
        path = self.root / 'game.exe'
        with self.assertRaises(ValueError):
            standalone.write_check(path, {})
        self.assertFalse(path.exists())


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mw-steam-discovery-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def game(self, root):
        path = root / 'src/system/conviction_game.exe'
        path.parent.mkdir(parents=True)
        path.write_bytes(b'owned discovery marker only')
        return root

    def manifest(self, library, name):
        apps = library / 'steamapps'
        apps.mkdir(parents=True, exist_ok=True)
        (apps / 'appmanifest_33220.acf').write_text('"installdir" "' + name + '"', 'utf-8')
        return apps / 'common' / name

    def test_installed_executable_location_wins_over_temp_bundle(self):
        game = self.game(self.root / 'actual game')
        exe = game / 'ConvictionEnhanced/Michael Works Mod.exe'
        self.assertEqual(standalone.discover_games(exe, []), [game])

    def test_library_manifest_discovery_and_deduplication(self):
        steam, library = self.root / 'Steam', self.root / 'Library'
        apps = steam / 'steamapps'
        apps.mkdir(parents=True)
        (apps / 'libraryfolders.vdf').write_text('"path" "' + str(library).replace('\\', '\\\\') + '"', 'utf-8')
        game = self.game(self.manifest(library, 'Custom Conviction'))
        self.assertEqual(standalone.discover_games(self.root / 'Setup.exe', [steam, library]), [game])

    def test_multiple_installs_are_returned_without_choosing(self):
        a, b = self.root / 'A', self.root / 'B'
        expected = [self.game(self.manifest(a, 'First')), self.game(self.manifest(b, 'Second'))]
        self.assertEqual(standalone.discover_games(self.root / 'Setup.exe', [a, b]), expected)

    def test_manifest_cannot_escape_library(self):
        library = self.root / 'library'
        self.manifest(library, '../escape')
        self.assertEqual(standalone.discover_games(self.root / 'Setup.exe', [library]), [])

    def test_private_account_is_redacted(self):
        text = standalone.friendly_error(ValueError(r'Failure in C:\Launcher\savegames\private-account\2'))
        self.assertNotIn('private-account', text)

    def test_os_error_does_not_expose_private_path(self):
        text = standalone.friendly_error(PermissionError(r'private-account secret-path'))
        self.assertNotIn('private-account', text)

    def test_launch_failure_has_no_direct_steam_fallback(self):
        with mock.patch.object(standalone.standalone_manager, 'launch', side_effect=ValueError('save preparation refused')) as launch, \
                mock.patch('tkinter.messagebox.showerror') as show, \
                mock.patch('os.startfile', create=True) as start:
            self.assertEqual(standalone.main(['--launch', '--game', str(self.root)]), 1)
        launch.assert_called_once()
        start.assert_not_called()
        show.assert_called_once()

    def test_gui_self_check_refuses_any_nonempty_game_folder(self):
        marker = self.root / 'keep.txt'
        marker.write_bytes(b'keep existing contents')
        with mock.patch.object(standalone, 'ManagerWindow') as window:
            with self.assertRaisesRegex(ValueError, 'nonempty'):
                standalone.gui_self_check(self.root)
        window.assert_not_called()
        self.assertEqual(marker.read_bytes(), b'keep existing contents')

    def test_gui_self_check_without_explicit_empty_fixture_never_opens_window(self):
        report = self.root / 'ui-report.json'
        with mock.patch.object(standalone, 'ManagerWindow') as window:
            self.assertEqual(standalone.main(['--ui-self-check', str(report)]), 1)
        window.assert_not_called()
        self.assertFalse(json.loads(report.read_text('utf-8'))['ok'])


if __name__ == '__main__':
    unittest.main()
