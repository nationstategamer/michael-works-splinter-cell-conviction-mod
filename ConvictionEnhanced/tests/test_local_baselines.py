"""Public baseline import checks using synthetic INIs and disposable folders."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

import synthetic_fixtures as synthetic

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
import local_baselines as baselines


class LocalBaselineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='public-baselines-')
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name).resolve()
        self.game = self.folder / 'game'
        self.game.mkdir()
        self.source = self.folder / 'owned-unpacked'
        self.source.mkdir()
        for name, data in synthetic.BASELINES.items():
            (self.source / name).write_bytes(data)
        self.catalog = self.folder / 'hashes.json'
        self.catalog.write_text(json.dumps(synthetic.hashes()), encoding='utf-8')
        self.cache = self.game / 'ConvictionEnhanced-state/baselines'

    def ready(self):
        return baselines.baselines_ready(self.game, catalog_path=self.catalog)

    def install(self, source=None):
        return baselines.import_baselines(self.game, self.source if source is None else source,
                                          catalog_path=self.catalog)

    def test_import_copies_only_five_verified_private_files_and_keeps_source(self):
        (self.source / 'unrelated-map.unr').write_bytes(b'fixture ignored, never imported')
        before = {p.name: p.read_bytes() for p in self.source.iterdir()}
        result = self.install()
        self.assertTrue(result['ready'])
        self.assertEqual(result['files'], 5)
        self.assertEqual(result['imported'], 5)
        self.assertEqual({p.name for p in self.cache.iterdir()}, set(synthetic.BASELINES))
        self.assertEqual({p.name: p.read_bytes() for p in self.source.iterdir()}, before)
        for name, expected in synthetic.BASELINES.items():
            self.assertEqual(baselines.read_baseline(self.game, name, catalog_path=self.catalog), expected)
        self.assertFalse((self.game / 'src').exists())

    def test_missing_cache_is_read_only_and_actionable(self):
        self.assertFalse(self.ready())
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())
        with self.assertRaisesRegex(baselines.BaselineError, 'Import the five pristine'):
            baselines.read_baseline(self.game, 'convictioncamera.ini', catalog_path=self.catalog)
        self.assertFalse((self.game / 'ConvictionEnhanced-state').exists())

    def test_default_source_is_owned_game_unpacked_folder(self):
        destination = self.game / 'src/system/unpacked'
        destination.mkdir(parents=True)
        for name, data in synthetic.BASELINES.items():
            (destination / name).write_bytes(data)
        result = baselines.import_baselines(self.game, catalog_path=self.catalog)
        self.assertTrue(result['ready'])
        self.assertEqual(result['source'], str(destination))

    def test_all_sources_preflight_before_any_cache_write(self):
        for label, name in (('missing', 'laststandmatchmanager.ini'), ('different', 'convictioncamera.ini')):
            with self.subTest(label=label):
                path = self.source / name
                original = path.read_bytes()
                if label == 'missing':
                    path.unlink()
                else:
                    path.write_bytes(b'not a supported pristine configuration')
                with self.assertRaises(baselines.BaselineError):
                    self.install()
                self.assertFalse(self.cache.exists())
                self.assertFalse((self.game / 'src').exists())
                path.write_bytes(original)

    def test_repeat_import_is_a_noop_and_partial_valid_cache_can_be_completed(self):
        self.install()
        with mock.patch.object(baselines.os, 'replace') as replace:
            self.assertEqual(self.install()['imported'], 0)
        replace.assert_not_called()
        (self.cache / 'healthtuning.ini').unlink()
        self.assertFalse(self.ready())
        self.assertEqual(self.install()['imported'], 1)
        self.assertTrue(self.ready())

    def test_changed_private_cache_is_not_overwritten(self):
        self.install()
        changed = self.cache / 'healthtuning.ini'
        changed.write_bytes(b'preserve modified private cache')
        self.assertFalse(self.ready())
        with self.assertRaisesRegex(baselines.BaselineError, 'cache was changed'):
            self.install()
        self.assertEqual(changed.read_bytes(), b'preserve modified private cache')

    def test_copy_failure_never_changes_game_and_retry_finishes_valid_cache(self):
        original_replace = baselines.os.replace
        calls = 0
        def fail_second(source, destination):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('synthetic full disk')
            return original_replace(source, destination)
        with mock.patch.object(baselines.os, 'replace', fail_second):
            with self.assertRaisesRegex(OSError, 'synthetic full disk'):
                self.install()
        self.assertFalse(self.ready())
        self.assertFalse((self.game / 'src').exists())
        self.assertFalse(list(self.cache.glob('*.tmp')))
        self.assertTrue(self.install()['ready'])

    def test_unknown_name_and_malformed_catalog_refuse(self):
        with self.assertRaisesRegex(baselines.BaselineError, 'Unsupported'):
            baselines.read_baseline(self.game, '../outside', catalog_path=self.catalog)
        bad = synthetic.hashes()
        bad['convictioncamera.ini'] = 'not-a-hash'
        self.catalog.write_text(json.dumps(bad), encoding='utf-8')
        with self.assertRaisesRegex(baselines.BaselineError, 'catalog'):
            self.install()
        self.assertFalse(self.cache.exists())

    def link(self, source, destination, directory=False):
        try:
            os.symlink(source, destination, target_is_directory=directory)
        except OSError as error:
            self.skipTest('Symlink creation is unavailable: ' + str(getattr(error, 'winerror', error.errno)))

    def test_symlink_source_leaf_refuses_before_writes(self):
        name = 'convictioncamera.ini'
        outside = self.folder / 'outside.ini'
        outside.write_bytes(synthetic.BASELINES[name])
        (self.source / name).unlink()
        self.link(outside, self.source / name)
        with self.assertRaises(baselines.BaselineError):
            self.install()
        self.assertFalse(self.cache.exists())

    def test_symlink_source_directory_refuses(self):
        link = self.folder / 'linked-unpacked'
        self.link(self.source, link, directory=True)
        with self.assertRaises(baselines.BaselineError):
            self.install(link)
        self.assertFalse(self.cache.exists())

    def test_symlink_destination_directory_refuses_and_preserves_outside(self):
        outside = self.folder / 'outside-state'
        outside.mkdir()
        marker = outside / 'unchanged.txt'
        marker.write_text('keep', encoding='utf-8')
        self.link(outside, self.game / 'ConvictionEnhanced-state', directory=True)
        with self.assertRaises(baselines.BaselineError):
            self.install()
        self.assertEqual(list(outside.iterdir()), [marker])


if __name__ == '__main__':
    unittest.main()
