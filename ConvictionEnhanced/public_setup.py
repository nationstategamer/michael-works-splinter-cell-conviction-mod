"""Public Windows setup for the Michael Works Conviction mod.

Only owned mod files and private local backup state are written. Original saves,
Steam Input and controller preferences are never edited by this helper.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sys

import enhanced
import overlay_install
import save_protection

PACKAGE = Path(__file__).resolve().parent
PREREQUISITES = {
    'src/system/version.dll': (
        'Ultimate ASI Loader 9.7.4 (32-bit version.dll)',
        'd5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6'),
    'src/system/dinput8.dll': (
        'Xidi DirectInput proxy 0.0.1-dev.1.0+cafe9af9',
        '5561a6f76bad4bc7af66e04771d7c61e7ce4d97edd08151aaea06aa7cf6f5559'),
    'src/system/Xidi.32.dll': (
        'Xidi 32-bit 0.0.1-dev.1.0+cafe9af9',
        '57859aba7fc67adf039119a05937fee40b53c53041b63645b761337abb98da77'),
}


class SetupError(enhanced.Conflict):
    pass


def require_closed():
    if enhanced.game_running():
        raise SetupError('Exit Conviction normally before setup, save preparation or launch.')


def plain_file(path: Path) -> bool:
    path = Path(os.path.abspath(path))
    if not path.exists():
        return False
    save_protection._safe_directory(path.parent)
    info = path.lstat()
    if info.st_file_attributes & (save_protection.REPARSE | save_protection.DIRECTORY):
        raise SetupError('A required file is redirected or is not a plain file: ' + path.name)
    return True


def resolve_game(game=None) -> Path:
    if os.name != 'nt':
        raise SetupError('This release requires Windows. No files were changed.')
    candidate = Path(game) if game is not None else PACKAGE.parent
    candidate = Path(os.path.abspath(candidate))
    if not (candidate / 'src/system/conviction_game.exe').is_file():
        raise SetupError('Select the actual Conviction game folder with --game PATH. '
                         'For a release install, place ConvictionEnhanced directly inside that folder.')
    try:
        save_protection._safe_directory(candidate)
        if not plain_file(candidate / 'src/system/conviction_game.exe'):
            raise SetupError('The game executable is missing.')
    except save_protection.SaveProtectionError as error:
        raise SetupError('Use the direct local game folder without links or junctions.') from error
    return candidate


def require_compatible(game: Path):
    for name in ('src/system/conviction_game.exe',
                 'src/system/scripts/SplinterCellConviction.FusionFix.asi',
                 enhanced.FUSION):
        if not plain_file(game / name):
            raise SetupError('Missing prerequisite: ' + name)
    manager = enhanced.Manager(game)
    manager.inspect()
    for relative, (label, expected) in PREREQUISITES.items():
        path = game / relative
        if not plain_file(path) or enhanced.digest(path.read_bytes()) != expected:
            raise SetupError('Missing or changed prerequisite: ' + relative + ' (' + label + '). '
                             'Install the complete supported official Conviction Fusion Fix bundle '
                             'described in docs/COMPATIBILITY.md. '
                             'Setup does not download or replace dependencies.')
    if not plain_file(game / 'src/system/Xidi.ini'):
        raise SetupError('The Xidi.ini configuration is missing. Use the complete supported official '
                         'Conviction Fusion Fix bundle described in docs/COMPATIBILITY.md. '
                         'Setup preserves controller mappings and Steam Input preferences.')
    return manager


def reviewed_build() -> dict:
    manifest = PACKAGE / 'BUILD.json'
    binary = PACKAGE / 'native/build/ConvictionEnhanced.asi'
    if not plain_file(manifest) or not plain_file(binary):
        raise SetupError('The reviewed production build is missing. Download the complete release '
                         'archive, or build and package the source first.')
    if manifest.stat().st_size > 16384:
        raise SetupError('BUILD.json is unexpectedly large.')
    info = json.loads(manifest.read_text('utf-8'))
    if (not isinstance(info, dict) or info.get('schema') != 1
            or not isinstance(info.get('overlay_version'), str)
            or not isinstance(info.get('sha256'), str)
            or re.fullmatch(r'[a-f0-9]{64}', info['sha256']) is None
            or info.get('production_exports') != ['InitializeASI']):
        raise SetupError('BUILD.json is malformed or is not a production build record.')
    data = binary.read_bytes()
    if hashlib.sha256(data).hexdigest() != info['sha256']:
        raise SetupError('The production plugin does not match BUILD.json; no installation performed.')
    overlay_install.validate_pe(data)
    import package as release_package
    if release_package.production_exports(data) != ['InitializeASI']:
        raise SetupError('The release plugin exposes unexpected or test-only exports.')
    return info


def launcher_roots() -> list[Path]:
    values = []
    try:
        import winreg
        keys = (r'SOFTWARE\Ubisoft\Launcher',
                r'SOFTWARE\WOW6432Node\Ubisoft\Launcher',
                r'SOFTWARE\Ubisoft\Ubisoft Game Launcher')
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            for key in keys:
                for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
                    try:
                        with winreg.OpenKey(hive, key, 0, winreg.KEY_READ | view) as opened:
                            value, kind = winreg.QueryValueEx(opened, 'InstallDir')
                        if kind in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) and value:
                            values.append(Path(os.path.expandvars(value)))
                    except OSError:
                        pass
    except ImportError:
        pass
    for variable in ('ProgramFiles(x86)', 'ProgramFiles'):
        value = os.environ.get(variable)
        if value:
            values.extend((Path(value) / 'Ubisoft/Ubisoft Game Launcher',
                           Path(value) / 'Ubisoft/Ubisoft Connect'))
    result, seen = [], set()
    for value in values:
        path = Path(os.path.abspath(value))
        key = os.path.normcase(str(path))
        if key not in seen:
            seen.add(key)
            result.append(path)
    return result


def validate_save_source(source: Path) -> Path:
    source = Path(os.path.abspath(source))
    try:
        save_protection._safe_directory(source)
        if source.name != '2' or source.parent.parent.name.casefold() != 'savegames':
            raise SetupError('Select Conviction\'s local savegames/<account>/2 folder with --save-source PATH.')
        launcher = source.parent.parent.parent
        known = {os.path.normcase(str(path)) for path in launcher_roots()}
        if (os.path.normcase(str(launcher)) not in known
                and not plain_file(launcher / 'ubiorbitapi_r2.dll')):
            raise SetupError('The save folder is not under a recognized Ubisoft launcher. '
                             'Use the launcher\'s actual savegames/<account>/2 directory.')
        save_protection._inventory(source)
    except save_protection.SaveProtectionError as error:
        raise SetupError('The selected original-save folder is missing, empty or unsafe.') from error
    return source


def discover_save_sources() -> list[Path]:
    result, seen = [], set()
    for launcher in launcher_roots():
        groups = launcher / 'savegames'
        if not groups.exists():
            continue
        try:
            save_protection._safe_directory(groups)
            accounts = sorted(groups.iterdir(), key=lambda p: p.name.casefold())
            for account in accounts:
                candidate = account / '2'
                if not candidate.exists():
                    continue
                candidate = validate_save_source(candidate)
                key = os.path.normcase(str(candidate))
                if key not in seen:
                    seen.add(key)
                    result.append(candidate)
        except (OSError, save_protection.SaveProtectionError) as error:
            raise SetupError('A discovered Ubisoft save path is unsafe. '
                             'Use --save-source with the direct local account folder.') from error
    return result


def choose_save_source(source=None) -> Path:
    if source is not None:
        return validate_save_source(Path(source))
    candidates = discover_save_sources()
    if len(candidates) != 1:
        if not candidates:
            raise SetupError('No local Conviction save group was found. Start the game once, create '
                             'a local save and exit, then supply --save-source PATH if needed.')
        raise SetupError('More than one Conviction save account was found. Nothing was selected. '
                         'Use diagnose --show-save-paths locally, then choose --save-source PATH.')
    return candidates[0]


def prepare_saves(game, source=None) -> dict:
    require_closed()
    game = resolve_game(game)
    manager = require_compatible(game)
    original = choose_save_source(source)
    manager.store.mkdir(parents=True, exist_ok=True)
    save_protection._safe_directory(manager.store)
    with manager.locked():
        require_closed()
        snapshot = save_protection.snapshot_save_group(original, manager.store / 'save-backups')
        report = save_protection.verify_originals(snapshot)
        if not report['unchanged']:
            raise SetupError('Original saves changed during preparation; retry after the launcher settles.')
        save_protection.prepare_practice_source(snapshot, manager.store)
        enhanced.atomic(manager.state_path('public-save-backup.json'), enhanced.json_bytes({
            'schema': 1, 'captured_utc': datetime.now(timezone.utc).isoformat(),
            'snapshot': str(snapshot), 'files': report['files'], 'originals_written': False}))
    return {'ready': True, 'files': report['files'], 'snapshot': str(snapshot)}


def diagnose(game, show_save_paths=False) -> dict:
    game = resolve_game(game)
    issues = []
    try:
        require_compatible(game)
        compatible = True
    except (enhanced.Conflict, OSError, save_protection.SaveProtectionError) as error:
        issues.append('A required game file could not be read; check local folder access.'
                      if isinstance(error, OSError) else str(error))
        compatible = False
    try:
        build = reviewed_build()
    except (enhanced.Conflict, OSError, ValueError) as error:
        issues.append('The release build could not be read; check local folder access.'
                      if isinstance(error, OSError) else str(error))
        build = None
    try:
        candidates = discover_save_sources()
    except (enhanced.Conflict, OSError) as error:
        issues.append('Local save folders could not be read; check folder access.'
                      if isinstance(error, OSError) else str(error))
        candidates = []
    running = enhanced.game_running()
    if not candidates:
        issues.append('No unambiguous local Conviction save source was found.')
    elif len(candidates) > 1:
        issues.append('Multiple save accounts need an explicit --save-source selection.')
    result = {'compatible': compatible, 'ready_for_install': not issues and not running,
              'issues': issues, 'game_running': running,
              'production_version': build['overlay_version'] if build else None,
              'save_account_count': len(candidates),
              'note': 'No game files, saves or settings were changed.'}
    if show_save_paths:
        result['private_local_save_sources'] = [str(path) for path in candidates]
    return result


def install(game, save_source=None, baseline_dir=None, presets=None) -> dict:
    require_closed()
    game = resolve_game(game)
    manager = require_compatible(game)
    build = reviewed_build()
    if baseline_dir is not None and presets is None:
        raise SetupError('--baseline-dir is used with an explicit --presets selection.')
    if presets is not None:
        import local_baselines
        try:
            local_baselines.import_baselines(game, Path(baseline_dir) if baseline_dir else None)
        except local_baselines.BaselineError as error:
            raise SetupError(str(error)) from error
        modules = set(presets) - {'comfort'}
        manager.plan(modules, mouse_input='standard')
    else:
        modules = None
    backup = prepare_saves(game, save_source)
    core = overlay_install.OverlayInstaller(game).install(build['sha256'])
    if modules is not None:
        manager.install(modules, mouse_input='standard')
    import overlay_bridge
    overlay_bridge.refresh(game)
    return {'installed': True, 'version': build['overlay_version'], 'core': core,
            'optional_presets': sorted(modules) if modules is not None else None,
            'save_backup_files': backup['files'], 'controller_preferences_changed': False}


def uninstall(game) -> dict:
    require_closed()
    game = resolve_game(game)
    manager = enhanced.Manager(game)
    active = manager.active()
    manager.check_active(active)
    if manager.state_path('pending.json').exists():
        raise SetupError('Recover the interrupted preset operation with enhanced.py recover before uninstalling.')
    installer = overlay_install.OverlayInstaller(game)
    status = installer.status()
    if not status['installed'] and not active:
        raise SetupError('No owned mod installation was found. No files were changed.')
    if status['installed']:
        installer.uninstall()
    if active:
        manager.uninstall()
    return {'uninstalled': True, 'original_saves_written': False,
            'private_backups_retained': True, 'queued_choices_retained': True}


def launch(game, save_source=None):
    game = resolve_game(game)
    require_compatible(game)
    import overlay_bridge
    return overlay_bridge.launch(game, save_source=save_source)


def main(argv=None):
    if sys.version_info < (3, 10):
        print('STOP: Install Python 3.10 or newer, then run setup again.', file=sys.stderr)
        return 1
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['diagnose', 'install', 'prepare-saves', 'launch', 'uninstall'])
    parser.add_argument('--game', type=Path, help='Actual game root; required when this folder is outside it.')
    parser.add_argument('--save-source', type=Path, help='Explicit local Ubisoft savegames/<account>/2 folder.')
    parser.add_argument('--baseline-dir', type=Path, help='Existing local unpacked INIs; no download or dump is performed.')
    parser.add_argument('--presets', nargs='+', choices=['comfort', 'tactical', 'deniable'],
                        help='Opt in to presets using locally verified baseline INIs; mouse input is standard.')
    parser.add_argument('--show-save-paths', action='store_true',
                        help='Diagnose only: display private local account paths; do not post this output publicly.')
    args = parser.parse_args(argv)
    try:
        if args.action == 'diagnose':
            result = diagnose(args.game, args.show_save_paths)
        elif args.action == 'install':
            result = install(args.game, args.save_source, args.baseline_dir, args.presets)
        elif args.action == 'prepare-saves':
            prepared = prepare_saves(args.game, args.save_source)
            result = {'ready': prepared['ready'], 'files': prepared['files'], 'original_saves_written': False}
        elif args.action == 'uninstall':
            result = uninstall(args.game)
        else:
            result = launch(args.game, args.save_source)
        print(json.dumps(result, indent=2) if isinstance(result, dict) else result)
        return 1 if args.action == 'diagnose' and result['issues'] else 0
    except (enhanced.Conflict, save_protection.SaveProtectionError, OSError, ValueError, KeyError, TypeError) as error:
        print('STOP:', error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
