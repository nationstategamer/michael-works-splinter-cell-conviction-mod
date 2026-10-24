"""Test public presets only in a new, private copy of selected licensed files.

The source game, its state and saves are read only. No game is launched. The
explicit work directory is retained for inspection and contains game content:
never commit, package or publish it. Run --help for the required paths.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
import enhanced as ce
import local_baselines

MAX_SOURCE_BYTES = 64 * 1024 * 1024
REPARSE_POINT = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
EXE = 'src/system/conviction_game.exe'
FUSION_ASI = 'src/system/scripts/SplinterCellConviction.FusionFix.asi'


def plain(path, *, directory=False, missing=False):
    """Reject links/junctions without resolving away the evidence first."""
    path = Path(os.path.abspath(path))
    for item in reversed((path, *path.parents)):
        try:
            info = item.lstat()
        except FileNotFoundError:
            if item == path and missing:
                return path
            raise ValueError('Required path is missing: ' + str(item))
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & REPARSE_POINT:
            raise ValueError('Links and junctions are not accepted: ' + str(item))
        if item != path or directory:
            if not stat.S_ISDIR(info.st_mode):
                raise ValueError('Expected an ordinary directory: ' + str(item))
        elif not stat.S_ISREG(info.st_mode):
            raise ValueError('Expected an ordinary file: ' + str(item))
    return path


def source_bytes(path):
    path = plain(path)
    size = path.stat().st_size
    if not 0 < size <= MAX_SOURCE_BYTES:
        raise ValueError('Unsupported source file size: ' + str(path))
    with path.open('rb') as stream:
        data = stream.read(MAX_SOURCE_BYTES + 1)
    plain(path)
    if len(data) != size or len(data) > MAX_SOURCE_BYTES:
        raise ValueError('Source changed during reading: ' + str(path))
    return data


def fingerprint(data):
    return hashlib.sha256(data).hexdigest()


def inventory(root):
    result = {}
    for path in root.rglob('*'):
        if path.is_dir():
            plain(path, directory=True)
        else:
            result[path.relative_to(root).as_posix()] = fingerprint(source_bytes(path))
    return result


def overlaps(first, second):
    return first == second or first.is_relative_to(second) or second.is_relative_to(first)


def run(game, baseline_dir, work, original_sequence=None):
    game = plain(game, directory=True)
    baseline_dir = plain(baseline_dir, directory=True)
    work = plain(work, directory=True, missing=True)
    if work.exists():
        raise ValueError('--work must be a NEW private directory; nothing is overwritten')
    for protected in (game, PACKAGE.parent, baseline_dir):
        if overlaps(work, protected):
            raise ValueError('--work must be outside the source game, public repository and baseline source')
    if ce.game_running():
        raise ce.Conflict('Close Conviction before running the isolated integration test')

    source_manager = ce.Manager(game)
    source_manager.inspect()
    catalog = json.loads((PACKAGE / 'compatibility.json').read_text('utf-8'))
    assets = catalog['blacklist_assets']
    if not isinstance(assets, dict) or len(assets) != 49:
        raise ValueError('Expected the pinned 49-asset compatibility catalog')
    selected = set(assets) | {EXE, FUSION_ASI, ce.FUSION}
    optional = (ce.LEGACY, 'src/system/update/weapontuning.ini',
                'src/system/update/convictionsettings.ini', 'src/system/update/actionscheme.ini')
    selected.update(name for name in optional if (game / name).exists())
    for name in selected:
        candidate = Path(name)
        if candidate.is_absolute() or '..' in candidate.parts:
            raise ValueError('Unsafe source catalog path: ' + name)
    source_files = {name: source_bytes(game / name) for name in selected}
    for name, expected in assets.items():
        if name != ce.AIM_SEQUENCE and fingerprint(source_files[name]) != expected:
            raise ce.Conflict('Missing or changed pinned control asset: ' + name)

    expected_aim = assets[ce.AIM_SEQUENCE]
    original_path = None
    if original_sequence is not None:
        original_path = plain(original_sequence)
        original = source_bytes(original_path)
    elif fingerprint(source_files[ce.AIM_SEQUENCE]) == expected_aim:
        original = source_files[ce.AIM_SEQUENCE]
    else:
        # Read just the manager-owned original blob, never copy personal state.
        active = source_manager.active()
        files = active.get('files') if isinstance(active, dict) else None
        owned = files.get(ce.AIM_SEQUENCE) if isinstance(files, dict) else None
        if (not isinstance(owned, dict) or active.get('root') != str(game)
                or owned.get('original') != expected_aim
                or owned.get('installed') != fingerprint(source_files[ce.AIM_SEQUENCE])):
            raise ce.Conflict('Source aim sequence is modified without verified ownership. '
                              'Supply its pinned pristine original with --original-sequence.')
        original_path = plain(source_manager.state_path('blobs/' + expected_aim))
        original = source_bytes(original_path)
        if ce.repair_aim_release(original) != source_files[ce.AIM_SEQUENCE]:
            raise ce.Conflict('Source aim sequence is not the exact owned aim-release repair')
    if fingerprint(original) != expected_aim:
        raise ce.Conflict('Aim sequence original does not match the pinned compatibility hash')

    baseline_catalog = json.loads((PACKAGE / 'baselines/hashes.json').read_text('utf-8'))
    if set(baseline_catalog) != set(ce.FILES):
        raise ValueError('Expected exactly five baseline hashes')
    baseline_files = {name: source_bytes(baseline_dir / name) for name in ce.FILES}
    for name, data in baseline_files.items():
        if fingerprint(data) != baseline_catalog[name]:
            raise local_baselines.BaselineError('Baseline does not match the pinned pristine file: ' + name)
    sources_before = {str(game / name): fingerprint(data) for name, data in source_files.items()}
    sources_before.update({str(baseline_dir / name): fingerprint(data) for name, data in baseline_files.items()})
    if original_path:
        sources_before[str(original_path)] = fingerprint(original)

    # All external inputs have been checked before any private output is created.
    work.mkdir()
    root = work / 'game'
    root.mkdir()
    for name, data in source_files.items():
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(original if name == ce.AIM_SEQUENCE else data)
    local_baselines.import_baselines(root, baseline_dir)
    before = inventory(root)
    manager = ce.Manager(root)
    manager.inspect()
    manager.plan({'tactical', 'deniable'}, mouse_input='standard')
    if inventory(root) != before:
        raise AssertionError('Planning changed the isolated starting files')

    phases = []
    manager.install({'tactical', 'deniable'}, mouse_input='standard')
    manager.check_active(manager.active())
    manager.check_controls()
    if (root / ce.AIM_SEQUENCE).read_bytes() != ce.repair_aim_release(original):
        raise AssertionError('All-module install did not produce the exact derived aim repair')
    phases.append('all_modules_standard_mouse')
    manager.install(set(), fov=100, deadzone=0.25, controls='original', mouse_input='standard')
    manager.check_active(manager.active())
    if (root / ce.AIM_SEQUENCE).read_bytes() != original:
        raise AssertionError('Original controls did not restore the pristine local aim sequence')
    phases.append('comfort_original_controls')
    manager.install({'deniable'}, fov=95, deadzone=0.15, controls='blacklist', mouse_input='standard')
    manager.check_active(manager.active())
    manager.check_controls()
    phases.append('deniable_blacklist_controls')
    manager.uninstall()
    after = inventory(root)
    for name, expected in before.items():
        if after.get(name) != expected:
            raise AssertionError('Uninstall did not restore starting file: ' + name)
    unexpected = [name for name in after.keys() - before.keys()
                  if not name.startswith('ConvictionEnhanced-state/')]
    if unexpected:
        raise AssertionError('Uninstall left added game files: ' + ', '.join(sorted(unexpected)))
    if manager.active() is not None or manager.state_path('pending.json').exists():
        raise AssertionError('Uninstall left active or pending installation state')
    phases.append('exact_uninstall_rollback')
    sources_after = {name: fingerprint(source_bytes(Path(name))) for name in sources_before}
    if sources_after != sources_before:
        raise AssertionError('A source file changed during the isolated test')
    report = dict(passed=True, source_read_only=True, game_launched=False,
                  saves_copied=False, personal_state_copied=False, pinned_control_assets=len(assets),
                  imported_baselines=len(baseline_files), starting_files=len(before),
                  phases=phases, source_hashes_unchanged=True,
                  starting_hashes=before)
    report_path = work / 'integration-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: isolated public presets, pinned compatibility, exact rollback and unchanged source files.')
    print('Private report: ' + str(report_path))
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path, help='Read-only licensed source game folder')
    parser.add_argument('--baseline-dir', required=True, type=Path, help='Read-only folder of five pinned pristine INIs')
    parser.add_argument('--work', required=True, type=Path, help='NEW private output folder outside the game and public repository')
    parser.add_argument('--original-sequence', type=Path, help='Optional pinned pristine aim sequence, when source repair is not manager owned')
    options = parser.parse_args(argv)
    try:
        run(options.game, options.baseline_dir, options.work, options.original_sequence)
    except (ce.Conflict, local_baselines.BaselineError, OSError, ValueError, KeyError, TypeError, AssertionError) as error:
        print('STOP: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())