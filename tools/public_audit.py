"""Audit the files selected for public source or a portable release archive.

Findings name files and categories, never the credential or private path found.
This is a focused release check, not a claim of exhaustive secret detection.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PRODUCTION_BINARY = 'ConvictionEnhanced/native/build/ConvictionEnhanced.asi'
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_TOTAL_BYTES = 64 * 1024 * 1024
GENERATED = {'build', 'dist', 'test-work', '__pycache__', '.git', '.vs'}
PRIVATE = {
    'convictionenhanced-state', 'save-backups', 'practice-saves',
    'checkpoint-library', 'analysis', 'analysis-deps', 'unpacked',
    '_incomplete', '_conflicts', 'validation',
}
SOURCE_BINARIES = {'.asi', '.dll', '.exe', '.obj', '.lib', '.exp', '.res',
                   '.pdb', '.ilk', '.zip', '.bmp', '.dmp', '.log', '.tmp', '.bak'}
GAME_FILES = {'.save', '.upload', '.umd', '.ums', '.upk', '.u', '.ini'}
SECRET_PATTERNS = (
    re.compile(r'gh[opusr]_[A-Za-z0-9]{30,}'),
    re.compile(r'github_pat_[A-Za-z0-9_]{40,}'),
    re.compile(r'AKIA[0-9A-Z]{16}'),
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
)
PRIVATE_PATH_PATTERNS = (
    re.compile(r'(?i)\b[A-Z]:[\\/]+Users[\\/]+[^\\/\s<>\x00]+'),
    re.compile(r'(?i)/' + 'Users' + r'/[^/\s<>\x00]+'),
    re.compile(r'(?i)[\\/]\.codex[\\/]+visualizations[\\/]'),
)
BUILD_PATH = re.compile(r'(?i)\b[A-Z]:[\\/][^\x00\r\n]{0,512}?\.(?:cpp|c|h)\b')


def known_game_hashes() -> set[str]:
    """Recognize complete pinned baselines and Fusion assets even if renamed."""
    hashes = set()
    for package in (ROOT / 'ConvictionEnhanced', ROOT):
        for relative, asset_key in (('baselines/hashes.json', None),
                                    ('compatibility.json', 'blacklist_assets')):
            path = package / relative
            if not path.is_file():
                continue
            catalog = json.loads(path.read_text(encoding='utf-8'))
            if asset_key is not None and isinstance(catalog, dict):
                catalog = catalog.get(asset_key)
            if (not isinstance(catalog, dict) or not catalog
                    or any(not isinstance(value, str) or re.fullmatch('[a-f0-9]{64}', value) is None
                           for value in catalog.values())):
                raise ValueError('Invalid trusted game hash catalog')
            hashes.update(catalog.values())
    return hashes


def generated_part(part: str) -> bool:
    name = part.casefold()
    return name in GENERATED or name.startswith('test-work')


def text_views(data: bytes):
    yield data.decode('utf-8', errors='ignore')
    # Compiler assertions and Windows resource strings may use either alignment.
    for offset in (0, 1):
        yield data[offset:].decode('utf-16le', errors='ignore')


def content_findings(name: str, data: bytes, binary: bool = False) -> list[dict]:
    categories = set()
    for text in text_views(data):
        if any(pattern.search(text) for pattern in SECRET_PATTERNS):
            categories.add('credential-like content')
        if any(pattern.search(text) for pattern in PRIVATE_PATH_PATTERNS):
            categories.add('private user/build path')
        if binary and BUILD_PATH.search(text):
            categories.add('absolute source path in production binary')
    return [{'file': name, 'reason': reason} for reason in sorted(categories)]


def name_findings(name: str, release: bool = False) -> list[dict]:
    findings = []
    path = PurePosixPath(name)
    parts = tuple(part.casefold() for part in path.parts)
    if ('\\' in name or path.is_absolute() or '..' in path.parts
            or not path.parts or ':' in name):
        return [{'file': name, 'reason': 'unsafe public path'}]
    if any(part.startswith('test-work') for part in parts):
        findings.append({'file': name, 'reason': 'generated test output'})
    if any(part in PRIVATE for part in parts):
        findings.append({'file': name, 'reason': 'private state, dump or diagnostic file'})
    if path.suffix.casefold() in GAME_FILES or '.save.' in path.name.casefold():
        findings.append({'file': name, 'reason': 'game configuration, asset or save file'})
    if path.name.casefold().startswith('.env') or path.name.casefold() in {
            'practice-originals.ini', 'active.json', 'overlay-install.json'}:
        findings.append({'file': name, 'reason': 'private configuration or credential file'})
    if not release and path.name == 'BUILD.json':
        findings.append({'file': name, 'reason': 'generated binary identity belongs in release only'})
    if path.suffix.casefold() in SOURCE_BINARIES:
        if not (release and name == PRODUCTION_BINARY):
            findings.append({'file': name, 'reason': 'unapproved compiled/generated file'})
    return findings


def audit_payload(payload: dict[str, bytes], release: bool = False) -> dict:
    findings = []
    total = 0
    game_hashes = known_game_hashes()
    for name, data in sorted(payload.items()):
        findings.extend(name_findings(name, release))
        total += len(data)
        if len(data) > MAX_FILE_BYTES:
            findings.append({'file': name, 'reason': 'file exceeds public audit size limit'})
            continue
        if hashlib.sha256(data).hexdigest() in game_hashes:
            findings.append({'file': name, 'reason': 'known full game baseline or asset content'})
        findings.extend(content_findings(name, data, binary=name == PRODUCTION_BINARY))
    if total > MAX_TOTAL_BYTES:
        findings.append({'file': '.', 'reason': 'payload exceeds public audit size limit'})
    return {'ok': not findings, 'checked_files': len(payload), 'bytes': total, 'findings': findings}


def source_files(root: Path) -> dict[str, bytes]:
    root = root.resolve()
    try:
        result = subprocess.run(['git', 'rev-parse', '--show-toplevel'], cwd=root,
                                capture_output=True, check=True, text=True)
        if Path(result.stdout.strip()).resolve() != root:
            raise ValueError('Not the selected repository root')
        result = subprocess.run(['git', 'ls-files', '-z', '--cached'], cwd=root,
                                capture_output=True, check=True)
        names = [item.decode('utf-8') for item in result.stdout.split(b'\0') if item]
    except (OSError, subprocess.CalledProcessError, ValueError):
        names = []
    if not names:
        # Before git init/staging, skip only known generated output folders.
        # Once tracked, even a wrongly added ignored file is audited above.
        names = [path.relative_to(root).as_posix() for path in root.rglob('*')
                 if path.is_file() and not any(generated_part(part)
                                              for part in path.relative_to(root).parts)
                 and path.name != 'BUILD.json' and path.suffix not in {'.pyc', '.pyo'}]
    payload = {}
    for name in names:
        path = root / name
        if path.resolve() != path.absolute() or not path.resolve().is_relative_to(root):
            raise ValueError('Redirected source file: ' + name)
        if not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
            raise ValueError('Missing or oversized source file: ' + name)
        payload[name] = path.read_bytes()
    return payload


def package_module(root: Path):
    package_path = root / 'ConvictionEnhanced/package.py'
    if not package_path.is_file():
        package_path = root / 'package.py'
    spec = importlib.util.spec_from_file_location('public_package_audit', package_path)
    if spec is None or spec.loader is None:
        raise ValueError('Public package source not found')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def audit_zip(path: Path, root: Path = ROOT) -> dict:
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if (len(entries) > 512 or len(entries) != len({entry.filename for entry in entries})
                or sum(entry.file_size for entry in entries) > MAX_TOTAL_BYTES
                or any(entry.file_size > MAX_FILE_BYTES for entry in entries)):
            raise ValueError('Duplicate or oversized release entries')
        if archive.testzip() is not None:
            raise ValueError('Release CRC failure')
        payload = {entry.filename: archive.read(entry.filename) for entry in entries}
    report = audit_payload(payload, release=True)
    if any(not name.startswith('ConvictionEnhanced/') for name in payload):
        report['findings'].append({'file': '.', 'reason': 'unexpected release root'})
    manifest_name = 'ConvictionEnhanced/PACKAGE-MANIFEST.json'
    manifest = json.loads(payload[manifest_name])
    expected = {'ConvictionEnhanced/' + name for name in manifest} | {manifest_name}
    if set(payload) != expected or any(
            hashlib.sha256(payload['ConvictionEnhanced/' + name]).hexdigest() != digest
            for name, digest in manifest.items()):
        raise ValueError('Release manifest mismatch')
    binary = payload[PRODUCTION_BINARY]
    module = package_module(root)
    if module.production_exports(binary) != ['InitializeASI']:
        raise ValueError('Unexpected production exports')
    build = json.loads(payload['ConvictionEnhanced/BUILD.json'])
    if (build.get('schema') != 1 or build.get('sha256') != hashlib.sha256(binary).hexdigest()
            or build.get('production_exports') != ['InitializeASI']
            or build.get('overlay_version') != module.OVERLAY_VERSION):
        raise ValueError('Reviewed binary identity mismatch')
    report['ok'] = not report['findings']
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument('--source', action='store_true')
    choice.add_argument('--zip', type=Path)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        report = audit_zip(args.zip, args.root) if args.zip else audit_payload(source_files(args.root))
        if not report['checked_files']:
            raise ValueError('No public files selected')
        print(json.dumps(report, indent=2))
        return 0 if report['ok'] else 2
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print('Public audit stopped: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

