"""Install/remove only the mod ASI, with ownership, backups and crash recovery.

Install: py -3 overlay_install.py install --sha256 REVIEWED_BUILD_SHA256
Remove:  py -3 overlay_install.py uninstall
Inspect: py -3 overlay_install.py status
Each command accepts --game PATH. The build source is fixed at
native/build/ConvictionEnhanced.asi next to this script. Settings are never edited.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

import enhanced

PACKAGE = Path(__file__).resolve().parent
TARGET = 'src/system/scripts/ConvictionEnhanced.asi'
RECORD = 'overlay-install.json'
JOURNAL = 'overlay-install-pending.json'
HASH = re.compile('[a-f0-9]{64}')


def closed():
    if enhanced.game_running():
        raise enhanced.Conflict('Close Conviction before installing or removing the overlay')


def hash_valid(value, nullable=False):
    return (nullable and value is None) or (isinstance(value, str) and HASH.fullmatch(value) is not None)


def validate_pe(data):
    """Refuse accidental EXE, x64, debug text or truncated build output."""
    try:
        if len(data) < 64 or data[:2] != b'MZ':
            raise ValueError()
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        if offset < 64 or data[offset:offset + 4] != b'PE\x00\x00':
            raise ValueError()
        machine, sections = struct.unpack_from('<HH', data, offset + 4)
        optional_size, flags = struct.unpack_from('<HH', data, offset + 20)
        magic = struct.unpack_from('<H', data, offset + 24)[0]
        if (machine != 0x14c or not sections or magic != 0x10b or not flags & 0x2000
                or optional_size < 96 or len(data) < offset + 24 + optional_size + 40 * sections):
            raise ValueError()
    except (ValueError, struct.error):
        raise enhanced.Conflict('Overlay build must be a complete 32-bit PE DLL') from None


class OverlayInstaller:
    def __init__(self, root):
        if not Path(root).is_dir():
            raise enhanced.Conflict('The game folder does not exist')
        self.manager = enhanced.Manager(root)

    def target(self):
        path = self.manager.root / TARGET
        if path.resolve() != path or not path.resolve().is_relative_to(self.manager.root):
            raise enhanced.Conflict('Overlay destination must not be redirected by a link or junction')
        return path

    def source(self):
        path = PACKAGE / 'native/build/ConvictionEnhanced.asi'
        if path.resolve() != path or not path.resolve().is_relative_to(PACKAGE):
            raise enhanced.Conflict('Overlay build source must not be redirected')
        return path

    def record(self):
        path = self.manager.state_path(RECORD)
        record = json.loads(path.read_text('utf-8')) if path.exists() else None
        self.validate_record(record)
        return record

    def validate_record(self, record):
        if record is None:
            return
        if (not isinstance(record, dict)
                or set(record) != {'schema', 'root', 'target', 'original', 'installed'}
                or record['schema'] != 1 or record['root'] != str(self.manager.root)
                or record['target'] != TARGET or not hash_valid(record['original'], nullable=True)
                or not hash_valid(record['installed'])):
            raise enhanced.Conflict('Overlay ownership record is invalid or belongs to a different folder')

    def check_owned(self, record):
        if record and enhanced.digest(enhanced.read_optional(self.target())) != record['installed']:
            raise enhanced.Conflict('Overlay changed since installation; preserve or reconcile it before continuing')
        if record:
            self.manager.blob(record['original'])

    def write_record(self, record):
        path = self.manager.state_path(RECORD)
        if record is None:
            if path.exists():
                path.unlink()
        else:
            enhanced.atomic(path, enhanced.json_bytes(record))

    def write_target(self, data):
        path = self.target()
        if data is None:
            if path.exists():
                path.unlink()
        elif enhanced.read_optional(path) != data:
            enhanced.atomic(path, data)

    def recover_unlocked(self):
        path = self.manager.state_path(JOURNAL)
        if not path.exists():
            return
        closed()
        journal = json.loads(path.read_text('utf-8'))
        keys = {'schema', 'root', 'target', 'before', 'after', 'previous', 'next'}
        if (not isinstance(journal, dict) or set(journal) != keys or journal['schema'] != 1
                or journal['root'] != str(self.manager.root) or journal['target'] != TARGET
                or not hash_valid(journal['before'], nullable=True)
                or not hash_valid(journal['after'], nullable=True)):
            raise enhanced.Conflict('Overlay recovery record is invalid')
        self.validate_record(journal['previous'])
        self.validate_record(journal['next'])
        previous, next_record = journal['previous'], journal['next']
        if previous and previous['installed'] != journal['before']:
            raise enhanced.Conflict('Overlay recovery ownership does not match the previous file')
        if next_record:
            original = previous['original'] if previous else journal['before']
            if next_record['installed'] != journal['after'] or next_record['original'] != original:
                raise enhanced.Conflict('Overlay recovery ownership does not match the proposed file')
        elif previous is None or journal['after'] != previous['original']:
            raise enhanced.Conflict('Overlay removal recovery does not match the original backup')
        current = enhanced.digest(enhanced.read_optional(self.target()))
        if current not in (journal['before'], journal['after']):
            raise enhanced.Conflict('Overlay recovery would overwrite an external edit; recovery was stopped')
        if self.record() not in (journal['previous'], journal['next']):
            raise enhanced.Conflict('Overlay ownership changed during recovery; recovery was stopped')
        before = self.manager.blob(journal['before'])
        self.manager.blob(journal['after'])
        self.write_target(before)
        self.write_record(journal['previous'])
        path.unlink()

    def transaction(self, data, record):
        closed()
        previous = self.record()
        journal = dict(schema=1, root=str(self.manager.root), target=TARGET,
                       before=self.manager.save_blob(enhanced.read_optional(self.target())),
                       after=self.manager.save_blob(data), previous=previous, next=record)
        enhanced.atomic(self.manager.state_path(JOURNAL), enhanced.json_bytes(journal))
        try:
            closed()
            self.write_target(data)
            self.write_record(record)
            self.manager.state_path(JOURNAL).unlink()
        except Exception:
            self.recover_unlocked()
            raise

    def check_other_transaction(self):
        if self.manager.state_path('pending.json').exists():
            raise enhanced.Conflict('A mod settings transaction needs recovery before overlay installation changes')

    def install(self, expected_sha):
        if isinstance(expected_sha, str):
            expected_sha = expected_sha.lower()
        if not hash_valid(expected_sha):
            raise enhanced.Conflict('Supply --sha256 with the reviewed overlay build fingerprint')
        closed()
        data = self.source().read_bytes()
        if enhanced.digest(data) != expected_sha:
            raise enhanced.Conflict('Overlay build fingerprint differs from --sha256; no installation changes made')
        validate_pe(data)
        with self.manager.locked():
            closed()
            self.check_other_transaction()
            self.manager.inspect()
            active = self.manager.active()
            self.manager.check_active(active)
            if active and active['selection'].get('controls') == 'blacklist':
                self.manager.check_controls()
            self.recover_unlocked()
            previous = self.record()
            self.check_owned(previous)
            if previous and previous['installed'] == expected_sha:
                return 'Overlay already installed; no files changed.'
            original = (previous['original'] if previous else
                        self.manager.save_blob(enhanced.read_optional(self.target())))
            next_record = dict(schema=1, root=str(self.manager.root), target=TARGET,
                               original=original, installed=expected_sha)
            self.transaction(data, next_record)
        return 'Overlay installed. Existing Fusion Fix, controller configuration and mod selections are unchanged.'

    def uninstall(self):
        closed()
        with self.manager.locked():
            closed()
            self.check_other_transaction()
            self.recover_unlocked()
            previous = self.record()
            if previous is None:
                raise enhanced.Conflict('No owned overlay installation found; existing files were left untouched')
            self.check_owned(previous)
            self.transaction(self.manager.blob(previous['original']), None)
        return 'Overlay removed and its original file or absence restored. Other mod settings are unchanged.'

    def status(self):
        # Read-only: no state directory or lock is created by this command.
        record = self.record()
        current = enhanced.digest(enhanced.read_optional(self.target()))
        result = dict(installed=record is not None, target=TARGET, current_sha256=current,
                      installed_sha256=record['installed'] if record else None,
                      original_sha256=record['original'] if record else None,
                      pending_recovery=self.manager.state_path(JOURNAL).exists(),
                      busy=self.manager.state_path('operation.lock').exists(), integrity='unowned')
        if result['pending_recovery']:
            result['integrity'] = 'recovery-required'
        elif record:
            self.check_owned(record)
            result['integrity'] = 'ok'
        elif current is None:
            result['integrity'] = 'absent'
        try:
            self.manager.inspect()
            result['compatible'] = True
        except enhanced.Conflict:
            result['compatible'] = False
        # Detect a concurrent operation instead of asserting a consistent snapshot.
        if self.record() != record or enhanced.digest(enhanced.read_optional(self.target())) != current:
            raise enhanced.Conflict('Overlay changed during inspection; retry status')
        return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'uninstall', 'status'])
    parser.add_argument('--game', type=Path, default=PACKAGE.parent)
    parser.add_argument('--sha256', help='Reviewed SHA-256 of the native build; required for install')
    args = parser.parse_args(argv)
    try:
        installer = OverlayInstaller(args.game)
        result = installer.install(args.sha256) if args.action == 'install' else getattr(installer, args.action)()
        print(json.dumps(result, indent=2) if isinstance(result, dict) else result)
    except (enhanced.Conflict, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('STOP:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
