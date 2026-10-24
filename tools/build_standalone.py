"""Build and inspect the self-contained Windows x64 mod installer/manager.

The embedded plugin remains x86. No game installation, saves, extracted baselines,
external prerequisite DLLs or developer test files are read by this builder.
Install the hash-pinned standalone-requirements.txt in a private build environment.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import importlib.util
import io
import json
import marshal
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys
import tempfile
import types
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'ConvictionEnhanced'
MANIFEST = 'STANDALONE-PAYLOAD.json'
RESOURCE_NAMES = (
    'VERSION', 'LICENSE.txt', 'COPYRIGHT.txt', 'THIRD-PARTY-NOTICES.md',
    'DEBUG-MENU.md', 'VALIDATION.md', 'compatibility.json', 'BUILD.json',
    'baselines/hashes.json', 'native/build/ConvictionEnhanced.asi',
    'native/vendor/imgui/LICENSE.txt', 'native/vendor/minhook/LICENSE.txt',
    'native/vendor/minhook/AUTHORS.txt', 'native/vendor/miniz/LICENSE',
    'native/vendor/miniz/PINNED.json',
)
PRIVATE_COMPONENTS = {
    'convictionenhanced-state', 'save-backups', 'practice-saves',
    'checkpoint-library', 'analysis', 'analysis-deps', 'unpacked', '.git',
    '__pycache__', '_incomplete', '_conflicts', 'site-packages',
}
PRIVATE_PATHS = (
    re.compile(r'(?i)\b[A-Z]:[\\/]+Users[\\/]+[^\\/\s<>\x00]+'),
    re.compile(r'(?i)[\\/]\.codex[\\/]+visualizations[\\/]'),
    re.compile(r'(?i)/' + 'Users' + r'/[^/\s<>\x00]+'),
)
SECRETS = (
    re.compile(r'gh[opusr]_[A-Za-z0-9]{30,}'),
    re.compile(r'github_pat_[A-Za-z0-9_]{40,}'),
    re.compile(r'AKIA[0-9A-Z]{16}'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
)


def load_package():
    spec = importlib.util.spec_from_file_location('standalone_release_package', PACKAGE / 'package.py')
    if spec is None or spec.loader is None:
        raise ValueError('Missing public package builder')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_name(name: str) -> bool:
    path = PurePosixPath(name)
    return bool(name and '\\' not in name and ':' not in name
                and not path.is_absolute() and '..' not in path.parts
                and all(part not in {'', '.'} for part in name.split('/')))


def private_content(data: bytes) -> bool:
    views = (data.decode('utf-8', errors='ignore'),
             data.decode('utf-16le', errors='ignore'),
             data[1:].decode('utf-16le', errors='ignore'))
    return any(pattern.search(text) for text in views for pattern in (*PRIVATE_PATHS, *SECRETS))


def code_private(code: types.CodeType) -> bool:
    """Inspect bytecode paths AND string constants, including nested functions."""
    if private_content(code.co_filename.encode('utf-8', errors='surrogatepass')):
        return True
    def constant_private(value):
        if isinstance(value, types.CodeType):
            return code_private(value)
        if isinstance(value, str):
            return private_content(value.encode('utf-8', errors='surrogatepass'))
        if isinstance(value, bytes):
            return private_content(value)
        if isinstance(value, (tuple, frozenset)):
            return any(constant_private(item) for item in value)
        return False
    return any(constant_private(value) for value in code.co_consts)


def zip_findings(data: bytes, label: str) -> list[dict]:
    """Inspect compressed standard-library/Tcl resources, including .pyc paths."""
    findings = []
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if (len(entries) > 4096 or len({entry.filename for entry in entries}) != len(entries)
                or sum(entry.file_size for entry in entries) > 64 * 1024 * 1024
                or any(entry.file_size > 16 * 1024 * 1024 for entry in entries)):
            return [{'file': label, 'reason': 'duplicate or oversized embedded ZIP'}]
        for entry in entries:
            name = entry.filename.rstrip('/')
            parts = tuple(part.casefold() for part in PurePosixPath(name).parts)
            if (not safe_name(name)
                    or any(part in PRIVATE_COMPONENTS or part.startswith('test-work') for part in parts)
                    or PurePosixPath(name).suffix.casefold() in {'.save', '.upload', '.umd', '.ums', '.upk', '.ini', '.pdb', '.log'}):
                findings.append({'file': label, 'reason': 'unsafe path in embedded ZIP'})
                continue
            content = archive.read(entry)
            if private_content(content):
                findings.append({'file': label + '/' + name, 'reason': 'private content in embedded ZIP'})
            if name.endswith('.pyc'):
                try:
                    code = marshal.loads(content[16:])
                except (ValueError, EOFError, TypeError):
                    code = None
                if not isinstance(code, types.CodeType) or code_private(code):
                    findings.append({'file': label + '/' + name, 'reason': 'private or invalid embedded bytecode'})
    return findings


def build_pins(path: Path = ROOT / 'tools/standalone-requirements.txt') -> dict[str, str]:
    pins = {}
    for line in path.read_text('utf-8').splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        match = re.fullmatch(r'([a-z0-9-]+)==([A-Za-z0-9.]+) --hash=sha256:[a-f0-9]{64}', line)
        if match is None or match[1] in pins:
            raise ValueError('Malformed or duplicate standalone dependency pin')
        pins[match[1]] = match[2]
    if 'pyinstaller' not in pins:
        raise ValueError('PyInstaller is not pinned')
    return pins


def require_build_environment() -> dict[str, str]:
    if os.name != 'nt' or struct.calcsize('P') != 8:
        raise ValueError('Build the standalone manager with 64-bit Python on Windows')
    pins = build_pins()
    for name, version in pins.items():
        if importlib.metadata.version(name) != version:
            raise ValueError('Use the exact hash-pinned standalone build environment: ' + name)
    return pins


def plain_bytes(path: Path) -> bytes:
    if not path.is_file() or path.resolve() != path.absolute():
        raise ValueError('Required public resource is missing or redirected: ' + path.name)
    data = path.read_bytes()
    if len(data) > 16 * 1024 * 1024 or private_content(data):
        raise ValueError('Public resource failed size/privacy checks: ' + path.name)
    return data


def windows_file_version(path: Path) -> str:
    """Read Tk's actual FileVersion string without loading Tk or opening a window.

    Tcl/Tk fixed numeric resources encode release level and serial in their final
    two fields, so treating the third numeric field as a patch version is wrong.
    """
    import ctypes
    from ctypes import wintypes
    library = ctypes.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32/version.dll'),
                            use_last_error=True)
    library.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    library.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    library.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                           wintypes.DWORD, ctypes.c_void_p]
    library.GetFileVersionInfoW.restype = wintypes.BOOL
    library.VerQueryValueW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                      ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    library.VerQueryValueW.restype = wintypes.BOOL
    size = library.GetFileVersionInfoSizeW(str(path), None)
    if not size:
        raise ValueError('The Tk runtime DLL has no readable version resource')
    buffer = ctypes.create_string_buffer(size)
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    if (not library.GetFileVersionInfoW(str(path), 0, size, buffer)
            or not library.VerQueryValueW(buffer, '\\VarFileInfo\\Translation',
                                           ctypes.byref(pointer), ctypes.byref(length))
            or length.value < 4 or length.value % 4):
        raise ValueError('Cannot inspect the Tk runtime DLL version')
    translations = ctypes.cast(pointer, ctypes.POINTER(wintypes.WORD))
    pairs = [(translations[index], translations[index + 1])
             for index in range(0, length.value // 2, 2)]
    for language, page in pairs:
        key = f'\\StringFileInfo\\{language:04x}{page:04x}\\FileVersion'
        if library.VerQueryValueW(buffer, key, ctypes.byref(pointer), ctypes.byref(length)):
            value = ctypes.wstring_at(pointer, length.value).rstrip('\x00').strip()
            if re.fullmatch(r'\d+\.\d+\.\d+', value):
                return value
    raise ValueError('The Tk runtime DLL has no exact release version string')


def runtime_component_version(component: str, base: Path) -> str:
    import _tkinter
    if component == 'tcl':
        import tkinter
        # Tcl() does not load Tk, create a window or require an interactive desktop.
        version = str(tkinter.Tcl().call('info', 'patchlevel'))
        family = _tkinter.TCL_VERSION
    elif component == 'tk':
        candidates = [path for path in (base / 'DLLs').glob('*.dll')
                      if re.fullmatch(r'(?:tk\d+t?|tcl\d+tk\d+)\.dll', path.name, re.IGNORECASE)]
        if len(candidates) != 1:
            raise ValueError('Cannot identify the exact Tk runtime DLL')
        version = windows_file_version(candidates[0])
        family = _tkinter.TK_VERSION
    else:
        raise ValueError('Unknown Tcl/Tk runtime component')
    if not re.fullmatch(r'\d+\.\d+\.\d+', version) or not version.startswith(family + '.'):
        raise ValueError('The Tcl/Tk runtime version does not match its Python binding')
    return version


def fallback_tcltk_license(component: str, version: str,
                          directory: Path = ROOT / 'tools/runtime-licenses') -> bytes:
    catalog = json.loads((directory / 'TCLTK-PINNED.json').read_text('utf-8'))
    if catalog.get('schema') != 1 or component not in {'tcl', 'tk'}:
        raise ValueError('Malformed Tcl/Tk license catalog')
    entry = catalog.get('licenses', {}).get(component, {}).get(version)
    if (not isinstance(entry, dict) or not isinstance(entry.get('file'), str)
            or '/' in entry['file'] or '\\' in entry['file'] or not safe_name(entry['file'])
            or re.fullmatch(r'[a-f0-9]{64}', entry.get('sha256', '')) is None):
        raise ValueError('No matching pinned ' + component + ' license for runtime ' + version)
    data = plain_bytes(directory / entry['file'])
    if sha256(data) != entry['sha256']:
        raise ValueError('Pinned ' + component + ' license integrity check failed')
    return data


def runtime_licenses() -> dict[str, bytes]:
    base = Path(sys.base_prefix)
    result = {'Python-LICENSE.txt': plain_bytes(base / 'LICENSE.txt')}
    versions = {}
    # CPython 3.15 packages Tcl/Tk 9 scripts and notices in ZIPs. Older supported
    # runtimes use unpacked Tcl/Tk 8 directories; retain the exact local notices.
    for component in ('tcl', 'tk'):
        versions[component] = runtime_component_version(component, base)
        data = None
        for archive in sorted((base / 'tcl').glob('lib' + component + '*.zip')):
            with zipfile.ZipFile(archive) as source:
                name = component + '_library/license.terms'
                if name in source.namelist():
                    data = source.read(name)
                    break
        if data is None:
            candidates = sorted((base / 'tcl').glob(component + '*/license.terms'))
            if not candidates:
                candidates = sorted((base / 'tcl').glob(component + '*/demos/license.terms'))
            if candidates:
                data = plain_bytes(candidates[0])
        if not data:
            # Some official Windows Python installers omit Tcl/Tk license.terms.
            # Use only an independently pinned notice for this exact component
            # patch version; never substitute a Tcl 9 notice for a Tcl 8 runtime.
            data = fallback_tcltk_license(component, versions[component])
        result[component.title() + '-LICENSE.txt'] = data
    installer = importlib.metadata.distribution('pyinstaller')
    copying = [path for path in installer.files or () if path.name == 'COPYING.txt']
    if len(copying) != 1:
        raise ValueError('PyInstaller license notice missing')
    result['PyInstaller-COPYING.txt'] = plain_bytes(Path(installer.locate_file(copying[0])))
    openssl = ROOT / 'tools/runtime-licenses/OpenSSL-LICENSE.txt'
    pinned = json.loads((openssl.parent / 'PINNED.json').read_text('utf-8'))
    result['OpenSSL-LICENSE.txt'] = plain_bytes(openssl)
    if sha256(result['OpenSSL-LICENSE.txt']) != pinned['sha256']:
        raise ValueError('Pinned OpenSSL license notice changed')
    import ssl
    notice = ('Standalone runtime notices\n\n'
              'CPython ' + sys.version.split()[0] + ' (Windows x64)\n'
              'Python Software Foundation and other credited contributors.\n'
              'https://www.python.org/\n\n'
              'Tcl ' + versions['tcl'] + '; Tk ' + versions['tk'] + '\n'
              'https://www.tcl-lang.org/\n\n'
              + ssl.OPENSSL_VERSION + '\n'
              'Copyright The OpenSSL Project Authors. Apache License 2.0.\n'
              'https://www.openssl.org/\n\n'
              'PyInstaller ' + installer.version + '\n'
              'GPL-2.0 with the bootloader exception; see COPYING text.\n'
              'https://pyinstaller.org/en/stable/license.html\n\n'
              'These components keep their own licenses. Michael Works personal-use\n'
              'terms cover only original mod code and documentation.\n')
    result['NOTICE.txt'] = notice.encode('utf-8')
    combined = [notice]
    for name, data in sorted(result.items()):
        if name != 'NOTICE.txt':
            combined += ['\n\n===== ' + name + ' =====\n\n', data.decode('utf-8')]
    result['ALL-NOTICES.txt'] = ''.join(combined).encode('utf-8')
    return {'runtime-licenses/' + name: data for name, data in result.items()}


def resource_payload() -> tuple[dict[str, bytes], dict]:
    package = load_package()
    version = package.DISTRIBUTION_VERSION
    if re.fullmatch(r'\d+\.\d+\.\d+', version) is None:
        raise ValueError('Malformed public distribution version')
    payload = {name: plain_bytes(PACKAGE / name) for name in RESOURCE_NAMES}
    payload['README.md'] = plain_bytes(ROOT / 'README.md')
    for path in sorted((ROOT / 'docs').glob('*.md')):
        payload['docs/' + path.name] = plain_bytes(path)
    payload.update(runtime_licenses())
    native = payload['native/build/ConvictionEnhanced.asi']
    identity = package.binary_identity(native)
    if json.loads(payload['BUILD.json']) != identity:
        raise ValueError('Native plugin does not match reviewed BUILD.json')
    if any(not safe_name(name) for name in payload):
        raise ValueError('Unsafe standalone resource name')
    # Reuse the trusted game-content catalog, allowing only this reviewed native
    # binary. Runtime libraries are collected later by PyInstaller, not from here.
    report = package.auditor().audit_payload(
        {'ConvictionEnhanced/' + name: data for name, data in payload.items()}, release=True)
    if not report['ok']:
        raise ValueError('Standalone resources failed public content audit')
    manifest = dict(schema=1, distribution_version=version, native_sha256=identity['sha256'],
                    files={name: sha256(data) for name, data in sorted(payload.items())})
    payload[MANIFEST] = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    return payload, manifest


def archive_audit(executable: Path, manifest: dict) -> dict:
    from PyInstaller.archive.readers import CArchiveReader
    archive = CArchiveReader(str(executable))
    private = []
    resources = 0
    modules = 0
    names = set(archive.toc)
    for name, entry in archive.toc.items():
        normalized = name.replace('\\', '/')
        parts = tuple(p.casefold() for p in PurePosixPath(normalized).parts)
        if (not safe_name(normalized) or any(p in PRIVATE_COMPONENTS or p.startswith('test-work') for p in parts)
                or any(normalized.casefold().endswith(suffix) for suffix in ('.save', '.upload', '.umd', '.ums', '.upk', '.ini', '.pdb', '.log'))):
            private.append({'file': normalized, 'reason': 'forbidden embedded path/type'})
            continue
        if (PurePosixPath(normalized).name.casefold() in {
                'version.dll', 'dinput8.dll', 'xidi.32.dll', 'overlay_test.dll', 'conviction_game.exe'}
                or normalized.casefold().endswith('.asi') and normalized != 'native/build/ConvictionEnhanced.asi'):
            private.append({'file': normalized, 'reason': 'game, prerequisite or test binary'})
            continue
        data = archive.extract(name)
        if normalized in manifest['files']:
            if sha256(data) != manifest['files'][normalized]:
                private.append({'file': normalized, 'reason': 'resource differs from manifest'})
            resources += 1
        if private_content(data):
            private.append({'file': normalized, 'reason': 'private path or credential-like content'})
        if normalized.casefold().endswith('.zip'):
            private.extend(zip_findings(data, normalized))
        typecode = entry[-1]
        if typecode == 's':
            code = marshal.loads(data)
            modules += 1
            if not isinstance(code, types.CodeType) or code_private(code):
                private.append({'file': normalized, 'reason': 'private code path or constant'})
        elif typecode == 'z':
            pyz = archive.open_embedded_archive(name)
            for module in pyz.toc:
                code = pyz.extract(module)
                if isinstance(code, types.CodeType):
                    modules += 1
                    if code_private(code):
                        private.append({'file': module, 'reason': 'private Python code path or constant'})
    if resources != len(manifest['files']) or MANIFEST not in names:
        private.append({'file': '.', 'reason': 'missing reviewed resources or manifest'})
    if private_content(executable.read_bytes()):
        private.append({'file': executable.name, 'reason': 'private path in executable container'})
    return dict(ok=not private, embedded_entries=len(names), payload_files=resources,
                python_modules=modules, findings=private)


def version_resource(version: str) -> str:
    numbers = tuple(int(value) for value in version.split('.')) + (0,)
    return ("VSVersionInfo(ffi=FixedFileInfo(filevers=" + repr(numbers)
            + ", prodvers=" + repr(numbers)
            + ", mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)), "
            "kids=[StringFileInfo([StringTable('040904B0', ["
            "StringStruct('CompanyName', 'Michael Works'), "
            "StringStruct('FileDescription', 'Michael Works Conviction Mod Setup and Manager'), "
            "StringStruct('FileVersion', '" + version + "'), "
            "StringStruct('ProductName', 'Michael Works Splinter Cell Conviction Mod'), "
            "StringStruct('ProductVersion', '" + version + "'), "
            "StringStruct('LegalCopyright', 'Copyright (c) 2026 Michael Works. Original mod work only.')"
            "])]), VarFileInfo([VarStruct('Translation', [1033, 1200])])])\n")


def build(output: Path, work: Path) -> Path:
    pins = require_build_environment()
    payload, manifest = resource_payload()
    version = manifest['distribution_version']
    name = 'Michael-Works-Splinter-Cell-Conviction-Mod-Setup-' + version
    resources = work / 'resources'
    resources.mkdir(parents=True, exist_ok=False)
    for relative, data in payload.items():
        path = resources / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    version_file = work / 'version.txt'
    version_file.write_text(version_resource(version), 'utf-8')
    output.mkdir(parents=True, exist_ok=True)
    args = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile',
            '--log-level', 'WARN',
            '--windowed', '--noupx', '--name', name, '--distpath', str(output),
            '--workpath', str(work / 'pyinstaller'), '--specpath', str(work),
            '--paths', str(PACKAGE), '--version-file', str(version_file),
            '--hidden-import', 'package', '--hidden-import', 'standalone_manager',
            '--hidden-import', 'external_launch']
    for relative in sorted(payload):
        parent = PurePosixPath(relative).parent.as_posix()
        args += ['--add-data', str(resources / relative) + os.pathsep + parent]
    args.append(str(PACKAGE / 'standalone.py'))
    environment = dict(os.environ, PYINSTALLER_CONFIG_DIR=str(work / 'cache'))
    subprocess.run(args, cwd=ROOT, env=environment, check=True)
    executable = output / (name + '.exe')
    report = archive_audit(executable, manifest)
    if not report['ok']:
        print(json.dumps(report, indent=2))
        raise ValueError('Standalone executable failed embedded privacy/integrity audit')
    digest = sha256(executable.read_bytes())
    identity = dict(schema=1, distribution_version=version, architecture='windows-x64',
                    native_architecture='x86', native_sha256=manifest['native_sha256'],
                    python_version=sys.version.split()[0], build_dependencies=pins,
                    filename=executable.name, bytes=executable.stat().st_size, sha256=digest,
                    embedded_audit=report)
    executable.with_suffix('.exe.sha256').write_text(digest + '  ' + executable.name + '\n', 'ascii')
    executable.with_suffix('.exe.json').write_text(json.dumps(identity, indent=2) + '\n', 'utf-8')
    print(json.dumps(identity, indent=2))
    return executable


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, default=PACKAGE / 'dist')
    parser.add_argument('--work-dir', type=Path,
                        help='A new private build directory; defaults to an OS temporary directory.')
    args = parser.parse_args(argv)
    try:
        if args.work_dir:
            work = args.work_dir.absolute()
            if work.exists():
                raise ValueError('Choose a new build work directory')
            work.mkdir(parents=True)
            build(args.output_dir.absolute(), work)
        else:
            with tempfile.TemporaryDirectory(prefix='mw-mod-standalone-build-') as directory:
                build(args.output_dir.absolute(), Path(directory))
        return 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError,
            importlib.metadata.PackageNotFoundError) as error:
        print('Standalone build stopped: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
