"""Bridge integration tests using disposable games; never launches a real game."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

import test_enhanced as fixtures

enhanced = fixtures.enhanced
SPEC = importlib.util.spec_from_file_location('overlay_bridge_under_test', fixtures.PACKAGE / 'overlay_bridge.py')
bridge = importlib.util.module_from_spec(SPEC)
with mock.patch.dict(sys.modules, {'enhanced': enhanced}):
    SPEC.loader.exec_module(bridge)


class BridgeTests(unittest.TestCase):
    write = fixtures.IsolatedGame.write
    read = fixtures.IsolatedGame.read
    snapshot = fixtures.IsolatedGame.snapshot

    def setUp(self):
        fixtures.IsolatedGame.setUp(self)
        preparation = mock.patch.object(bridge, 'prepare_launch_saves', return_value={'ready': True})
        preparation.start()
        self.addCleanup(preparation.stop)
        self.manager.install({'tactical', 'deniable'}, mouse_input='standard')
        self.controller = 'src/system/update/controller-fixture.ini'
        self.write(self.controller, b'UseController=1\r\nSteamInput=unchanged\r\n')

    def queue(self, **overrides):
        selected = bridge.selection_from_active(self.manager.active())
        selected.update(overrides)
        data = bridge.render_ini({'Selection': selected, 'Request': {
            'Schema': '1', 'BaseRevision': bridge.revision(self.manager)}})
        enhanced.atomic(self.manager.state_path(bridge.PENDING), data)
        return data

    def test_refresh_exports_verified_selection_without_changing_game_or_active(self):
        before, active = self.snapshot(), self.manager.state_path('active.json').read_bytes()
        self.write('src/system/conviction_game.exe', b'fixture executable')
        before = self.snapshot()
        result = bridge.refresh(self.root)
        self.assertEqual(result['Integrity'], 'ok')
        self.assertEqual(result['CanQueue'], '1')
        self.assertEqual(result['GameSHA256'], enhanced.digest(b'fixture executable'))
        self.assertRegex(result['CheckedAtUTC'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$')
        text = self.manager.state_path(bridge.STATUS).read_text('ascii')
        self.assertIn('MouseInput=standard', text)
        self.assertNotIn(str(self.root), text)
        self.assertNotIn('diffs_from_baseline', text)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path('active.json').read_bytes(), active)

    def test_apply_explicit_selection_preserves_controller_and_other_options(self):
        self.queue(FOV='100', Deadzone='0.25', Controls='original', MouseInput='standard', Tactical='0')
        bridge.apply(self.root)
        selected = self.manager.active()['selection']
        self.assertEqual(selected['fov'], 100)
        self.assertEqual(selected['deadzone'], 0.25)
        self.assertEqual(selected['controls'], 'original')
        self.assertEqual(selected['mouse_input'], 'standard')
        self.assertEqual(selected['modules'], ['comfort', 'deniable'])
        self.assertIn(b'UnrelatedUserChoice=73', self.read(enhanced.FUSION))
        self.assertEqual(self.read(self.controller), b'UseController=1\r\nSteamInput=unchanged\r\n')
        self.assertFalse(self.manager.state_path(bridge.PENDING).exists())
        self.assertTrue(self.manager.state_path('overlay-applied.ini').exists())

    def test_explicit_raw_mouse_requires_selected_raw_request(self):
        self.queue(MouseInput='raw')
        bridge.apply(self.root)
        self.assertEqual(self.manager.active()['selection']['mouse_input'], 'raw')

    def test_noop_is_acknowledged_without_install_or_game_write(self):
        queued = self.queue()
        before, active = self.snapshot(), self.manager.state_path('active.json').read_bytes()
        with mock.patch.object(bridge.CheckedManager, 'install') as install:
            result = bridge.apply(self.root)
        install.assert_not_called()
        self.assertIn('already match', result)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path('active.json').read_bytes(), active)
        self.assertEqual(self.manager.state_path('overlay-applied.ini').read_bytes(), queued)

    def test_stale_request_is_retained_and_does_not_overwrite_newer_settings(self):
        queued = self.queue(FOV='100')
        self.manager.install(set(), mouse_input='standard')
        before = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, 'stale'):
            bridge.apply(self.root)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), queued)
        self.assertEqual(bridge.refresh(self.root)['Pending'], 'stale')

    def test_revision_race_is_rechecked_inside_install_lock(self):
        queued = self.queue(FOV='100')
        real_install = bridge.CheckedManager.install
        def intervening_install(checked, *args, **kwargs):
            self.manager.install(set(), fov=90, mouse_input='standard')
            return real_install(checked, *args, **kwargs)
        with mock.patch.object(bridge.CheckedManager, 'install', intervening_install):
            with self.assertRaisesRegex(enhanced.Conflict, 'stale'):
                bridge.apply(self.root)
        self.assertEqual(self.manager.active()['selection']['fov'], 90)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), queued)

    def test_queue_race_is_rechecked_inside_install_lock(self):
        self.queue(FOV='100')
        real_install = bridge.CheckedManager.install
        replacement = []
        def replace_queue(checked, *args, **kwargs):
            replacement.append(self.queue(FOV='90'))
            return real_install(checked, *args, **kwargs)
        with mock.patch.object(bridge.CheckedManager, 'install', replace_queue):
            with self.assertRaisesRegex(enhanced.Conflict, 'changed'):
                bridge.apply(self.root)
        self.assertEqual(self.manager.active()['selection']['fov'], 95)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), replacement[0])

    def test_external_file_edit_blocks_apply_and_is_retained(self):
        queued = self.queue(FOV='100')
        self.write(enhanced.FUSION, self.read(enhanced.FUSION) + b'; external edit\r\n')
        before = self.snapshot()
        with self.assertRaises(enhanced.Conflict):
            bridge.apply(self.root)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), queued)
        self.assertEqual(bridge.refresh(self.root)['CanQueue'], '0')

    def test_failed_install_rolls_back_and_keeps_request(self):
        queued = self.queue(FOV='100', MouseInput='raw')
        before, active = self.snapshot(), self.manager.state_path('active.json').read_bytes()
        write_target = enhanced.Manager.write_target
        calls = []
        def fail_once(manager, relative, value):
            calls.append(relative)
            if len(calls) == 2:
                raise OSError('simulated disk failure')
            return write_target(manager, relative, value)
        with mock.patch.object(bridge.CheckedManager, 'write_target', fail_once):
            with self.assertRaisesRegex(OSError, 'simulated'):
                bridge.apply(self.root)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path('active.json').read_bytes(), active)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), queued)
        self.assertFalse(self.manager.state_path('pending.json').exists())

    def test_running_game_blocks_mutation_and_launch(self):
        queued = self.queue(FOV='100')
        before = self.snapshot()
        with mock.patch.object(enhanced, 'game_running', return_value=True):
            for action in (bridge.apply, bridge.launch, bridge.discard):
                with self.subTest(action=action.__name__), self.assertRaises(enhanced.Conflict):
                    action(self.root)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), queued)

    def test_manager_lock_and_bridge_lock_block_conflicting_operations(self):
        self.queue(FOV='100')
        before = self.snapshot()
        for lock in ('operation.lock', 'overlay-operation.lock'):
            with self.subTest(lock=lock):
                self.manager.state_path(lock).write_bytes(b'other process')
                try:
                    with self.assertRaises(enhanced.Conflict):
                        bridge.apply(self.root)
                    self.assertEqual(self.manager.state_path(lock).read_bytes(), b'other process')
                finally:
                    self.manager.state_path(lock).unlink()
        self.assertEqual(self.snapshot(), before)

    def test_interrupted_manager_transaction_blocks_apply_and_launch(self):
        self.queue(FOV='100')
        self.manager.state_path('pending.json').write_bytes(b'{}')
        with self.assertRaises(enhanced.Conflict):
            bridge.apply(self.root)
        result = bridge.refresh(self.root)
        self.assertEqual(result['PendingRecovery'], '1')
        self.assertEqual(result['CanQueue'], '0')
        with mock.patch.object(bridge.os, 'startfile', create=True) as start:
            with self.assertRaises(enhanced.Conflict):
                bridge.launch(self.root)
            start.assert_not_called()

    def test_launch_applies_before_requesting_fixed_steam_uri(self):
        self.queue(FOV='100')
        observed = []
        def start(uri, *, show_cmd):
            self.assertEqual(show_cmd, 1)
            observed.append((uri, self.manager.active()['selection']['fov']))
            self.assertTrue(self.manager.state_path('operation.lock').exists())
        with mock.patch.object(bridge.os, 'startfile', start, create=True):
            bridge.launch(self.root)
        self.assertEqual(observed, [('steam://rungameid/33220', 100)])

    def test_launch_refuses_incompatible_fusion(self):
        with mock.patch.object(enhanced.Manager, 'inspect', side_effect=enhanced.Conflict('wrong build')):
            with mock.patch.object(bridge.os, 'startfile', create=True) as start:
                with self.assertRaises(enhanced.Conflict):
                    bridge.launch(self.root)
                start.assert_not_called()

    def test_damaged_blacklist_assets_disable_queue_and_block_launch(self):
        catalog = json.loads((enhanced.PACKAGE / 'compatibility.json').read_text('utf-8'))
        damaged = next(iter(catalog['blacklist_assets']))
        self.write(damaged, b'damaged controls')
        self.assertEqual(bridge.refresh(self.root)['CanQueue'], '0')
        with mock.patch.object(bridge.os, 'startfile', create=True) as start:
            with self.assertRaises(enhanced.Conflict):
                bridge.launch(self.root)
            start.assert_not_called()

    def test_absent_active_has_safe_diagnostics_and_launch_refuses(self):
        self.manager.uninstall()
        result = bridge.refresh(self.root)
        self.assertEqual(result['Installed'], '0')
        self.assertEqual(result['Integrity'], 'not-installed')
        self.assertEqual(result['CanQueue'], '0')
        with mock.patch.object(bridge.os, 'startfile', create=True) as start:
            with self.assertRaises(enhanced.Conflict):
                bridge.launch(self.root)
            start.assert_not_called()

    def test_invalid_requests_never_change_game_files(self):
        good = self.queue(FOV='100')
        invalid = [
            good.replace(b'FOV=100', b'FOV=101'),
            good.replace(b'Deadzone=0.15', b'Deadzone=nan'),
            good.replace(b'Tactical=1', b'Tactical=true'),
            good.replace(b'Schema=1', b'Schema=2'),
            good.replace(b'MouseInput=standard\r\n', b''),
            good + b'[Command]\r\nRun=anything\r\n',
            good + b'[DEFAULT]\r\nPath=../outside\r\n',
            good.replace(b'Controls=blacklist', b'Controls=blacklist\r\nControls=original'),
            good.replace(b'Controls=blacklist', b'Controls=../../anything'),
            good + b'\x00', b'\xff', b'x' * 4097,
        ]
        before = self.snapshot()
        for data in invalid:
            with self.subTest(data=data[:80]):
                self.manager.state_path(bridge.PENDING).write_bytes(data)
                with self.assertRaises(enhanced.Conflict):
                    bridge.apply(self.root)
                self.assertEqual(self.snapshot(), before)
                self.assertEqual(self.manager.state_path(bridge.PENDING).read_bytes(), data)
                self.assertEqual(bridge.refresh(self.root)['Pending'], 'invalid')

    def test_discard_archives_invalid_request_without_changing_settings(self):
        before = self.snapshot()
        self.manager.state_path(bridge.PENDING).write_bytes(b'invalid request')
        bridge.discard(self.root)
        self.assertFalse(self.manager.state_path(bridge.PENDING).exists())
        self.assertEqual(self.manager.state_path('overlay-discarded.ini').read_bytes(), b'invalid request')
        self.assertEqual(self.snapshot(), before)

    def test_nonexistent_root_is_rejected_without_creating_it(self):
        root = self.root / 'not-a-game'
        for action in (bridge.refresh, bridge.apply, bridge.launch, bridge.discard):
            with self.subTest(action=action.__name__), self.assertRaises(enhanced.Conflict):
                action(root)
        self.assertFalse(root.exists())

    def test_redirected_queue_and_status_paths_are_rejected(self):
        destination = Path(self.temp.name) / 'unrelated.ini'
        destination.write_bytes(b'preserve this')
        original_resolve = Path.resolve
        for name in (bridge.PENDING, bridge.STATUS):
            linked = self.manager.store / name
            # Deterministically exercise the same resolution guard on hosts that
            # do not grant symlink creation rights. The actual junction test below
            # separately verifies an OS-level redirection without needing admin.
            def redirected_resolve(path, *args, **kwargs):
                return destination if path == linked else original_resolve(path, *args, **kwargs)
            with mock.patch.object(Path, 'resolve', redirected_resolve):
                with self.assertRaises(enhanced.Conflict):
                    bridge.refresh(self.root)
                self.assertEqual(destination.read_bytes(), b'preserve this')

    def test_state_directory_junction_or_symlink_is_rejected(self):
        another_game = Path(self.temp.name) / 'another-game'
        another_game.mkdir()
        destination = Path(self.temp.name) / 'redirected-state'
        destination.mkdir()
        link = another_game / 'ConvictionEnhanced-state'
        if os.name == 'nt':
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(destination)],
                                    capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        else:
            link.symlink_to(destination, target_is_directory=True)
        try:
            with self.assertRaises(enhanced.Conflict):
                bridge.refresh(another_game)
            self.assertEqual(list(destination.iterdir()), [])
        finally:
            if os.name == 'nt':
                link.rmdir()
            else:
                link.unlink()


if __name__ == '__main__':
    unittest.main()
