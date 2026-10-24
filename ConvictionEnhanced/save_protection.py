"""Read-only originals + consistent, verified backups for checkpoint practice.

Original saves are never written by this helper. Native practice_storage.cpp
isolates subsequent game save operations; this backup is an additional recovery
copy, not a restore-after-autosave scheme. Snapshot directories contain private
local paths and saved data and must never enter the portable release archive.
"""
from __future__ import annotations

import argparse
import ctypes as c
from ctypes import wintypes as w
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import uuid


MAX_FILES = 128
MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
REPARSE = 0x400
DIRECTORY = 0x10
INVALID = c.c_void_p(-1).value


class SaveProtectionError(RuntimeError):
    pass


class FileTime(c.Structure):
    _fields_ = [('low', w.DWORD), ('high', w.DWORD)]


class FileInfo(c.Structure):
    _fields_ = [('attributes', w.DWORD), ('created', FileTime),
                ('accessed', FileTime), ('modified', FileTime),
                ('volume', w.DWORD), ('size_high', w.DWORD),
                ('size_low', w.DWORD), ('links', w.DWORD),
                ('index_high', w.DWORD), ('index_low', w.DWORD)]


def _kernel():
    k = c.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p,
                             w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.GetFileInformationByHandle.argtypes = [w.HANDLE, c.POINTER(FileInfo)]
    k.GetFileInformationByHandle.restype = w.BOOL
    k.ReadFile.argtypes = [w.HANDLE, c.c_void_p, w.DWORD,
                          c.POINTER(w.DWORD), c.c_void_p]
    k.ReadFile.restype = w.BOOL
    k.GetFileAttributesW.argtypes = [w.LPCWSTR]
    k.GetFileAttributesW.restype = w.DWORD
    k.MoveFileExW.argtypes = [w.LPCWSTR, w.LPCWSTR, w.DWORD]
    k.MoveFileExW.restype = w.BOOL
    return k


def _win(path: Path) -> str:
    return '\\\\?\\' + str(path)


def _safe_directory(path: Path) -> Path:
    path = Path(os.path.abspath(path))
    if not path.drive or len(path.drive) != 2 or path.drive[1] != ':':
        raise SaveProtectionError('Use an absolute local-drive directory.')
    if ':' in str(path)[2:]:
        raise SaveProtectionError('Alternate streams are not supported.')
    k = _kernel()
    for current in [*reversed(path.parents), path]:
        attrs = k.GetFileAttributesW(_win(current))
        if attrs == 0xffffffff or not attrs & DIRECTORY or attrs & REPARSE:
            raise SaveProtectionError('Save/backup directory is missing or unsafe.')
    return path


def _inventory(source: Path) -> list[str]:
    names = []
    for item in source.iterdir():
        info = item.lstat()
        if info.st_file_attributes & (DIRECTORY | REPARSE):
            raise SaveProtectionError('Save group contains a subdirectory or reparse point; no snapshot committed.')
        if info.st_size > MAX_FILE_BYTES:
            raise SaveProtectionError('A save exceeds the supported backup size.')
        names.append(item.name)
    if not names or len(names) > MAX_FILES:
        raise SaveProtectionError('Save group is empty or exceeds the supported file count.')
    return sorted(names, key=str.casefold)


class LockedSave:
    """Deny writes/deletes until the complete group snapshot has been read."""
    def __init__(self, path: Path):
        self.k = _kernel()
        self.handle = self.k.CreateFileW(_win(path), 0x80000000, 1, None, 3,
                                         0x00200000, None)
        if self.handle == INVALID:
            raise SaveProtectionError('A save is busy; wait for saving to finish and retry.')
        try:
            self.info = self.information()
        except Exception:
            self.close()
            raise
        if self.info.attributes & (REPARSE | DIRECTORY):
            self.close()
            raise SaveProtectionError('Save target is not an ordinary file.')

    def information(self):
        info = FileInfo()
        if not self.k.GetFileInformationByHandle(self.handle, c.byref(info)):
            raise SaveProtectionError('Could not inspect a locked save.')
        return info

    def read(self) -> bytes:
        size = (self.info.size_high << 32) | self.info.size_low
        if size > MAX_FILE_BYTES:
            raise SaveProtectionError('A save exceeds the supported backup size.')
        data = c.create_string_buffer(size or 1)
        done = w.DWORD()
        if size and (not self.k.ReadFile(self.handle, data, size, c.byref(done), None)
                     or done.value != size):
            raise SaveProtectionError('Could not read a complete locked save.')
        after = self.information()
        before_identity = (self.info.volume, self.info.index_high, self.info.index_low,
                           self.info.modified.high, self.info.modified.low,
                           self.info.size_high, self.info.size_low)
        after_identity = (after.volume, after.index_high, after.index_low,
                          after.modified.high, after.modified.low,
                          after.size_high, after.size_low)
        if before_identity != after_identity:
            raise SaveProtectionError('A save changed during backup; no snapshot committed.')
        return data.raw[:size]

    def close(self):
        if self.handle != INVALID:
            self.k.CloseHandle(self.handle)
            self.handle = INVALID


def _write_new(path: Path, value: bytes):
    with path.open('xb') as out:
        out.write(value)
        out.flush()
        os.fsync(out.fileno())


def _coherent_bytes(source: Path):
    names = _inventory(source)
    locked = []
    try:
        for name in names:
            locked.append((name, LockedSave(source / name)))
        if _inventory(source) != names:
            raise SaveProtectionError('Save inventory changed; no snapshot committed.')
        result = {name: handle.read() for name, handle in locked}
        if sum(map(len, result.values())) > MAX_TOTAL_BYTES:
            raise SaveProtectionError('Save group exceeds the supported total size.')
        if _inventory(source) != names:
            raise SaveProtectionError('Save inventory changed; no snapshot committed.')
        return result
    finally:
        for _, handle in locked:
            handle.close()


def snapshot_save_group(source: Path, store: Path) -> Path:
    """Copy a flat Ubisoft product save group; never modify source files."""
    source = _safe_directory(source)
    store = Path(os.path.abspath(store))
    if store == source or store.is_relative_to(source):
        raise SaveProtectionError('Backups must be outside the original save group.')
    existing = store
    while not existing.exists():
        existing = existing.parent
    _safe_directory(existing)
    store.mkdir(parents=True, exist_ok=True)
    store = _safe_directory(store)
    data = _coherent_bytes(source)
    identifier = uuid.uuid4().hex
    pending = store / ('.pending-' + identifier)
    committed = store / identifier
    pending.mkdir()
    (pending / 'blobs').mkdir()
    manifest = {'schema': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'source': str(source), 'files': {}}
    written = set()
    for name, payload in data.items():
        digest = hashlib.sha256(payload).hexdigest()
        if digest not in written:
            _write_new(pending / 'blobs' / digest, payload)
            written.add(digest)
        manifest['files'][name] = {'size': len(payload), 'sha256': digest}
    encoded = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    _write_new(pending / 'manifest.json', encoded)
    if not _kernel().MoveFileExW(_win(pending), _win(committed), 8):
        raise SaveProtectionError('Could not commit backup; originals were not changed.')
    validate_snapshot(committed)
    return committed


def validate_snapshot(snapshot: Path) -> dict:
    snapshot = _safe_directory(snapshot)
    manifest_path = snapshot / 'manifest.json'
    if manifest_path.lstat().st_file_attributes & REPARSE or manifest_path.stat().st_size > 128 * 1024:
        raise SaveProtectionError('Backup manifest is unsafe or too large.')
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if not isinstance(manifest, dict) or set(manifest) != {'schema', 'created_utc', 'source', 'files'} or manifest['schema'] != 1:
        raise SaveProtectionError('Unsupported save-backup schema.')
    if not isinstance(manifest['source'], str) or not isinstance(manifest['created_utc'], str):
        raise SaveProtectionError('Backup source metadata is malformed.')
    files = manifest['files']
    if not isinstance(files, dict) or not 0 < len(files) <= MAX_FILES:
        raise SaveProtectionError('Backup inventory is malformed.')
    _safe_directory(snapshot / 'blobs')
    total = 0
    for name, entry in files.items():
        if not name or Path(name).name != name or name in {'.', '..'} or ':' in name or '/' in name or '\\' in name:
            raise SaveProtectionError('Unsafe filename in backup manifest.')
        if not isinstance(entry, dict) or set(entry) != {'size', 'sha256'} or not isinstance(entry['size'], int) or not 0 <= entry['size'] <= MAX_FILE_BYTES:
            raise SaveProtectionError('Backup entry is malformed.')
        digest = entry['sha256']
        if not isinstance(digest, str) or len(digest) != 64 or any(ch not in '0123456789abcdef' for ch in digest):
            raise SaveProtectionError('Backup digest is malformed.')
        blob = snapshot / 'blobs' / digest
        info = blob.lstat()
        if info.st_file_attributes & (DIRECTORY | REPARSE) or info.st_size != entry['size']:
            raise SaveProtectionError('Backup blob is missing, unsafe or incomplete.')
        if hashlib.sha256(blob.read_bytes()).hexdigest() != digest:
            raise SaveProtectionError('Backup blob failed its integrity check.')
        total += entry['size']
    if total > MAX_TOTAL_BYTES:
        raise SaveProtectionError('Backup exceeds supported total size.')
    return manifest


def verify_originals(snapshot: Path) -> dict:
    manifest = validate_snapshot(snapshot)
    current = _coherent_bytes(_safe_directory(Path(manifest['source'])))
    actual = {name: hashlib.sha256(data).hexdigest() for name, data in current.items()}
    expected = {name: entry['sha256'] for name, entry in manifest['files'].items()}
    return {'unchanged': actual == expected, 'files': len(expected),
            'added': sorted(set(actual) - set(expected)),
            'removed': sorted(set(expected) - set(actual)),
            'changed': sorted(name for name in actual.keys() & expected.keys()
                              if actual[name] != expected[name]),
            'current_hashes': actual}


def plan_restore(snapshot: Path, expected_current_hashes: dict[str, str]) -> dict:
    """Validate a recovery plan and refuse external edits; write no originals."""
    report = verify_originals(snapshot)
    if report['current_hashes'] != expected_current_hashes:
        raise SaveProtectionError('Original saves changed externally; recovery plan refused.')
    return {'ready': True, 'writes_performed': False, 'already_original': report['unchanged'],
            'files': report['files'], 'reason': 'Validated backup only; no automatic restoration is performed.'}


def prepare_practice_source(snapshot: Path, mod_state_root: Path) -> Path:
    """Publish a private UTF-16 marker from a verified unchanged source group.

    The native guard additionally verifies that the directory belongs to the
    currently loaded Ubisoft API's savegames/<account>/2 hierarchy. This helper
    never changes source saves or grants the game permission to write them.
    """
    manifest = validate_snapshot(snapshot)
    if not verify_originals(snapshot)['unchanged']:
        raise SaveProtectionError('Saved games changed; make a fresh backup before preparing practice.')
    source = _safe_directory(Path(manifest['source']))
    if source.name != '2':
        raise SaveProtectionError('Practice source must be Conviction product save group 2.')
    state = _safe_directory(mod_state_root)
    target = state / 'practice-originals.ini'
    if target.exists() and target.lstat().st_file_attributes & (REPARSE | DIRECTORY):
        raise SaveProtectionError('Practice source marker target is unsafe.')
    if any(ch in str(source) for ch in '\r\n'):
        raise SaveProtectionError('Practice source path contains unsupported characters.')
    text = ('[Source]\r\nSchema=1\r\nDirectory=' + str(source)
            + '\r\nFiles=' + str(len(manifest['files']))
            + '\r\nSnapshot=' + str(Path(snapshot).resolve()) + '\r\n')
    temporary = state / ('.practice-originals-' + uuid.uuid4().hex + '.tmp')
    _write_new(temporary, text.encode('utf-16'))
    if not _kernel().MoveFileExW(_win(temporary), _win(target), 1 | 8):
        raise SaveProtectionError('Could not publish practice source marker; originals were not changed.')
    return target


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['snapshot', 'verify'])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--store', type=Path, default=Path(__file__).resolve().parent.parent / 'ConvictionEnhanced-state' / 'save-backups')
    parser.add_argument('--snapshot', type=Path)
    parser.add_argument('--marker', type=Path, help='After snapshot, prepare the private marker in this mod-state directory')
    args = parser.parse_args(argv)
    try:
        if args.action == 'snapshot':
            if args.source is None:
                parser.error('--source is required for snapshot')
            result = snapshot_save_group(args.source, args.store)
            if args.marker is not None:
                prepare_practice_source(result, args.marker)
            print('Verified save backup committed: ' + str(result))
        else:
            if args.snapshot is None:
                parser.error('--snapshot is required for verify')
            report = verify_originals(args.snapshot)
            print(json.dumps({key: value for key, value in report.items() if key != 'current_hashes'}, indent=2))
            return 0 if report['unchanged'] else 2
    except (SaveProtectionError, OSError, ValueError) as error:
        print('Save protection: ' + str(error))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
