"""Settings bridge for the in-game menu; Python 3.10+, standard library only.

Commands: refresh exports diagnostics; apply commits a queued selection while the
game is closed; launch applies, verifies and opens Steam; discard archives a queue.
The ASI never applies settings. Launch Michael Works Mod.cmd must be used after queuing them.
No Steam Input, UseController, game memory, credentials or save data are accessed.
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
from datetime import datetime, timezone
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import enhanced
import external_launch

STATUS = 'overlay-status.ini'
PENDING = 'overlay-pending.ini'
MAX_REQUEST_BYTES = 4096
CHOICES = {
    'FOV': {'90', '95', '100'},
    'Deadzone': {'0.15', '0.2', '0.25'},
    'Controls': {'blacklist', 'original'},
    'MouseInput': {'standard', 'raw'},
    'Tactical': {'0', '1'},
    'Deniable': {'0', '1'},
}
DEFAULTS = dict(FOV='95', Deadzone='0.15', Controls='blacklist',
                MouseInput='standard', Tactical='0', Deniable='0')


def parser():
    result = configparser.ConfigParser(interpolation=None, strict=True,
                                       empty_lines_in_values=False)
    result.optionxform = str
    return result


def parse_request(data):
    """A queue can only choose the six supported values, never paths or commands."""
    if not data or len(data) > MAX_REQUEST_BYTES:
        raise enhanced.Conflict('Queued settings are empty or too large')
    try:
        text = data.decode('ascii')
        if '\x00' in text:
            raise ValueError('NUL byte')
        ini = parser()
        ini.read_string(text)
        if ini.defaults() or set(ini.sections()) != {'Selection', 'Request'}:
            raise ValueError('unexpected sections or defaults')
        if set(ini['Selection']) != set(CHOICES) or set(ini['Request']) != {'Schema', 'BaseRevision'}:
            raise ValueError('unexpected or missing keys')
        if ini['Request']['Schema'] != '1':
            raise ValueError('unsupported queue schema')
        selection = dict(ini['Selection'])
        if any(value not in CHOICES[key] for key, value in selection.items()):
            raise ValueError('unsupported setting value')
        revision = ini['Request']['BaseRevision']
        if re.fullmatch('[a-f0-9]{64}', revision) is None:
            raise ValueError('invalid revision')
        return selection, revision
    except (UnicodeError, configparser.Error, ValueError) as error:
        raise enhanced.Conflict('Invalid queued settings: ' + str(error)) from error


def selection_from_active(active):
    source = active['selection']
    modules = source['modules']
    if not isinstance(modules, list) or set(modules) - {'comfort', 'tactical', 'deniable'}:
        raise enhanced.Conflict('Installed module selection is unsupported')
    selected = dict(FOV=str(source['fov']), Deadzone=str(source['deadzone']),
                    Controls=source['controls'], MouseInput=source.get('mouse_input', 'raw'),
                    Tactical=str(int('tactical' in modules)), Deniable=str(int('deniable' in modules)))
    if any(value not in CHOICES[key] for key, value in selected.items()):
        raise enhanced.Conflict('Installed settings cannot be represented by this menu')
    return selected


def read_state(manager, name):
    path = manager.state_path(name)
    if not path.exists():
        return None
    with path.open('rb') as stream:
        return stream.read(MAX_REQUEST_BYTES + 1) if name == PENDING else stream.read()


def revision(manager):
    return enhanced.digest(read_state(manager, 'active.json'))


def require_closed():
    if enhanced.game_running():
        raise enhanced.Conflict('Close Conviction before applying settings or launching again')


def clean_message(message):
    # No local paths/account names, exception dumps or arbitrary config contents are exported.
    return re.sub(r'[^A-Za-z0-9 .,;:_()/+\-]', '?', message)[:240]


def render_ini(sections):
    ini = parser()
    for section, values in sections.items():
        ini[section] = {key: str(value) for key, value in values.items()}
    result = io.StringIO()
    ini.write(result, space_around_delimiters=False)
    return result.getvalue().replace('\n', '\r\n').encode('ascii')


def safe_identity(manager, relative):
    path = manager.root / relative
    if path.resolve() != path or not path.resolve().is_relative_to(manager.root):
        raise enhanced.Conflict('Diagnostic identity path is redirected')
    return enhanced.digest(enhanced.read_optional(path)) or 'missing'


@contextlib.contextmanager
def bridge_lock(manager):
    """Serialize bridge writers independently of the manager's transaction lock."""
    manager.store.mkdir(parents=True, exist_ok=True)
    lock = manager.state_path('overlay-operation.lock')
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as error:
        raise enhanced.Conflict('Another menu bridge operation is running; retry after it finishes') from error
    try:
        os.close(fd)
        yield
    finally:
        manager.state_path('overlay-operation.lock').unlink()


def refresh_unlocked(manager):
    # The manager lock prevents a partial installation from being exported as a good state.
    with manager.locked():
        current_revision = revision(manager)
        status = dict(Schema='1', Version=clean_message(enhanced.VERSION),
                      CheckedAtUTC=datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), Installed='0',
                      Integrity='not-installed', Compatible='0', CanQueue='0',
                      Revision=current_revision or '', Pending='none', PendingRecovery='0',
                      GameSHA256='missing', FusionSHA256='missing', Message='Michael Works Splinter Cell Conviction Mod is not installed.')
        selected = dict(DEFAULTS)
        try:
            status['GameSHA256'] = safe_identity(manager, 'src/system/conviction_game.exe')
            status['FusionSHA256'] = safe_identity(manager, 'src/system/scripts/SplinterCellConviction.FusionFix.asi')
            manager.inspect()
            status['Compatible'] = '1'
            active = manager.active()
            if active:
                status['Installed'] = '1'
                selected = selection_from_active(active)
                manager.check_active(active)
                if selected['Controls'] == 'blacklist':
                    manager.check_controls()
                status.update(Integrity='ok', Message='Saved settings and managed files verified.')
                status['CanQueue'] = '1'
            elif owned_core_installed(manager.root):
                status.update(Installed='1', Integrity='ok', CanQueue='0',
                              Message='Core menu installed. Optional presets need verified local baseline INIs before settings can be queued.')
        except (enhanced.Conflict, OSError, ValueError, KeyError, TypeError):
            status.update(Integrity='conflict', CanQueue='0',
                          Message='Installation check failed. Close the game and run the manager check.')
        if manager.state_path('pending.json').exists():
            status.update(PendingRecovery='1', CanQueue='0', Integrity='conflict',
                          Message='Interrupted installation found. Close the game and recover with the manager.')
        pending = read_state(manager, PENDING)
        if pending is not None:
            try:
                _, base = parse_request(pending)
                status['Pending'] = 'queued' if base == current_revision else 'stale'
            except enhanced.Conflict:
                status['Pending'] = 'invalid'
        enhanced.atomic(manager.state_path(STATUS), render_ini({'Status': status, 'Selection': selected}))
        return status


class CheckedManager(enhanced.Manager):
    """Revision guard is evaluated inside Manager.install's existing exclusive lock."""
    expected_revision = None
    expected_request = None

    def __init__(self, root):
        if not Path(root).is_dir():
            raise enhanced.Conflict('The game folder does not exist')
        super().__init__(root)

    def guard_request(self):
        require_closed()
        if not self.expected_revision or revision(self) != self.expected_revision:
            raise enhanced.Conflict('Queued settings are stale. Reopen the menu and save again, or discard them')
        if read_state(self, PENDING) != self.expected_request:
            raise enhanced.Conflict('Queued settings changed during the operation; retry')
        if self.state_path('pending.json').exists():
            raise enhanced.Conflict('Interrupted installation found. Recover with the manager first')

    def plan(self, *args, **kwargs):
        self.guard_request()
        return super().plan(*args, **kwargs)


def archive_request(manager, data, action):
    """Keep a bounded last receipt; never delete a newer queued request."""
    if read_state(manager, PENDING) != data:
        raise enhanced.Conflict('Queued settings changed; the new request has been retained')
    destination = manager.state_path('overlay-' + action + '.ini')
    os.replace(manager.state_path(PENDING), destination)


def apply_unlocked(manager):
    require_closed()
    data = read_state(manager, PENDING)
    if data is None:
        return 'No queued settings.'
    selected, base = parse_request(data)
    manager.expected_revision, manager.expected_request = base, data
    # No-op requests are acknowledged without re-writing any game file or active.json.
    with manager.locked():
        manager.guard_request()
        active = manager.active()
        if not active:
            raise enhanced.Conflict('Install Michael Works Splinter Cell Conviction Mod before using queued settings')
        manager.inspect()
        manager.check_active(active)
        if selection_from_active(active)['Controls'] == 'blacklist':
            manager.check_controls()
        unchanged = selection_from_active(active) == selected
        if unchanged:
            archive_request(manager, data, 'applied')
            return 'Settings already match; queued request acknowledged.'
    modules = {name for key, name in (('Tactical', 'tactical'), ('Deniable', 'deniable'))
               if selected[key] == '1'}
    manager.install(modules, fov=int(selected['FOV']), deadzone=float(selected['Deadzone']),
                    controls=selected['Controls'], mouse_input=selected['MouseInput'])
    with manager.locked():
        archive_request(manager, data, 'applied')
    return 'Queued settings applied. Start the game to use them.'


def refresh(root):
    manager = CheckedManager(root)
    with bridge_lock(manager):
        return refresh_unlocked(manager)


def apply(root):
    manager = CheckedManager(root)
    with bridge_lock(manager):
        result = apply_unlocked(manager)
        refresh_unlocked(manager)
        return result


def discard(root):
    manager = CheckedManager(root)
    with bridge_lock(manager):
        require_closed()
        data = read_state(manager, PENDING)
        if data is not None:
            with manager.locked():
                archive_request(manager, data, 'discarded')
        refresh_unlocked(manager)
        return 'Queued settings discarded and archived.' if data is not None else 'No queued settings.'


def owned_core_installed(root):
    # A core-only public install has no preset active.json. Its own ASI
    # ownership/integrity record is still sufficient to launch the menu.
    import overlay_install
    status = overlay_install.OverlayInstaller(root).status()
    return status['installed'] and status['integrity'] == 'ok' and status['compatible']


def prepare_launch_saves(root, save_source=None):
    import public_setup
    return public_setup.prepare_saves(root, save_source)


def launch(root, save_source=None):
    manager = CheckedManager(root)
    with bridge_lock(manager):
        require_closed()
        apply_unlocked(manager)
        status = refresh_unlocked(manager)
        if status['Integrity'] != 'ok' or status['Compatible'] != '1' or status['Pending'] != 'none':
            raise enhanced.Conflict('Installation or queue check failed. Game was not launched')
        # This preparation commits a new coherent backup and private source
        # marker before every public launch. Account ambiguity refuses launch.
        prepare_launch_saves(root, save_source)
        # Check again immediately before requesting Steam startup, holding the manager lock.
        with manager.locked():
            require_closed()
            if (revision(manager) or '') != status['Revision'] or read_state(manager, PENDING) is not None:
                raise enhanced.Conflict('Settings changed before launch; retry')
            if manager.state_path('pending.json').exists():
                raise enhanced.Conflict('Interrupted installation found before launch; recover with the manager')
            manager.inspect()
            active = manager.active()
            manager.check_active(active)
            if active and selection_from_active(active)['Controls'] == 'blacklist':
                manager.check_controls()
            external_launch.request_steam_launch()
        return 'Launch requested through Steam. Pause the game, then use Ctrl+Shift+F10 for the Michael Works mod menu.'


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('action', choices=['refresh', 'apply', 'launch', 'discard'])
    cli.add_argument('--game', type=Path, default=Path(__file__).resolve().parent.parent)
    cli.add_argument('--save-source', type=Path, help='Explicit local Ubisoft savegames/<account>/2 folder for launch.')
    args = cli.parse_args(argv)
    try:
        if not args.game.is_dir():
            raise enhanced.Conflict('The game folder does not exist')
        result = launch(args.game, args.save_source) if args.action == 'launch' else globals()[args.action](args.game)
        print(json.dumps(result, indent=2) if isinstance(result, dict) else result)
    except (enhanced.Conflict, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print('STOP:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
