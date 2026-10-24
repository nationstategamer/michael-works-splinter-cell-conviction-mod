"""Build public preview 0.8.3 using a portable-file allowlist.

The sole compiled file is the production x86 ASI. Game binaries, personal state,
vendor repositories, test DLLs, harness binaries and build objects are excluded.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct
import zipfile


ROOT = Path(__file__).resolve().parent
OVERLAY_VERSION = '0.8.3'
DEPENDENCIES = {
    'Dear ImGui': {
        'tag': 'v1.91.9b',
        'commit': 'f5befd2d29e66809cd1110a152e375a7f1981f06',
        'source': 'https://github.com/ocornut/imgui',
        'license': 'native/vendor/imgui/LICENSE.txt',
    },
    'miniz': {
        'tag': '3.0.2',
        'commit': '293d4db1b7d0ffee9756d035b9ac6f7431ef8492',
        'source': 'https://github.com/richgel999/miniz',
        'license': 'native/vendor/miniz/LICENSE',
    },
    'MinHook': {
        'tag': 'v1.3.4',
        'commit': 'c3fcafdc10146beb5919319d0683e44e3c30d537',
        'source': 'https://github.com/TsudaKageyu/minhook',
        'license': 'native/vendor/minhook/LICENSE.txt',
    },
}


def production_exports(data):
    """Reject a wrong-architecture or test-only DLL before bundling it."""
    def unpack(fmt, offset):
        return struct.unpack_from(fmt, data, offset)

    try:
        if data[:2] != b'MZ':
            raise ValueError('not a Windows PE file')
        pe, = unpack('<I', 0x3c)
        if data[pe:pe + 4] != b'PE\0\0':
            raise ValueError('invalid PE header')
        machine, count = unpack('<HH', pe + 4)
        optional_size, characteristics = unpack('<HH', pe + 20)
        optional = pe + 24
        magic, = unpack('<H', optional)
        if machine != 0x14c or magic != 0x10b or not characteristics & 0x2000:
            raise ValueError('production ASI must be an x86 DLL')
        export_rva, export_size = unpack('<II', optional + 96)
        if optional_size < 104 or not export_rva or export_size < 40:
            raise ValueError('missing exports')
        sections = []
        for number in range(count):
            offset = optional + optional_size + number * 40
            _, address, raw_size, raw_offset = unpack('<IIII', offset + 8)
            sections.append((address, raw_size, raw_offset))

        def file_offset(rva, size=1):
            for address, raw_size, raw_offset in sections:
                relative = rva - address
                if 0 <= relative and relative + size <= raw_size:
                    result = raw_offset + relative
                    if result + size <= len(data):
                        return result
            raise ValueError('export points outside the file')

        directory = file_offset(export_rva, 40)
        name_count, = unpack('<I', directory + 24)
        name_table, = unpack('<I', directory + 32)
        if name_count > 256:
            raise ValueError('unexpected export count')
        table = file_offset(name_table, name_count * 4)
        names = set()
        for number in range(name_count):
            rva, = unpack('<I', table + number * 4)
            start = file_offset(rva)
            end = data.find(b'\0', start, start + 256)
            if end < 0:
                raise ValueError('invalid export name')
            names.add(data[start:end].decode('ascii'))
        if 'InitializeASI' not in names or any('CETest' in name for name in names):
            raise ValueError('missing loader export or test-only exports present')
        return sorted(names)
    except (struct.error, UnicodeError) as error:
        raise ValueError('invalid production ASI') from error


def auditor(root=ROOT):
    candidates = (root.parent / 'tools/public_audit.py', root / 'tools/public_audit.py')
    for path in candidates:
        if path.is_file():
            spec = importlib.util.spec_from_file_location('portable_release_auditor', path)
            if spec is not None and spec.loader is not None:
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                return module
    raise ValueError('Public audit helper missing; build from the complete public source checkout')


def binary_identity(data):
    exports = production_exports(data)
    if exports != ['InitializeASI']:
        raise ValueError('The production plugin must export only InitializeASI')
    return dict(schema=1, overlay_version=OVERLAY_VERSION,
                sha256=hashlib.sha256(data).hexdigest(), production_exports=exports)


def write_build_manifest(root=ROOT):
    data = (root / 'native/build/ConvictionEnhanced.asi').read_bytes()
    identity = binary_identity(data)
    findings = auditor(root).content_findings(
        'ConvictionEnhanced/native/build/ConvictionEnhanced.asi', data, binary=True)
    if findings:
        raise ValueError('Production binary failed the public privacy audit')
    target = root / 'BUILD.json'
    temporary = target.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(identity, indent=2) + '\n', encoding='ascii')
    temporary.replace(target)
    print('Reviewed x86 plugin identity written to BUILD.json: ' + identity['sha256'])
    return identity


def release_files(root):
    names = [
        'VERSION', 'LICENSE.txt', 'COPYRIGHT.txt', 'THIRD-PARTY-NOTICES.md', 'README.md', 'DEBUG-MENU.md', 'VALIDATION.md', 'enhanced.py',
        'Manage.cmd', 'compatibility.json', 'package.py', 'controller_layout.py',
        'public_setup.py', 'local_baselines.py', 'Install.cmd', 'Uninstall.cmd', 'Prepare Saves.cmd',
        'save_protection.py', 'overlay_bridge.py', 'overlay_install.py', 'Launch Michael Works Mod.cmd', 'Launch Enhanced.cmd',
        'native/README.md', 'native/branding.h', 'native/version.rc', 'native/overlay.cpp', 'native/overlay.def',
        'native/build.cmd', 'native/harness.cpp', 'native/build/ConvictionEnhanced.asi',
        'native/controller_input.h', 'native/controller_input.cpp',
        'native/controller_state.h', 'native/controller_state_test.cpp',
        'native/free_camera.h', 'native/free_camera.cpp', 'native/free_camera_test.cpp',
        'native/free_camera_math.h', 'native/free_camera_math_test.cpp',
        'native/hud_control.h', 'native/hud_control.cpp', 'native/hud_control_test.cpp',
        'native/gameplay_context.h', 'native/gameplay_context_test.cpp',
        'native/checkpoint_control.h', 'native/checkpoint_control.cpp', 'native/checkpoint_control_test.cpp',
        'native/checkpoint_archive.h', 'native/checkpoint_archive.cpp', 'native/checkpoint_archive_test.cpp',
        'native/checkpoint_library.h', 'native/checkpoint_library.cpp', 'native/checkpoint_library_test.cpp',
        'native/checkpoint_request.h', 'native/checkpoint_request_test.cpp',
        'native/exploration_policy.h', 'native/exploration_policy_test.cpp',
        'native/exploration_control.h', 'native/exploration_control.cpp', 'native/exploration_control_test.cpp',
        'native/exploration_marker_math.h', 'native/exploration_marker_math_test.cpp',
        'native/exploration_landing_math.h', 'native/exploration_landing_math_test.cpp',
        'native/practice_storage.h', 'native/practice_storage.cpp', 'native/practice_storage_test.cpp',
        'native/controller_startup.h', 'native/controller_startup.cpp',
        'native/controller_startup_policy.h', 'native/controller_startup_policy_test.cpp',
        'native/interface_preferences.h', 'native/interface_preferences.cpp', 'native/interface_preferences_test.cpp',
    ]
    names.append('baselines/hashes.json')
    for folder in ('tests',):
        names += [path.relative_to(root).as_posix() for path in (root / folder).glob('*')
                  if path.is_file() and path.suffix in ('.json', '.md', '.py')]
    names += ['native/vendor/imgui/' + name for name in (
        'LICENSE.txt', 'imconfig.h', 'imgui.h', 'imgui_internal.h', 'imgui.cpp',
        'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
        'imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h',
        'backends/imgui_impl_win32.h', 'backends/imgui_impl_win32.cpp',
        'backends/imgui_impl_dx9.h', 'backends/imgui_impl_dx9.cpp',
    )]
    names += ['native/vendor/minhook/' + name for name in (
        'LICENSE.txt', 'AUTHORS.txt', 'include/MinHook.h',
        'src/buffer.c', 'src/buffer.h', 'src/hook.c', 'src/trampoline.c',
        'src/trampoline.h', 'src/hde/hde32.c', 'src/hde/hde32.h',
        'src/hde/hde64.c', 'src/hde/hde64.h', 'src/hde/pstdint.h',
        'src/hde/table32.h', 'src/hde/table64.h',
    )]
    names += ['native/vendor/miniz/' + name for name in (
        'LICENSE', 'PINNED.json', 'miniz.h', 'miniz_common.h', 'miniz_tinfl.h',
        'miniz_tdef.h', 'miniz_zip.h', 'miniz_export.h', 'miniz_tinfl.c',
    )]
    payload = {}
    for name in sorted(set(names)):
        path = root / name
        if path.resolve() != path or not path.is_file():
            raise ValueError('Missing or redirected release file: ' + name)
        payload[name] = path.read_bytes()
    # Repository landing docs use repository-root links. The installed archive
    # keeps them and their linked docs together under ConvictionEnhanced/.
    repository = root.parent
    if (repository / 'README.md').is_file() and (repository / 'ConvictionEnhanced').is_dir():
        payload['README.md'] = (repository / 'README.md').read_bytes()
        for name in ('CONTRIBUTING.md', 'SECURITY.md', 'CHANGELOG.md'):
            path = repository / name
            if path.is_file():
                if path.resolve() != path:
                    raise ValueError('Redirected public repository document')
                payload[name] = path.read_bytes()
        docs = repository / 'docs'
        if docs.is_dir():
            for path in sorted(docs.glob('*.md')):
                if path.resolve() != path:
                    raise ValueError('Redirected public documentation')
                content = path.read_text(encoding='utf-8').replace('](../ConvictionEnhanced/', '](../')
                payload['docs/' + path.name] = content.encode('utf-8')
    helper = next((path for path in (repository / 'tools/public_audit.py', root / 'tools/public_audit.py')
                   if path.is_file()), None)
    if helper is None or helper.resolve() != helper:
        raise ValueError('Missing or redirected public audit helper')
    payload['tools/public_audit.py'] = helper.read_bytes()
    if 'native/README.md' in payload:
        payload['native/README.md'] = payload['native/README.md'].replace(b'](../../docs/', b'](../docs/').replace(
            b'](../../LICENSE.txt)', b'](../LICENSE.txt)')
    identity = binary_identity(payload['native/build/ConvictionEnhanced.asi'])
    payload['BUILD.json'] = (json.dumps(identity, indent=2) + '\n').encode('ascii')
    exports = identity['production_exports']
    metadata = dict(product_name='Michael Works Splinter Cell Conviction Mod',
                    copyright='Copyright (c) 2026 Michael Works. All rights reserved. Original mod work only.',
                    overlay_version=OVERLAY_VERSION,
                    manager_version=payload['VERSION'].decode('ascii').strip(),
                    distribution='public-preview',
                    license='LicenseRef-Michael-Works-Personal-Use',
                    game_baselines_included=False,
                    production_sha256=identity['sha256'],
                    dependencies=DEPENDENCIES, production_exports=exports)
    payload['RELEASE.json'] = (json.dumps(metadata, indent=2) + '\n').encode('utf-8')
    report = auditor(root).audit_payload(
        {'ConvictionEnhanced/' + name: data for name, data in payload.items()}, release=True)
    if not report['ok']:
        details = '; '.join(item['file'] + ': ' + item['reason'] for item in report['findings'])
        raise ValueError('Public release audit refused payload: ' + details)
    return payload


def build(root=ROOT):
    payload = release_files(root)
    manifest = {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())}
    dist = root / 'dist'
    dist.mkdir(exist_ok=True)
    target = dist / ('Michael-Works-Splinter-Cell-Conviction-Mod-' + OVERLAY_VERSION + '.zip')
    temporary = target.with_suffix('.zip.tmp')
    try:
        with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(payload.items()):
                archive.writestr('ConvictionEnhanced/' + name, data)
            archive.writestr('ConvictionEnhanced/PACKAGE-MANIFEST.json',
                             json.dumps(manifest, indent=2) + '\n')
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise ValueError('Archive CRC verification failed')
            for name, sha in manifest.items():
                if hashlib.sha256(archive.read('ConvictionEnhanced/' + name)).hexdigest() != sha:
                    raise ValueError('Archive hash verification failed: ' + name)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    sha = hashlib.sha256(target.read_bytes()).hexdigest()
    (dist / (target.name + '.sha256')).write_text(sha + '  ' + target.name + '\n', encoding='ascii')
    print(str(target))
    print(f'{len(payload) + 1} files; {target.stat().st_size} bytes; integrity verified')
    print('SHA-256: ' + sha)
    return target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest-only', action='store_true',
                        help='Validate the production plugin and write BUILD.json; do not package')
    args = parser.parse_args(argv)
    if args.manifest_only:
        write_build_manifest()
    else:
        build()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
