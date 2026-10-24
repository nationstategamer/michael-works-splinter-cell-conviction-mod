"""Publication boundaries of the standalone builder; no build tools are required."""
from pathlib import Path
import importlib.util
import io
import json
import marshal
import sys
import tempfile
import types
import unittest
import zipfile
from unittest import mock

TOOL = Path(__file__).resolve().parents[2] / 'tools/build_standalone.py'
if TOOL.is_file():
    spec = importlib.util.spec_from_file_location('standalone_build_tests_tool', TOOL)
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)


@unittest.skipUnless(TOOL.is_file(), 'Full source checkout required for build-tool checks')
class StandaloneBuildTests(unittest.TestCase):
    def test_resource_names_cannot_escape_the_bundle(self):
        for name in ('../private.txt', '/absolute.txt', 'C:/private.txt',
                     'native\\plugin.asi', 'double//slash', './relative', ''):
            with self.subTest(name=name):
                self.assertFalse(build.safe_name(name))
        self.assertTrue(build.safe_name('native/build/ConvictionEnhanced.asi'))

    def test_privacy_scans_utf16_and_nested_bytecode(self):
        path = 'C:' + '/' + 'Users' + '/' + 'private-builder/source.py'
        self.assertTrue(build.private_content(path.encode('utf-16le')))
        code = compile('def nested():\n    return ' + repr(path) + '\n', 'safe/module.py', 'exec')
        self.assertTrue(build.code_private(code))
        self.assertTrue(build.code_private(compile('value = (' + repr(path) + ', 1)', 'safe/tuple.py', 'exec')))
        self.assertTrue(build.code_private(compile('pass', path, 'exec')))
        self.assertFalse(build.code_private(compile('value = 1', 'safe/module.py', 'exec')))
        self.assertFalse(build.code_private(compile('value = "\\ud800"', 'safe/unicode.py', 'exec')))

    def test_dependency_pins_need_unique_exact_versions_and_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'requirements.txt'
            good = 'pyinstaller==6.22.3 --hash=sha256:' + 'a' * 64 + '\n'
            path.write_text(good)
            self.assertEqual(build.build_pins(path), {'pyinstaller': '6.22.3'})
            for invalid in ('pyinstaller>=6\n', good + good,
                            'another==1.0 --hash=sha256:' + 'b' * 64 + '\n'):
                path.write_text(invalid)
                with self.assertRaises(ValueError):
                    build.build_pins(path)

    def audit(self, entries, files, pyz=None):
        class Reader:
            def __init__(self, _):
                self.toc = {name: (0, len(data), len(data), 0, kind)
                            for name, (kind, data) in entries.items()}

            def extract(self, name):
                return entries[name][1]

            def open_embedded_archive(self, _):
                class Embedded:
                    toc = list(pyz or {})

                    def extract(self, name):
                        return pyz[name]
                return Embedded()

        modules = {name: types.ModuleType(name) for name in (
            'PyInstaller', 'PyInstaller.archive', 'PyInstaller.archive.readers')}
        modules['PyInstaller.archive.readers'].CArchiveReader = Reader
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'fixture.exe'
            executable.write_bytes(b'owned-fixture-container')
            with mock.patch.dict(sys.modules, modules):
                return build.archive_audit(executable, {'files': files})

    def test_reviewed_resources_pass_and_modified_resources_fail(self):
        entries = {build.MANIFEST: ('x', b'{}'), 'LICENSE.txt': ('x', b'owned-license')}
        files = {'LICENSE.txt': build.sha256(b'owned-license')}
        self.assertTrue(self.audit(entries, files)['ok'])
        entries['LICENSE.txt'] = ('x', b'changed-after-review')
        self.assertFalse(self.audit(entries, files)['ok'])

    def test_unlisted_game_configuration_and_test_outputs_are_rejected(self):
        for name in ('game/config.ini', 'test-work-extra/innocent.txt', 'practice-saves/data.json'):
            entries = {build.MANIFEST: ('x', b'{}'), name: ('x', b'private-fixture')}
            with self.subTest(name=name):
                self.assertFalse(self.audit(entries, {})['ok'])
                memory = io.BytesIO()
                with zipfile.ZipFile(memory, 'w') as archive:
                    archive.writestr(name, b'private-fixture')
                self.assertTrue(build.zip_findings(memory.getvalue(), 'runtime.zip'))

    def test_pyz_and_main_script_private_paths_are_rejected(self):
        path = 'C:' + '/' + 'Users' + '/' + 'private-builder/entry.py'
        code = compile('pass', path, 'exec')
        entries = {build.MANIFEST: ('x', b'{}'), 'entry': ('s', marshal.dumps(code))}
        self.assertFalse(self.audit(entries, {})['ok'])
        entries = {build.MANIFEST: ('x', b'{}'), 'PYZ.pyz': ('z', b'owned-pyz-fixture')}
        self.assertFalse(self.audit(entries, {}, {'hidden_module': code})['ok'])

    def test_missing_manifest_or_reviewed_resource_is_rejected(self):
        self.assertFalse(self.audit({}, {})['ok'])
        entries = {build.MANIFEST: ('x', b'{}')}
        self.assertFalse(self.audit(entries, {'LICENSE.txt': 'a' * 64})['ok'])

    def test_compressed_bytecode_is_inspected_for_private_paths(self):
        path = 'C:' + '/' + 'Users' + '/' + 'private-builder/module.py'
        code = compile('pass', path, 'exec')
        memory = io.BytesIO()
        with zipfile.ZipFile(memory, 'w', zipfile.ZIP_DEFLATED) as archive:
            archive.writestr('module.pyc', b'0' * 16 + marshal.dumps(code))
        self.assertTrue(build.zip_findings(memory.getvalue(), 'base_library.zip'))

    def test_duplicate_compressed_members_are_rejected(self):
        memory = io.BytesIO()
        with zipfile.ZipFile(memory, 'w') as archive:
            archive.writestr('same.txt', b'first')
            with self.assertWarns(UserWarning):
                archive.writestr('same.txt', b'second')
        self.assertTrue(build.zip_findings(memory.getvalue(), 'runtime.zip'))

    def test_exact_tcltk_fallback_uses_separate_verified_upstream_notices(self):
        tcl = build.fallback_tcltk_license('tcl', '8.6.15')
        tk = build.fallback_tcltk_license('tk', '8.6.15')
        self.assertNotEqual(tcl, tk)
        self.assertNotIn(b'Apple Inc.', tcl)
        self.assertIn(b'Apple Inc.', tk)
        for component, version in (('tcl', '9.0.4'), ('tk', '8.6.14'), ('unknown', '8.6.15')):
            with self.assertRaises(ValueError):
                build.fallback_tcltk_license(component, version)
        import _tkinter
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / 'DLLs').mkdir()
            (base / 'DLLs/tk86t.dll').write_bytes(b'owned-version-resource-fixture')
            with mock.patch.object(_tkinter, 'TK_VERSION', '8.6'), \
                    mock.patch.object(build, 'windows_file_version', return_value='8.6.15'):
                self.assertEqual(build.runtime_component_version('tk', base), '8.6.15')
            with mock.patch.object(_tkinter, 'TK_VERSION', '9.0'), \
                    mock.patch.object(build, 'windows_file_version', return_value='8.6.15'):
                with self.assertRaises(ValueError):
                    build.runtime_component_version('tk', base)

    def test_tcltk_fallback_rejects_tampered_or_redirected_catalog_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve()
            content = b'owned-license-fixture'
            (path / 'notice.txt').write_bytes(content)
            entry = {'file': 'notice.txt', 'sha256': build.sha256(content)}
            catalog = {'schema': 1, 'licenses': {'tcl': {'8.6.15': entry}}}
            pin = path / 'TCLTK-PINNED.json'
            pin.write_text(json.dumps(catalog))
            self.assertEqual(build.fallback_tcltk_license('tcl', '8.6.15', path), content)
            (path / 'notice.txt').write_bytes(b'tampered')
            with self.assertRaisesRegex(ValueError, 'integrity'):
                build.fallback_tcltk_license('tcl', '8.6.15', path)
            entry['file'] = '../outside.txt'
            pin.write_text(json.dumps(catalog))
            with self.assertRaises(ValueError):
                build.fallback_tcltk_license('tcl', '8.6.15', path)


if __name__ == '__main__':
    unittest.main()
