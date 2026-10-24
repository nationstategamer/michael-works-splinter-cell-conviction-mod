"""External-launch state tests never start Steam or access game/save files."""
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import external_launch


class ExternalLaunchTests(unittest.TestCase):
    def test_only_bundle_path_entries_are_removed(self):
        bundle = r'D:\owned-fixture\bundle'
        original = (r'C:\Windows\System32;D:\owned-fixture\bundle;'
                    r'"D:\owned-fixture\BUNDLE\bin";D:\owned-fixture\bundle-other;'
                    r'D:\Game\bin;;%KEEP_ORIGINAL_TEXT%')
        self.assertEqual(external_launch._without_bundle_paths(original, bundle),
                         r'C:\Windows\System32;D:\owned-fixture\bundle-other;D:\Game\bin;;%KEEP_ORIGINAL_TEXT%')

    def test_source_launch_requests_a_normal_window_without_dll_changes(self):
        with mock.patch.object(external_launch, '_bundle_root', return_value=None), \
                mock.patch.object(external_launch, '_set_dll_directory') as dll, \
                mock.patch.object(external_launch.os, 'startfile', create=True) as start:
            external_launch.request_steam_launch()
        start.assert_called_once_with(external_launch.STEAM_URI, show_cmd=1)
        dll.assert_not_called()

    def test_frozen_launch_cleans_and_restores_state(self):
        bundle = r'D:\owned-fixture\bundle'
        original = r'D:\owned-fixture\bundle;C:\Windows\System32'
        calls = []
        def launched(uri, *, show_cmd):
            calls.append((uri, show_cmd, os.environ['PATH']))
            self.assertEqual(dll.call_args, mock.call(None))
        with mock.patch.object(external_launch, '_bundle_root', return_value=bundle), \
                mock.patch.object(external_launch, 'current_dll_directory', return_value=bundle), \
                mock.patch.object(external_launch, '_set_dll_directory') as dll, \
                mock.patch.dict(os.environ, {'PATH': original}), \
                mock.patch.object(external_launch.os, 'startfile', side_effect=launched, create=True):
            external_launch.request_steam_launch()
            self.assertEqual(os.environ['PATH'], original)
        self.assertEqual(calls, [(external_launch.STEAM_URI, 1, r'C:\Windows\System32')])
        self.assertEqual(dll.call_args_list, [mock.call(None), mock.call(bundle)])

    def test_external_failure_restores_dll_directory_and_path(self):
        bundle = r'D:\owned-fixture\bundle'
        original = bundle + r';C:\Windows'
        with mock.patch.object(external_launch, '_bundle_root', return_value=bundle), \
                mock.patch.object(external_launch, 'current_dll_directory', return_value=bundle), \
                mock.patch.object(external_launch, '_set_dll_directory') as dll, \
                mock.patch.dict(os.environ, {'PATH': original}), \
                mock.patch.object(external_launch.os, 'startfile', side_effect=OSError('fixture refusal'), create=True):
            with self.assertRaisesRegex(OSError, 'fixture refusal'):
                external_launch.request_steam_launch()
            self.assertEqual(os.environ['PATH'], original)
        self.assertEqual(dll.call_args_list, [mock.call(None), mock.call(bundle)])

    def test_failed_sanitization_never_requests_steam(self):
        with mock.patch.object(external_launch, '_bundle_root', return_value=r'D:\owned-fixture\bundle'), \
                mock.patch.object(external_launch, 'current_dll_directory', return_value=r'D:\owned-fixture\bundle'), \
                mock.patch.object(external_launch, '_set_dll_directory', side_effect=external_launch.LaunchEnvironmentError('refused')), \
                mock.patch.object(external_launch.os, 'startfile', create=True) as start:
            with self.assertRaises(external_launch.LaunchEnvironmentError):
                external_launch.request_steam_launch()
        start.assert_not_called()

    def test_failed_inspection_never_requests_steam(self):
        with mock.patch.object(external_launch, '_bundle_root', return_value=r'D:\owned-fixture\bundle'), \
                mock.patch.object(external_launch, 'current_dll_directory', side_effect=external_launch.LaunchEnvironmentError('refused')), \
                mock.patch.object(external_launch.os, 'startfile', create=True) as start:
            with self.assertRaises(external_launch.LaunchEnvironmentError):
                external_launch.request_steam_launch()
        start.assert_not_called()

    def test_absent_path_stays_absent_even_after_callback_failure(self):
        with mock.patch.object(external_launch, '_bundle_root', return_value=r'D:\owned-fixture\bundle'), \
                mock.patch.object(external_launch, 'current_dll_directory', return_value=None), \
                mock.patch.object(external_launch, '_set_dll_directory'), \
                mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(ValueError, 'fixture'):
                with external_launch.clean_external_environment():
                    self.assertNotIn('PATH', os.environ)
                    os.environ['PATH'] = 'temporary callback value'
                    raise ValueError('fixture')
            self.assertNotIn('PATH', os.environ)

    @unittest.skipUnless(os.name == 'nt', 'Windows DLL search state')
    def test_real_windows_state_is_clean_during_scope_and_restored_after_error(self):
        original = external_launch.current_dll_directory()
        with tempfile.TemporaryDirectory(prefix='mw-external-launch-') as directory:
            try:
                external_launch._set_dll_directory(directory)
                self.assertEqual(external_launch.current_dll_directory(), directory)
                with mock.patch.object(external_launch, '_bundle_root', return_value=directory), \
                        mock.patch.dict(os.environ, {'PATH': directory + ';' + os.environ.get('PATH', '')}):
                    path = os.environ['PATH']
                    with self.assertRaisesRegex(ValueError, 'fixture'):
                        with external_launch.clean_external_environment():
                            self.assertIsNone(external_launch.current_dll_directory())
                            self.assertFalse(os.environ['PATH'].startswith(directory + ';'))
                            raise ValueError('fixture')
                    self.assertEqual(os.environ['PATH'], path)
                    self.assertEqual(external_launch.current_dll_directory(), directory)
            finally:
                external_launch._set_dll_directory(original)


if __name__ == '__main__':
    unittest.main()
