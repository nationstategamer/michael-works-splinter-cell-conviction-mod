"""Owned standalone setup/launcher using the existing guarded mod backends.

The GUI validates its frozen payload before entering these APIs. This module
never changes original saves, controller preferences or compatibility components.
The installed manager remains available after removing the mod.
"""
from __future__ import annotations

import contextlib
import json
import msvcrt
import os
from pathlib import Path
import re
import struct

import enhanced
import overlay_install
import public_setup
import save_protection


VERSION = '0.8.5'
TARGET = 'ConvictionEnhanced/Michael Works Mod.exe'
RECORD = 'standalone-manager.json'
JOURNAL = 'standalone-manager-pending.json'
LOCK = 'standalone-operation.lock'
MAX_EXE_BYTES = 64 * 1024 * 1024
RECORD_KEYS = {'schema', 'root', 'target', 'installed', 'version', 'save_source'}


class PartialInstallError(public_setup.SetupError):
    """The guarded core installed, but the persistent setup tool did not finish."""
    core_installed = True

    def __init__(self, result, recovery_required=False):
        self.result = result
        self.recovery_required = recovery_required
        message = ('The mod installed and its save backup is ready, but the local setup tool '
                   'could not be placed. Keep using the downloaded manager to launch, '
                   'check or remove the mod.')
        if recovery_required:
            message += ' Use Recover interrupted setup before another installation attempt.'
        else:
            message += ' The previous local setup tool or its absence was restored.'
        super().__init__(message)


def _target(manager):
    path = manager.root / TARGET
    if path.resolve() != path or not path.resolve().is_relative_to(manager.root):
        raise public_setup.SetupError('The local setup-tool destination is redirected.')
    ancestor = path.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    save_protection._safe_directory(ancestor)
    if path.exists() and not public_setup.plain_file(path):
        raise public_setup.SetupError('The local setup-tool destination is not a plain file.')
    return path


def _read(path, maximum=MAX_EXE_BYTES):
    if not public_setup.plain_file(path):
        raise public_setup.SetupError('The setup-tool file is missing.')
    before = path.stat()
    if not 0 < before.st_size <= maximum:
        raise public_setup.SetupError('The setup-tool file is empty or exceeds the supported size.')
    with path.open('rb') as source:
        data = source.read(maximum + 1)
    after = path.stat()
    if (len(data) != before.st_size or len(data) > maximum
            or (before.st_size, before.st_mtime_ns, before.st_ino)
            != (after.st_size, after.st_mtime_ns, after.st_ino)):
        raise public_setup.SetupError('The setup-tool file changed while it was being read.')
    return data


def _validate_exe(data):
    """Catch wrong/truncated source files; the GUI supplies its own executable."""
    try:
        if len(data) < 64 or data[:2] != b'MZ':
            raise ValueError()
        pe, = struct.unpack_from('<I', data, 0x3c)
        if pe < 64 or data[pe:pe + 4] != b'PE\0\0':
            raise ValueError()
        machine, sections = struct.unpack_from('<HH', data, pe + 4)
        optional_size, flags = struct.unpack_from('<HH', data, pe + 20)
        magic, = struct.unpack_from('<H', data, pe + 24)
        if ((machine, magic) not in {(0x14c, 0x10b), (0x8664, 0x20b)}
                or not sections or not flags & 2 or flags & 0x2000
                or optional_size < 96 or len(data) < pe + 24 + optional_size + 40 * sections):
            raise ValueError()
    except (ValueError, struct.error):
        raise public_setup.SetupError('Use the complete Windows standalone manager executable.') from None


def _hash(value, nullable=False):
    return ((nullable and value is None) or
            isinstance(value, str) and re.fullmatch('[a-f0-9]{64}', value) is not None)


def _validate_record(manager, record):
    if record is None:
        return
    if (not isinstance(record, dict) or set(record) != RECORD_KEYS or record['schema'] != 1
            or record['root'] != str(manager.root) or record['target'] != TARGET
            or not _hash(record['installed']) or not isinstance(record['version'], str)
            or re.fullmatch(r'\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?', record['version']) is None):
        raise public_setup.SetupError('The setup-tool ownership record is invalid or belongs to another game folder.')
    source = record['save_source']
    if (source is not None and (not isinstance(source, str) or any(c in source for c in '\r\n\0')
                               or not Path(source).is_absolute()
                               or str(Path(os.path.abspath(source))) != source)):
        raise public_setup.SetupError('The remembered save-account selection is malformed.')


def _record(manager):
    path = manager.state_path(RECORD)
    if not path.exists():
        return None
    try:
        data = _read(path, 16384)
        result = json.loads(data.decode('utf-8'))
    except (UnicodeError, ValueError) as error:
        raise public_setup.SetupError('The setup-tool ownership record is malformed.') from error
    _validate_record(manager, result)
    return result


def _current(manager):
    path = _target(manager)
    return enhanced.digest(_read(path)) if path.exists() else None


def _owned(manager, record, *, refuse_collision=True):
    actual = _current(manager)
    if record is not None and actual != record['installed']:
        raise public_setup.SetupError('The installed setup tool changed. Preserve the changed file before reinstalling.')
    if record is None and actual is not None and refuse_collision:
        raise public_setup.SetupError('An unowned file occupies the setup-tool destination. '
                                     'Preserve it elsewhere before installing; it was not replaced.')
    return actual


def _write_record(manager, record):
    path = manager.state_path(RECORD)
    if record is None:
        path.unlink(missing_ok=True)
    else:
        enhanced.atomic(path, enhanced.json_bytes(record))


def _write_target(manager, data):
    path = _target(manager)
    if data is None:
        path.unlink(missing_ok=True)
    elif (not path.exists() or _read(path) != data):
        path.parent.mkdir(parents=True, exist_ok=True)
        save_protection._safe_directory(path.parent)
        _target(manager)
        enhanced.atomic(path, data)


@contextlib.contextmanager
def _operation(manager):
    manager.store.mkdir(parents=True, exist_ok=True)
    save_protection._safe_directory(manager.store)
    lock = manager.state_path(LOCK)
    # Keep one plain empty file. The OS releases the byte-range lock when a
    # process exits, including an abrupt exit; no stale sentinel or unlink race
    # can prevent the recovery tool from opening next time.
    fd = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_BINARY, 0o600)
    locked = False
    try:
        if not public_setup.plain_file(lock) or os.fstat(fd).st_size != 0:
            raise public_setup.SetupError('The standalone operation lock is not a plain empty file.')
        os.lseek(fd, 0, os.SEEK_SET)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            locked = True
        except OSError as error:
            raise public_setup.SetupError('Another standalone setup operation is running. '
                                         'Close the other manager and retry.') from error
        yield
    finally:
        try:
            if locked:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        finally:
            os.close(fd)


def _recover_unlocked(manager):
    path = manager.state_path(JOURNAL)
    if not path.exists():
        return False
    public_setup.require_closed()
    try:
        journal = json.loads(_read(path, 65536).decode('utf-8'))
    except (UnicodeError, ValueError) as error:
        raise public_setup.SetupError('The interrupted setup-tool record is malformed.') from error
    keys = {'schema', 'root', 'target', 'before', 'after', 'previous', 'next'}
    if (not isinstance(journal, dict) or set(journal) != keys or journal['schema'] != 1
            or journal['root'] != str(manager.root) or journal['target'] != TARGET
            or not _hash(journal['before'], nullable=True) or not _hash(journal['after'])):
        raise public_setup.SetupError('The interrupted setup-tool record is invalid.')
    previous, next_record = journal['previous'], journal['next']
    _validate_record(manager, previous)
    _validate_record(manager, next_record)
    if (next_record is None or next_record['installed'] != journal['after']
            or (previous is None and journal['before'] is not None)
            or (previous is not None and previous['installed'] != journal['before'])):
        raise public_setup.SetupError('Interrupted setup ownership does not match the recorded files.')
    if _current(manager) not in (journal['before'], journal['after']):
        raise public_setup.SetupError('Setup recovery found an external file edit and stopped.')
    if _record(manager) not in (previous, next_record):
        raise public_setup.SetupError('Setup ownership changed during recovery; recovery stopped.')
    before = manager.blob(journal['before'])
    manager.blob(journal['after'])
    _write_target(manager, before)
    _write_record(manager, previous)
    path.unlink()
    return True


def _place_unlocked(manager, data, previous, source):
    public_setup.require_closed()
    if _record(manager) != previous:
        raise public_setup.SetupError('Setup ownership changed while the mod was being installed.')
    _owned(manager, previous)
    sha = enhanced.digest(data)
    next_record = dict(schema=1, root=str(manager.root), target=TARGET,
                       installed=sha, version=VERSION, save_source=str(source))
    if previous == next_record:
        return False
    before = _read(_target(manager)) if previous else None
    journal = dict(schema=1, root=str(manager.root), target=TARGET,
                   before=manager.save_blob(before), after=manager.save_blob(data),
                   previous=previous, next=next_record)
    enhanced.atomic(manager.state_path(JOURNAL), enhanced.json_bytes(journal))
    try:
        public_setup.require_closed()
        _write_target(manager, data)
        _write_record(manager, next_record)
        manager.state_path(JOURNAL).unlink()
    except Exception:
        _recover_unlocked(manager)
        raise
    return True


def _save_source(manager, explicit=None):
    if explicit is not None:
        return public_setup.choose_save_source(explicit)
    record = _record(manager)
    if record is not None:
        _owned(manager, record)
        if record['save_source'] is not None:
            return public_setup.choose_save_source(record['save_source'])
    return public_setup.choose_save_source()


def inspect(game, save_source=None):
    """Read-only local readiness; save_source is private and must not be logged."""
    game = public_setup.resolve_game(game)
    manager = enhanced.Manager(game)
    issues = []
    compatible, build, selected, candidates = False, None, None, []
    status = dict(installed=False, integrity='absent', pending_recovery=False)
    try:
        public_setup.require_compatible(game)
        compatible = True
        build = public_setup.reviewed_build()
    except (enhanced.Conflict, save_protection.SaveProtectionError, OSError, ValueError) as error:
        issues.append('Required files could not be read; check local folder access.'
                      if isinstance(error, OSError) else str(error))
    try:
        record = _record(manager)
        status['installed'] = record is not None
        current = _owned(manager, record)
        status['integrity'] = 'ok' if record else ('unowned' if current else 'absent')
        status['pending_recovery'] = manager.state_path(JOURNAL).exists()
        if status['pending_recovery']:
            status['integrity'] = 'recovery-required'
    except (enhanced.Conflict, save_protection.SaveProtectionError, OSError, ValueError) as error:
        status['integrity'] = 'conflict'
        issues.append('The local setup tool could not be checked.' if isinstance(error, OSError) else str(error))
    try:
        candidates = public_setup.discover_save_sources()
        selected = _save_source(manager, save_source)
    except (enhanced.Conflict, save_protection.SaveProtectionError, OSError) as error:
        issues.append('The local save-account selection could not be read.' if isinstance(error, OSError) else str(error))
    recovery = any(manager.state_path(name).exists() for name in
                   (JOURNAL, overlay_install.JOURNAL, 'pending.json'))
    if recovery:
        issues.append('Interrupted setup found. Use Recover interrupted setup with the game closed.')
    running = enhanced.game_running()
    if running:
        issues.append('Exit Conviction normally before setup, removal or launch.')
    ready = not issues and not running
    return dict(ready=ready, ready_for_install=ready, compatible=compatible,
                game_running=running, production_version=build['overlay_version'] if build else None,
                save_account_count=len(candidates), save_source=str(selected) if selected else None,
                issues=issues, manager=status, recovery_required=recovery)


def install(game, source_exe, save_source=None, baseline_dir=None, presets=None):
    public_setup.require_closed()
    game = public_setup.resolve_game(game)
    manager = enhanced.Manager(game)
    source_path = Path(os.path.abspath(source_exe))
    if source_path.resolve() != source_path:
        raise public_setup.SetupError('Use a direct local standalone manager executable.')
    data = _read(source_path)
    _validate_exe(data)
    # Refuse an unknown destination before core installation, baseline import or
    # save preparation. Recovery, if needed, is an explicit separate action.
    previous = _record(manager)
    _owned(manager, previous)
    if manager.state_path(JOURNAL).exists():
        raise public_setup.SetupError('Use Recover interrupted setup before reinstalling.')
    public_setup.require_compatible(game)
    public_setup.reviewed_build()
    selected = _save_source(manager, save_source)
    with _operation(manager):
        with manager.locked():
            public_setup.require_closed()
            if _record(manager) != previous:
                raise public_setup.SetupError('Setup ownership changed before installation.')
            _owned(manager, previous)
        result = public_setup.install(game, selected, baseline_dir, presets)
        try:
            with manager.locked():
                _place_unlocked(manager, data, previous, selected)
        except Exception as error:
            raise PartialInstallError(result, manager.state_path(JOURNAL).exists()) from error
    return dict(result, manager_installed=True, manager_path=str(_target(manager)),
                manager_version=VERSION, manager_retained_on_remove=True)


def launch(game, save_source=None):
    public_setup.require_closed()
    game = public_setup.resolve_game(game)
    manager = enhanced.Manager(game)
    with _operation(manager):
        if manager.state_path(JOURNAL).exists():
            raise public_setup.SetupError('Use Recover interrupted setup before launching.')
        selected = _save_source(manager, save_source)
        return public_setup.launch(game, selected)


def remove(game):
    public_setup.require_closed()
    game = public_setup.resolve_game(game)
    manager = enhanced.Manager(game)
    with _operation(manager):
        if manager.state_path(JOURNAL).exists():
            raise public_setup.SetupError('Use Recover interrupted setup before removing the mod.')
        result = public_setup.uninstall(game)
    return dict(result, manager_retained=True, private_backups_retained=True)


def recover(game):
    """Recover only existing owned journals; never restore original save files."""
    public_setup.require_closed()
    game = public_setup.resolve_game(game)
    public_setup.require_compatible(game)
    manager = enhanced.Manager(game)
    recovered = []
    with _operation(manager):
        with manager.locked():
            public_setup.require_closed()
            if manager.state_path('pending.json').exists():
                manager.recover_unlocked()
                recovered.append('presets')
            if manager.state_path(overlay_install.JOURNAL).exists():
                overlay_install.OverlayInstaller(game).recover_unlocked()
                recovered.append('core')
            if _recover_unlocked(manager):
                recovered.append('manager')
        if recovered:
            import overlay_bridge
            overlay_bridge.refresh(game)
    return dict(recovered=recovered, original_saves_written=False,
                private_backups_retained=True)
