"""Focused public release boundary tests, using synthetic bytes only."""
from pathlib import Path
import importlib.util
import hashlib
import json
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

PACKAGE = Path(__file__).resolve().parents[1]
AUDIT_PATH = PACKAGE.parent / 'tools/public_audit.py'
if not AUDIT_PATH.is_file():
    AUDIT_PATH = PACKAGE / 'tools/public_audit.py'
SPEC = importlib.util.spec_from_file_location('public_audit_under_test', AUDIT_PATH)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class PublicBoundaryTests(unittest.TestCase):
    def test_only_designated_production_binary_is_allowed(self):
        for name in ('ConvictionEnhanced/native/build/overlay_test.dll', 'game.exe',
                     'ConvictionEnhanced/native/build/harness.exe'):
            self.assertFalse(audit.audit_payload({name: b'fixture'}, release=True)['ok'])
        self.assertTrue(audit.audit_payload({audit.PRODUCTION_BINARY: b'fixture'}, release=True)['ok'])
        self.assertFalse(audit.audit_payload({audit.PRODUCTION_BINARY: b'fixture'})['ok'])

    def test_game_baselines_and_private_state_never_publish(self):
        for name in ('ConvictionEnhanced/baselines/convictioncamera.ini',
                     'ConvictionEnhanced-state/save-backups/fixture.json',
                     'ConvictionEnhanced/native/analysis/diagnostic.json',
                     'src/system/UMDS/fixture.umd', '2.save', '2.save.upload', '.env'):
            self.assertFalse(audit.audit_payload({name: b'fixture'}, release=True)['ok'], name)
        self.assertTrue(audit.audit_payload({'ConvictionEnhanced/baselines/hashes.json': b'{}'})['ok'])

    def test_private_paths_and_credentials_are_detected_without_echo(self):
        secret = 'ghp_' + 'A' * 36
        windows_path = 'C:' + chr(92) + 'Users' + chr(92) + 'FixturePerson' + chr(92) + 'work.cpp'
        for data in (secret.encode(), windows_path.encode(), windows_path.encode('utf-16le')):
            report = audit.audit_payload({'fixture.txt': data})
            self.assertFalse(report['ok'])
            self.assertNotIn(secret, json.dumps(report))
            self.assertNotIn('FixturePerson', json.dumps(report))

    def test_binary_absolute_source_paths_are_detected_in_both_encodings(self):
        path = 'D:' + chr(92) + 'BuildRoot' + chr(92) + 'native' + chr(92) + 'fixture.cpp'
        for data in (path.encode(), path.encode('utf-16le')):
            self.assertFalse(audit.audit_payload({audit.PRODUCTION_BINARY: data}, release=True)['ok'])

    def test_unsafe_archive_paths_are_rejected(self):
        for name in ('../outside.py', '/outside.py', 'C:/outside.py', 'folder' + chr(92) + 'file.py'):
            self.assertFalse(audit.audit_payload({name: b'fixture'})['ok'])

    def test_renaming_full_game_content_does_not_bypass_audit(self):
        synthetic = b'synthetic baseline fixture, not game data'
        with mock.patch.object(audit, 'known_game_hashes', return_value={hashlib.sha256(synthetic).hexdigest()}):
            report = audit.audit_payload({'notes.txt': synthetic})
        self.assertFalse(report['ok'])
        self.assertEqual(report['findings'][0]['reason'], 'known full game baseline or asset content')


    def test_real_catalog_read_detects_renamed_baseline_and_fusion_asset(self):
        baseline = b'hand-authored synthetic baseline content'
        asset = b'hand-authored synthetic control asset content'
        for portable in (False, True):
            with self.subTest(portable=portable), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                package = root if portable else root / 'ConvictionEnhanced'
                (package / 'baselines').mkdir(parents=True)
                (package / 'baselines/hashes.json').write_text(json.dumps({
                    'fixture.ini': hashlib.sha256(baseline).hexdigest()}), encoding='utf-8')
                (package / 'compatibility.json').write_text(json.dumps({
                    'blacklist_assets': {'fixture.xml': hashlib.sha256(asset).hexdigest()},
                    'notes': 'metadata is not a content hash'}), encoding='utf-8')
                with mock.patch.object(audit, 'ROOT', root):
                    self.assertEqual(audit.known_game_hashes(), {
                        hashlib.sha256(baseline).hexdigest(), hashlib.sha256(asset).hexdigest()})
                    for data in (baseline, asset):
                        report = audit.audit_payload({'renamed-notes.md': data})
                        self.assertFalse(report['ok'])
                        self.assertEqual(report['findings'][0]['reason'],
                                         'known full game baseline or asset content')

    def test_malformed_trusted_asset_hash_catalog_cannot_silently_disable_detection(self):
        for value in ({'not-a-hash': 'bad'}, [], None):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                (root / 'compatibility.json').write_text(json.dumps({'blacklist_assets': value}), encoding='utf-8')
                with mock.patch.object(audit, 'ROOT', root):
                    with self.assertRaisesRegex(ValueError, 'trusted game hash catalog'):
                        audit.known_game_hashes()

    def test_all_test_work_prefixes_refuse_even_ordinary_file_extensions(self):
        for name in ('test-work/notes.txt',
                     'ConvictionEnhanced/native/test-work-public-final/notes.json',
                     'TEST-WORK-FAILED/synthetic.cpp'):
            for release in (False, True):
                with self.subTest(name=name, release=release):
                    report = audit.audit_payload({name: b'generated fixture'}, release=release)
                    self.assertFalse(report['ok'])
                    self.assertIn('generated test output', {item['reason'] for item in report['findings']})
        self.assertTrue(audit.audit_payload({'native/test_worker.cpp': b'synthetic source'})['ok'])

    def test_pre_git_inventory_excludes_generated_test_work_prefixes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'README.md').write_text('public fixture', encoding='utf-8')
            for name in ('test-work', 'test-work-public-final', 'TEST-WORK-FAILED'):
                target = root / 'native' / name
                target.mkdir(parents=True)
                (target / 'notes.json').write_text('{}', encoding='utf-8')
            with mock.patch.object(audit.subprocess, 'run', side_effect=FileNotFoundError):
                payload = audit.source_files(root)
            self.assertEqual(set(payload), {'README.md'})

    def test_tracked_generated_test_work_cannot_hide_behind_inventory_exclusion(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'README.md').write_text('public fixture', encoding='utf-8')
            target = root / 'native/test-work-public-final'
            target.mkdir(parents=True)
            (target / 'notes.json').write_text('{}', encoding='utf-8')
            results = [SimpleNamespace(stdout=str(root) + '\n'),
                       SimpleNamespace(stdout=b'README.md\0native/test-work-public-final/notes.json\0')]
            with mock.patch.object(audit.subprocess, 'run', side_effect=results):
                payload = audit.source_files(root)
            self.assertEqual(len(payload), 2)
            report = audit.audit_payload(payload)
            self.assertFalse(report['ok'])
            self.assertIn('generated test output', {item['reason'] for item in report['findings']})


if __name__ == '__main__':
    unittest.main()
