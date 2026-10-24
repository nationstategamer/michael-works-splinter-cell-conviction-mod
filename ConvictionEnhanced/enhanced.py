"""Michael Works Splinter Cell Conviction Mod: byte-preserving, reversible Fusion Fix preset manager.

Python 3.10+, standard library only. No game binaries or saves are patched.
"""
from __future__ import annotations

import argparse
import contextlib
from collections import Counter
import csv
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid
import xml.etree.ElementTree as ET
from xml.parsers import expat

import local_baselines

PACKAGE = Path(__file__).resolve().parent
VERSION = '0.1.0-preview'
EXE_HASH = 'beebb33559e064ec2f78065a4e9bc8dfe8844fa8711ea4afbcb880616170936f'
FUSION_HASH = '41291418dd5b5e69c6758f81b06dff6ced8f54344661efc148a6146c5c084ce3'
FUSION = 'src/system/scripts/SplinterCellConviction.FusionFix.ini'
LEGACY = 'src/system/scripts/SplinterCellConviction.FusionMod.asi'
AIM_SEQUENCE = ('src/system/update/data/Blacklist/Sequences/maps/_single/'
                '08_thirdechelon_b/seq_bb03_narrativewalk.xml')
FILES = ('convictioncamera.ini', 'aiconfig.ini', 'healthtuning.ini',
         'huntermatchmanager.ini', 'laststandmatchmanager.ini')
ALLOWED = {FUSION, LEGACY, AIM_SEQUENCE, *('src/system/update/' + f for f in FILES)}

def repair_aim_release(data):
    """Derive a release track from the user's pinned local sequence bytes.

    No game XML is embedded in this public source. Parsing validates the exact
    functional node; Expat locates its original byte span so every other byte,
    including formatting/comments, remains unchanged.
    """
    if not isinstance(data, bytes) or not 0 < len(data) <= 2 * 1024 * 1024:
        raise Conflict('Aim-release sequence has an unsupported type or size')
    try:
        expected = json.loads((PACKAGE / 'compatibility.json').read_text('utf-8'))['blacklist_assets'][AIM_SEQUENCE]
        if not isinstance(expected, str) or re.fullmatch('[a-f0-9]{64}', expected) is None or digest(data) != expected:
            raise Conflict('Aim-release sequence is not the locally verified pinned original')
        if b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
            raise Conflict('Aim-release sequence must not contain document types or entities')
        root = ET.fromstring(data)
    except (ET.ParseError, OSError, ValueError, TypeError, KeyError) as error:
        raise Conflict('Aim-release sequence is not valid XML') from error
    tasks = [task for task in root.findall('Task')
             if task.get('name') == 'Start Narrative Walk']
    if root.tag != 'Sequence' or root.get('name') != 'BB03_NarrativeWalk' or len(tasks) != 1:
        raise Conflict('Unexpected aim-release sequence or start task')
    task = tasks[0]
    timelines = task.findall('Timeline')
    if task.get('first') != '1' or len(timelines) != 1 or timelines[0].get('name') != 'NewTimeline':
        raise Conflict('Unexpected aim-release start timeline')
    timeline = timelines[0]
    press = [track for track in timeline.findall('Track') if track.get('name') == 'NewTrack_21']
    disable = [command for command in timeline.iter('CommandInstance')
               if command.get('name') == 'PlayerEnableAllButtons']
    disabled = (len(disable) == 1 and
                disable[0].find("ParameterInstance[@name='Character']") is not None and
                disable[0].find("ParameterInstance[@name='Character']").get('data') == 'Repository::o_Character_Sam' and
                disable[0].find("ParameterInstance[@name='Enable']/Boolean") is not None and
                disable[0].find("ParameterInstance[@name='Enable']/Boolean").get('value') == '0')
    if (len(press) != 1 or not disabled or
            b'eActionType_ReleaseLeftTrigger' in data or b'MW_ReleaseAim' in data):
        raise Conflict('Aim-release sequence does not match the unique verified press-only block')
    track = press[0]
    character = 'Repository::o_Character_Sam'
    action = 'Repository::Unreal::enums::eActionType::eActionType_PressLeftTrigger'
    if (track.get('entity') != character or len(track) != 1 or track[0].tag != 'Event'
            or track[0].get('end') != '-1.000000' or len(track[0]) != 1):
        raise Conflict('Unexpected aim-release action schema')
    command = track[0][0]
    parameters = list(command)
    by_name = {item.get('name'): item for item in parameters}
    if (command.tag != 'CommandInstance' or command.get('name') != 'PlayerEnableButton'
            or len(parameters) != 3 or any(item.tag != 'ParameterInstance' for item in parameters)
            or set(by_name) != {'Character', 'ActionType', 'Enable'}
            or by_name['Character'].get('data') != character or len(by_name['Character'])
            or by_name['ActionType'].get('data') != action or len(by_name['ActionType'])
            or len(by_name['Enable']) != 1 or by_name['Enable'][0].tag != 'Boolean'
            or by_name['Enable'][0].get('value') != '1' or len(by_name['Enable'][0])):
        raise Conflict('Unexpected aim-release action schema')
    parser = expat.ParserCreate()
    stack, spans = [], []
    def start(tag, attributes):
        stack.append((tag, attributes, parser.CurrentByteIndex))
    def end(tag):
        current = stack.pop()
        if (tag == 'Track' and current[1].get('name') == 'NewTrack_21' and len(stack) == 3
                and stack[-1][0] == 'Timeline' and stack[-1][1].get('name') == 'NewTimeline'
                and stack[-2][0] == 'Task' and stack[-2][1].get('name') == 'Start Narrative Walk'):
            closing = parser.CurrentByteIndex
            if data[closing:closing+2] != b'</':
                raise Conflict('Unexpected aim-release closing tag')
            stop = data.find(b'>', closing)
            if stop < 0:
                raise Conflict('Unexpected aim-release byte span')
            spans.append((current[2], stop + 1))
    parser.StartElementHandler, parser.EndElementHandler = start, end
    parser.ExternalEntityRefHandler = lambda *arguments: 0
    try:
        parser.Parse(data, True)
    except expat.ExpatError as error:
        raise Conflict('Aim-release sequence byte locations are invalid') from error
    if len(spans) != 1:
        raise Conflict('Aim-release sequence has ambiguous byte locations')
    first, stop = spans[0]
    line = data.rfind(b'\n', 0, first) + 1
    if not data[line:first].strip():
        first = line
    if data[stop:stop+2] == b'\r\n':
        stop += 2
    elif data[stop:stop+1] == b'\n':
        stop += 1
    original = data[first:stop]
    opening_end = original.find(b'>') + 1
    name = re.compile(rb'(\bname\s*=\s*)([\x22\x27])NewTrack_21\2')
    opening, replacements = name.subn(lambda match: match[1] + match[2] + b'MW_ReleaseAim' + match[2], original[:opening_end])
    if replacements != 1 or original.count(action.encode('ascii')) != 1:
        raise Conflict('Aim-release byte transformation is ambiguous')
    release = (opening + original[opening_end:]).replace(
        b'eActionType_PressLeftTrigger', b'eActionType_ReleaseLeftTrigger', 1)
    return data[:stop] + release + data[stop:]


class Conflict(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest() if data is not None else None


def read_optional(path):
    return path.read_bytes() if path.exists() else None


def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temp.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp, path)
    finally:
        if temp.exists():
            temp.unlink()


def json_bytes(value):
    return (json.dumps(value, indent=2) + '\n').encode('utf-8')


def structure(data):
    """Count all original section/key occurrences, including repeated array entries."""
    counts, section = Counter(), ''
    for line in data.decode('latin-1').splitlines():
        heading = re.match(r'^\s*\[([^\]]+)\]', line)
        if heading:
            section = heading[1].casefold()
            counts[(section, None)] += 1
        else:
            entry = re.match(r'^\s*([\w]+)\s*=', line)
            if entry:
                counts[(section, entry[1].casefold())] += 1
    return counts


def patch(data, section, key, old, new, *, field=None, occurrence=None, count=1):
    """Change only the value token; never roundtrip through an INI serializer."""
    text = data.decode('latin-1')  # reversible mapping, including arbitrary comments
    lines = text.splitlines(keepends=True)
    current = None
    matches = []
    for i, line in enumerate(lines):
        heading = re.match(r'^\s*\[([^\]]+)\]', line)
        if heading:
            current = heading[1].casefold()
        if current == section.casefold() and re.match(r'^\s*' + re.escape(key) + r'\s*=', line, re.I):
            matches.append(i)
    if len(matches) != count:
        raise Conflict(f'{section}/{key}: expected {count} rows, found {len(matches)}')
    selected = matches if occurrence is None else [matches[occurrence]]
    for i in selected:
        line = lines[i]
        # Exclude comments from matching, retaining every original byte around the value.
        end = line.find('//')
        active = line if end < 0 else line[:end]
        expression = (r'(?<![\w])' + re.escape(field) + r'\s*=\s*') if field else (r'^\s*' + re.escape(key) + r'\s*=\s*')
        found = list(re.finditer('(' + expression + r')([^\s,;()]+)', active, re.I))
        if len(found) != 1:
            raise Conflict(f'{section}/{key}: ambiguous value for {field or key}')
        match = found[0]
        value = match[2]
        if value not in (str(old), str(new)):
            raise Conflict(f'{section}/{key}/{field or key}: expected {old!r}, found {value!r}; review your existing override')
        lines[i] = line[:match.start(2)] + str(new) + line[match.end(2):]
    return ''.join(lines).encode('latin-1')


def game_running():
    if os.name != 'nt':
        return False
    result = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True,
                            check=True, creationflags=subprocess.CREATE_NO_WINDOW)
    return any(row and row[0].lower().startswith('conviction') and row[0].lower().endswith('.exe')
               for row in csv.reader(result.stdout.splitlines()))


class Manager:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.store = self.root / 'ConvictionEnhanced-state'
        if self.store.is_symlink() or self.store.resolve() != self.store:
            raise Conflict('State directory must not be redirected by a link or junction')

    def target(self, relative):
        if relative not in ALLOWED:
            raise Conflict('Unrecognized managed path: ' + relative)
        result = self.root / relative
        if result.resolve() != result or not result.resolve().is_relative_to(self.root):
            raise Conflict('Managed paths must not contain links or junctions: ' + relative)
        return result

    def active(self):
        p = self.state_path('active.json')
        return json.loads(p.read_text('utf-8')) if p.exists() else None

    def state_path(self, relative):
        p = self.store / relative
        if p.resolve() != p or not p.resolve().is_relative_to(self.store):
            raise Conflict('State paths must not contain links or junctions')
        return p

    def blob(self, sha):
        if sha is None:
            return None
        if not re.fullmatch('[a-f0-9]{64}', sha):
            raise Conflict('Invalid backup hash')
        value = self.state_path('blobs/' + sha).read_bytes()
        if digest(value) != sha:
            raise Conflict('Backup integrity check failed: ' + sha)
        return value

    def save_blob(self, data):
        sha = digest(data)
        if sha is not None:
            p = self.state_path('blobs/' + sha)
            if not p.exists():
                atomic(p, data)
            elif p.read_bytes() != data:
                raise Conflict('Backup integrity check failed')
        return sha

    def inspect(self):
        exe = self.root / 'src/system/conviction_game.exe'
        asi = self.root / 'src/system/scripts/SplinterCellConviction.FusionFix.asi'
        if digest(read_optional(exe)) != EXE_HASH:
            raise Conflict('Unsupported game executable. This preview targets the inspected Steam build 11503660; no files changed.')
        if digest(read_optional(asi)) != FUSION_HASH:
            raise Conflict('Fusion Fix binary differs from the inspected 04.10.2026 build. Review compatibility before using this preview.')
        if not (self.root / FUSION).is_file():
            raise Conflict('Fusion Fix configuration is missing')

    def check_active(self, active):
        if active:
            if active['root'] != str(self.root):
                raise Conflict('Backup belongs to a different game folder')
            for relative, item in active['files'].items():
                if digest(read_optional(self.target(relative))) != item['installed']:
                    raise Conflict('Changed since installation: ' + relative + '. Preserve/reconcile your edits before switching or uninstalling.')
                self.blob(item['original'])

    def baseline(self, relative, active):
        if active and relative in active['files']:
            value = self.blob(active['files'][relative]['original'])
        else:
            value = read_optional(self.target(relative))
        name = Path(relative).name
        if name not in FILES and value is not None:
            return value
        if name not in FILES:
            raise Conflict('Missing required file: ' + relative)
        try:
            pristine = local_baselines.read_baseline(self.root, name, catalog_path=PACKAGE / 'baselines/hashes.json')
        except (local_baselines.BaselineError, OSError) as error:
            raise Conflict(str(error)) from error
        if value is not None and structure(pristine) - structure(value):
            raise Conflict('Existing override is incomplete or structurally incompatible: ' + relative)
        return pristine if value is None else value

    def check_controls(self):
        catalog = json.loads((PACKAGE / 'compatibility.json').read_text('utf-8'))
        active = self.active()
        if active and active['root'] != str(self.root):
            raise Conflict('Backup belongs to a different game folder')
        for relative, sha in catalog['blacklist_assets'].items():
            candidate = self.root / relative
            data = read_optional(candidate)
            owned = active['files'].get(relative) if active else None
            if relative == AIM_SEQUENCE and owned:
                # Accept only this exact manager-owned repair over the original
                # pinned Fusion bytes. A matching edited asset is never adopted.
                original = self.blob(owned['original'])
                valid = (owned['original'] == sha and
                         digest(data) == owned['installed'] and
                         data in (original, repair_aim_release(original)))
            else:
                valid = digest(data) == sha
            if not candidate.resolve().is_relative_to(self.root) or not valid:
                raise Conflict('Missing or changed Fusion Blacklist control asset: ' + relative)

    def plan(self, modules, fov=95, deadzone=0.15, controls='blacklist', mouse_input=None):
        self.inspect()
        if self.state_path('pending.json').exists():
            raise Conflict('Interrupted transaction found. Run recover first.')
        active = self.active()
        self.check_active(active)
        if mouse_input is None:
            mouse_input = active['selection'].get('mouse_input', 'raw') if active else 'raw'
        if mouse_input not in ('raw', 'standard'):
            raise Conflict('Mouse input must be raw or standard')
        output, changes = {}, []
        def edit(name, section, key, old, new, **kwargs):
            relative = name if '/' in name else 'src/system/update/' + name
            before = output.get(relative)
            if before is None:
                before = self.baseline(relative, active)
            output[relative] = patch(before, section, key, old, new, **kwargs)
            changes.append(dict(file=relative, section=section, key=key, accepted_before=[old,new], new=new,
                                changed=before != output[relative], **kwargs))
        edit('convictioncamera.ini', 'Default', 'FOV', '90', str(fov), field='v')
        edit('convictioncamera.ini', 'Echelon.ECamera', 'm_inputDeadZone', '0.25', str(deadzone))
        for key in ('DisableNegativeMouseAcceleration', 'DisableMouseSmoothing'):
            edit(FUSION, 'GAMEPLAY', key, '0', '1')
        edit(FUSION, 'GAMEPLAY', 'RawMouseInput', '1' if mouse_input == 'standard' else '0',
             '0' if mouse_input == 'standard' else '1')
        # Both documented values are valid inputs; selected style is explicit in the plan.
        cfg = output[FUSION]
        for old in ('0', '1'):
            try:
                output[FUSION] = patch(cfg, 'GAMEPLAY', 'BlacklistControlScheme', old, '1' if controls == 'blacklist' else '0')
                break
            except Conflict:
                if old == '1':
                    raise
        changes.append(dict(file=FUSION, section='GAMEPLAY', key='BlacklistControlScheme', new=controls))
        if controls == 'blacklist':
            self.check_controls()
            before = self.baseline(AIM_SEQUENCE, active)
            output[AIM_SEQUENCE] = repair_aim_release(before)
            changes.append(dict(file=AIM_SEQUENCE, task='Start Narrative Walk',
                                action='eActionType_ReleaseLeftTrigger', new='enabled',
                                changed=before != output[AIM_SEQUENCE]))
        if 'tactical' in modules:
            edit('aiconfig.ini', 'Echelon.EAIManager', 'm_fTimeToGoBackToPatrol', '40.0', '60.0')
            # These arrays encode class by position. Refuse known reorderings instead of
            # applying a Thug change to a different class in a customized override.
            ai = output['src/system/update/aiconfig.ini']
            for n in range(5):
                patch(ai, 'Echelon.EAIManager', 'm_GlobalAIParametersByClass', str(n+1), str(n+1),
                      field='iNbMaxFlank', occurrence=n, count=5)
            for n in range(3):
                edit('aiconfig.ini', 'Echelon.EAIManager', 'm_GlobalAIParametersByClass', '40', '60',
                     field='fSearchActiveDuration', occurrence=n, count=5)
            edit('healthtuning.ini', 'Health_DefaultPlayer', 'RegenRate', '80', '40')
            edit('healthtuning.ini', 'Health_DefaultPlayer', 'RegenDelay', '0', '3')
        if 'deniable' in modules:
            edit('huntermatchmanager.ini', 'EchelonAI.ECoopHunterSpawner', 'm_iDetectionReinforcementSizeSolo', '2', '3')
            edit('laststandmatchmanager.ini', 'Echelon.ELastStandMatchManager', 'm_fTimeBetweenWaves', '25.0f', '15.0f')
        # Remove old Fusion Mod from ASI discovery; its exact bytes stay in the backup store.
        if self.target(LEGACY).exists() or (active and LEGACY in active['files']):
            output[LEGACY] = None
        if active:
            for relative, item in active['files'].items():
                if relative not in output:
                    output[relative] = self.blob(item['original'])
        report = dict(version=VERSION, modules=['comfort'] + sorted(modules), fov=fov,
                      deadzone=deadzone, controls=controls, mouse_input=mouse_input, runtime_validated=False, changes=changes,
                      files={k: {'before': digest(read_optional(self.target(k))), 'after': digest(v)} for k,v in output.items()})
        report['diffs_from_baseline'] = {k: ''.join(difflib.unified_diff(
            self.baseline(k, active).decode('latin-1').splitlines(True), v.decode('latin-1').splitlines(True),
            fromfile=k + ' (baseline)', tofile=k + ' (enhanced)', n=2))
            for k,v in output.items() if v is not None and k != LEGACY}
        return output, report

    @contextlib.contextmanager
    def locked(self):
        self.store.mkdir(parents=True, exist_ok=True)
        lock = self.state_path('operation.lock')
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        except FileExistsError:
            raise Conflict('Another operation is running, or operation.lock remains after a crash. Close other managers before removing that lock.')
        try:
            os.close(fd)
            yield
        finally:
            lock.unlink()

    def transaction(self, output, next_active):
        if game_running():
            raise Conflict('Close Conviction before changing installed files')
        # Persist recovery data before any game file is touched.
        previous = self.active()
        pending = dict(root=str(self.root), previous=previous, files={k: {'before': self.save_blob(read_optional(self.target(k))),
                                                     'after': digest(v)} for k,v in output.items()})
        atomic(self.state_path('pending.json'), json_bytes(pending))
        try:
            for relative, data in output.items():
                self.write_target(relative, data)
            self.write_active(next_active)
            self.state_path('pending.json').unlink()
        except Exception:
            self.recover_unlocked()
            raise

    def write_target(self, relative, data):
        p = self.target(relative)
        if data is None:
            if p.exists():
                p.unlink()
        elif read_optional(p) != data:
            atomic(p, data)

    def write_active(self, active):
        p = self.state_path('active.json')
        if active is None:
            if p.exists():
                p.unlink()
        else:
            atomic(p, json_bytes(active))

    def install(self, modules, **options):
        with self.locked():
            output, report = self.plan(modules, **options)
            previous = self.active()
            records = {}
            for relative, data in output.items():
                original = (previous['files'][relative]['original'] if previous and relative in previous['files']
                            else self.save_blob(read_optional(self.target(relative))))
                if digest(data) != original:
                    records[relative] = dict(original=original, installed=digest(data))
            active = dict(root=str(self.root), version=VERSION, files=records, selection=report)
            self.transaction(output, active)
            return report

    def uninstall(self):
        with self.locked():
            if self.state_path('pending.json').exists():
                raise Conflict('Run recover before uninstalling')
            active = self.active()
            if not active:
                raise Conflict('Michael Works Splinter Cell Conviction Mod is not installed')
            self.check_active(active)
            self.transaction({k: self.blob(v['original']) for k,v in active['files'].items()}, None)

    def recover_unlocked(self):
        if game_running():
            raise Conflict('Close Conviction before recovery')
        p = self.state_path('pending.json')
        if not p.exists():
            raise Conflict('No interrupted transaction found')
        pending = json.loads(p.read_text('utf-8'))
        if pending.get('root') != str(self.root):
            raise Conflict('Recovery records belong to a different game folder')
        restores = {}
        for relative, item in pending['files'].items():
            current = digest(read_optional(self.target(relative)))
            if current not in (item['before'], item['after']):
                raise Conflict('Recovery would overwrite a subsequent edit: ' + relative)
            restores[relative] = self.blob(item['before'])
        for relative, data in restores.items():
            self.write_target(relative, data)
        self.write_active(pending['previous'])
        p.unlink()


def menu(root):
    manager = Manager(root)
    presets = {'1': set(), '2': {'tactical'}, '3': {'deniable'}, '4': {'tactical','deniable'}}
    while True:
        print('\nMichael Works Splinter Cell Conviction Mod ' + VERSION)
        print('1  Comfort (camera and controls only)\n2  Comfort + Tactical stealth\n3  Comfort + Deniable Ops\n4  All modules\n5  Check installation\n6  Restore pre-mod files\n7  Recover interrupted operation\n0  Exit')
        choice = input('Select: ').strip()
        try:
            if choice == '0':
                return 0
            if choice in presets:
                result = manager.install(presets[choice])
                print('Installed: ' + ', '.join(result['modules']))
                print('FOV 95 / deadzone 0.15 / Blacklist-style controls. Restart the game to apply.')
                print('Mouse input: ' + result['mouse_input'])
            elif choice == '5':
                active = manager.active()
                manager.check_active(active)
                print('Installed: ' + ', '.join(active['selection']['modules']) if active else 'Not installed.')
                if manager.state_path('pending.json').exists():
                    print('Interrupted operation found: choose Recover.')
            elif choice == '6':
                manager.uninstall()
                print('Restored exact pre-installation files, including legacy Fusion Mod if present before installation.')
            elif choice == '7':
                with manager.locked():
                    manager.recover_unlocked()
                print('Interrupted operation rolled back.')
        except (Conflict, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            print('STOP:', error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['plan', 'install', 'uninstall', 'status', 'recover', 'menu'])
    parser.add_argument('--game', type=Path, default=PACKAGE.parent)
    parser.add_argument('--modules', nargs='*', choices=['tactical', 'deniable'], default=[])
    parser.add_argument('--fov', type=int, choices=[90,95,100], default=95)
    parser.add_argument('--deadzone', type=float, choices=[0.15,0.2,0.25], default=0.15)
    parser.add_argument('--controls', choices=['blacklist','original'], default='blacklist')
    parser.add_argument('--mouse-input', choices=['raw','standard'], default=None,
                        help='Keep the installed choice by default; standard restores the original mouse movement path')
    parser.add_argument('--output', type=Path, help='Plan only: write complete staged overrides and manifest here')
    args = parser.parse_args()
    try:
        if args.action == 'menu':
            return menu(args.game)
        manager = Manager(args.game)
        options = dict(fov=args.fov, deadzone=args.deadzone, controls=args.controls, mouse_input=args.mouse_input)
        if args.action == 'plan':
            output, report = manager.plan(set(args.modules), **options)
            if args.output:
                dest = args.output.resolve()
                # Never allow staging to act as an unbacked install or overwrite prior output.
                if dest.exists() or dest == manager.root or dest.is_relative_to(manager.root / 'src'):
                    raise Conflict('Plan output must be a new directory outside the live src tree')
                for relative, data in output.items():
                    if data is not None:
                        atomic(dest / relative, data)
                atomic(dest / 'manifest.json', json_bytes(report))
            print(json.dumps(report, indent=2))
        elif args.action == 'install':
            report = manager.install(set(args.modules), **options)
            print('Installed: ' + ', '.join(report['modules']))
            print('FOV:', args.fov, '| deadzone:', args.deadzone, '| controls:', args.controls)
            print('Mouse input:', report['mouse_input'])
            print('Backup and rollback records:', manager.store)
            print('Preview build: gameplay validation remains required.')
        elif args.action == 'uninstall':
            manager.uninstall()
            print('Restored the exact pre-installation files, including legacy Fusion Mod if originally present.')
        elif args.action == 'recover':
            with manager.locked():
                manager.recover_unlocked()
            print('Interrupted operation rolled back.')
        else:
            active = manager.active()
            manager.check_active(active)
            print(json.dumps({'installed': bool(active), 'pending_recovery': manager.state_path('pending.json').exists(),
                              'selection': active['selection'] if active else None}, indent=2))
    except (Conflict, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('STOP:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
