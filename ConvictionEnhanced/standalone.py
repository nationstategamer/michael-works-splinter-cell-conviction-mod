"""Standalone installer, manager and protected launcher; no system Python needed."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import queue
import re
import sys
import threading
import webbrowser

import package as release_package
import public_setup
import save_protection
import standalone_manager

PACKAGE = Path(__file__).resolve().parent
VERSION = release_package.DISTRIBUTION_VERSION
GAME_EXE = 'src/system/conviction_game.exe'
GUIDE = ('https://github.com/nationstategamer/michael-works-splinter-cell-conviction-mod/'
         'blob/main/docs/INSTALLATION.md')
HASH = re.compile(r'[a-f0-9]{64}')


def verify_payload(root=PACKAGE):
    """Check all frozen data before any game/save operation; never trust paths."""
    root = Path(os.path.abspath(root))
    manifest = root / 'STANDALONE-PAYLOAD.json'
    if not public_setup.plain_file(manifest) or manifest.stat().st_size > 128 * 1024:
        raise public_setup.SetupError('The installer payload is incomplete. Download a fresh official copy.')
    info = json.loads(manifest.read_text('utf-8'))
    if (not isinstance(info, dict) or set(info) != {'schema', 'distribution_version', 'native_sha256', 'files'}
            or info['schema'] != 1 or info['distribution_version'] != VERSION
            or not isinstance(info['files'], dict) or not 4 <= len(info['files']) <= 256
            or not isinstance(info['native_sha256'], str)
            or HASH.fullmatch(info['native_sha256']) is None):
        raise public_setup.SetupError('The installer payload record is invalid.')
    required = {'BUILD.json', 'compatibility.json', 'baselines/hashes.json',
                'native/build/ConvictionEnhanced.asi', 'LICENSE.txt'}
    if not required.issubset(info['files']):
        raise public_setup.SetupError('The installer is missing required resources.')
    total = 0
    for name, digest in info['files'].items():
        if not isinstance(name, str) or not isinstance(digest, str) or HASH.fullmatch(digest) is None:
            raise public_setup.SetupError('The installer contains an invalid resource record.')
        relative = PurePosixPath(name)
        if (not relative.parts or relative.is_absolute() or '..' in relative.parts
                or ':' in name or '\\' in name or name != relative.as_posix()):
            raise public_setup.SetupError('The installer contains an unsafe resource path.')
        path = root / name
        if (not public_setup.plain_file(path) or path.stat().st_size > 16 * 1024 * 1024):
            raise public_setup.SetupError('A required installer resource is missing or unsafe.')
        data = path.read_bytes()
        total += len(data)
        if total > 64 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != digest:
            raise public_setup.SetupError('Installer integrity check failed. Download a fresh official copy.')
    build = json.loads((root / 'BUILD.json').read_text('utf-8'))
    if (not isinstance(build, dict) or build.get('schema') != 1
            or build.get('overlay_version') != release_package.OVERLAY_VERSION
            or build.get('production_exports') != ['InitializeASI']
            or build.get('sha256') != info['native_sha256']):
        raise public_setup.SetupError('The installer and plugin identities disagree.')
    # Validate actual production architecture/exports, not just a file digest.
    data = (root / 'native/build/ConvictionEnhanced.asi').read_bytes()
    public_setup.overlay_install.validate_pe(data)
    if (hashlib.sha256(data).hexdigest() != info['native_sha256']
            or release_package.production_exports(data) != ['InitializeASI']):
        raise public_setup.SetupError('The installer plugin is not a production build.')
    return {'ok': True, 'distribution_version': VERSION,
            'overlay_version': build.get('overlay_version'),
            'production_sha256': info['native_sha256'], 'payload_files': len(info['files'])}


def _steam_roots():
    values = []
    try:
        import winreg
        keys = ((winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam', 'SteamPath'),
                (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath'))
        for hive, key, value in keys:
            try:
                with winreg.OpenKey(hive, key) as opened:
                    text, kind = winreg.QueryValueEx(opened, value)
                if kind in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) and text:
                    values.append(Path(os.path.expandvars(text)))
            except OSError:
                pass
    except ImportError:
        pass
    for variable in ('ProgramFiles(x86)', 'ProgramFiles'):
        value = os.environ.get(variable)
        if value:
            values.append(Path(value) / 'Steam')
    return values


def _small_text(path, maximum):
    try:
        if path.is_file() and path.stat().st_size <= maximum:
            return path.read_text('utf-8', errors='replace')
    except OSError:
        pass
    return ''


def discover_games(executable=None, steam_roots=None):
    """Read Steam manifests and the installed manager location, without writes."""
    binary = Path(executable or (sys.executable if getattr(sys, 'frozen', False) else __file__))
    installed = binary.parent.parent
    if binary.parent.name.casefold() == 'convictionenhanced' and (installed / GAME_EXE).is_file():
        return [installed]
    libraries = list(_steam_roots() if steam_roots is None else steam_roots)
    for steam in list(libraries):
        text = _small_text(Path(steam) / 'steamapps/libraryfolders.vdf', 1024 * 1024)
        libraries.extend(Path(value.replace('\\\\', '\\'))
                         for value in re.findall(r'"path"\s*"([^"\r\n]+)"', text))
    found = {}
    for library in libraries:
        apps = Path(library) / 'steamapps'
        text = _small_text(apps / 'appmanifest_33220.acf', 64 * 1024)
        match = re.search(r'"installdir"\s*"([^"\r\n]+)"', text)
        if match and not any(value in match[1] for value in ('/', '\\', ':')) and match[1] not in ('.', '..'):
            game = apps / 'common' / match[1]
            if (game / GAME_EXE).is_file():
                game = Path(os.path.abspath(game))
                found[os.path.normcase(str(game))] = game
    return list(found.values())


def friendly_error(error, save_source=None):
    if isinstance(error, OSError):
        return ('Windows could not access a required file. Close other copies of this manager '
                'and check that you can write to the game folder. Your save backups are retained.')
    text = str(error) or 'The operation could not finish. No original saves were replaced.'
    text = text.replace('with enhanced.py recover', 'using Recover interrupted setup')
    if save_source:
        text = text.replace(str(save_source), '<selected save folder>')
    profile = os.environ.get('USERPROFILE')
    if profile:
        text = text.replace(profile, '<user folder>')
    text = re.sub(r'(?i)(savegames[\\/])[^\\/\r\n]+', r'\1<account>', text)
    return text


def write_check(path, result):
    """Self-check writes only a caller-selected new JSON report, never a game."""
    target = Path(os.path.abspath(path))
    if target.suffix.casefold() != '.json':
        raise ValueError('Self-check requires a new .json report path.')
    save_protection._safe_directory(target.parent)
    with target.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')


class ManagerWindow:
    def __init__(self, initial_game=None, initial_save=None):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.root = tk.Tk()
        self.root.title('Michael Works Splinter Cell Conviction Mod')
        self.root.geometry('830x700')
        self.root.minsize(770, 650)
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        style = ttk.Style(self.root)
        style.configure('TButton', padding=(10, 7))
        style.configure('Title.TLabel', font=('Segoe UI', 18, 'bold'))
        style.configure('TLabel', font=('Segoe UI', 10))
        self.events = queue.Queue()
        self.busy = False
        self.controls = []
        games = discover_games() if initial_game is None else []
        self.game = tk.StringVar(value=str(initial_game or (games[0] if len(games) == 1 else '')))
        self.save = tk.StringVar(value=str(initial_save or ''))
        self.baseline = tk.StringVar()
        self.presets = tk.BooleanVar(value=False)
        self.tactical = tk.BooleanVar(value=False)
        self.deniable = tk.BooleanVar(value=False)

        body = ttk.Frame(self.root, padding=20)
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=1)
        ttk.Label(body, text='Michael Works', style='Title.TLabel').grid(row=0, sticky='w')
        ttk.Label(body, text='Splinter Cell Conviction Mod  |  Setup ' + VERSION).grid(row=1, sticky='w', pady=(2, 16))
        paths = ttk.Frame(body)
        paths.grid(row=2, sticky='ew')
        paths.columnconfigure(0, weight=1)
        self.path_row(paths, 0, 'Game folder', self.game, 'Choose the Conviction game folder')
        self.path_row(paths, 2, 'Save folder (optional; found automatically when possible)', self.save,
                      'Choose your Ubisoft savegames / account / 2 folder')
        ttk.Label(body, text='Fusion Fix is checked before changes. Your saves are backed up before install and every launch.',
                  wraplength=760).grid(row=3, sticky='w', pady=(12, 10))
        actions = ttk.Frame(body)
        actions.grid(row=4, sticky='ew')
        for column, (label, action) in enumerate((('Check installation', 'check'), ('Install / update', 'install'),
                                                  ('Launch game', 'launch'), ('Remove mod', 'remove'))):
            button = ttk.Button(actions, text=label, command=lambda selected=action: self.start(selected))
            button.grid(row=0, column=column, padx=(0, 8), sticky='ew')
            actions.columnconfigure(column, weight=1)
            self.controls.append(button)

        optional = ttk.LabelFrame(body, text='Optional configuration presets', padding=10)
        optional.grid(row=5, sticky='ew', pady=(14, 10))
        optional.columnconfigure(0, weight=1)
        base = ttk.Checkbutton(optional, text='Use camera and control presets', variable=self.presets,
                               command=self.preset_master_changed)
        base.grid(row=0, column=0, sticky='w')
        self.controls.append(base)
        flags = ttk.Frame(optional)
        flags.grid(row=1, column=0, sticky='w', pady=(4, 6))
        for column, (text, variable) in enumerate((('Tactical stealth', self.tactical), ('Deniable Ops', self.deniable))):
            widget = ttk.Checkbutton(flags, text=text, variable=variable,
                                     command=lambda: self.presets.set(True))
            widget.grid(row=0, column=column, padx=(0, 20))
            self.controls.append(widget)
        self.path_row(optional, 2, 'Local baseline folder (optional presets only)', self.baseline,
                      'Choose your locally extracted pristine baseline INIs')
        ttk.Label(optional, text='Preset changes use files from your own game. Existing presets are kept when this section is unused.',
                  wraplength=730).grid(row=4, column=0, columnspan=2, sticky='w', pady=(5, 0))

        self.status = tk.Text(body, height=6, wrap='word', font=('Segoe UI', 10), relief='flat',
                              background='#f0f3f6', padx=12, pady=10, state='disabled')
        self.status.grid(row=6, sticky='nsew', pady=(0, 10))
        body.rowconfigure(6, weight=1)
        foot = ttk.Frame(body)
        foot.grid(row=7, sticky='ew')
        foot.columnconfigure(0, weight=1)
        ttk.Label(foot, text='Copyright (c) 2026 Michael Works. Free personal use; other rights reserved.',
                  wraplength=500).grid(row=0, column=0, sticky='w')
        ttk.Button(foot, text='Installation guide', command=lambda: webbrowser.open(GUIDE)).grid(row=0, column=1)
        ttk.Button(foot, text='License and notices', command=self.licenses).grid(row=1, column=0, sticky='w', pady=(8, 0))
        recover = ttk.Button(foot, text='Recover interrupted setup', command=lambda: self.start('recover'))
        recover.grid(row=1, column=1, pady=(8, 0), sticky='e')
        self.controls.append(recover)
        self.say('Choose your game folder, then check the installation. Install / update keeps the tested Fusion Fix and controller settings.')
        self.root.after(100, self.poll)

    def path_row(self, frame, row, label, variable, title):
        ttk = self.ttk
        ttk.Label(frame, text=label).grid(row=row, column=0, columnspan=2, sticky='w', pady=(6, 3))
        entry = ttk.Entry(frame, textvariable=variable)
        entry.grid(row=row + 1, column=0, sticky='ew', padx=(0, 8))
        button = ttk.Button(frame, text='Browse...', command=lambda: self.browse(variable, title))
        button.grid(row=row + 1, column=1)
        self.controls.extend((entry, button))

    def browse(self, variable, title):
        from tkinter import filedialog
        result = filedialog.askdirectory(parent=self.root, title=title, mustexist=True)
        if result:
            variable.set(result)

    def preset_master_changed(self):
        if not self.presets.get():
            self.tactical.set(False)
            self.deniable.set(False)

    def licenses(self):
        from tkinter import scrolledtext
        window = self.tk.Toplevel(self.root)
        window.title('Michael Works Mod - License and notices')
        window.geometry('820x600')
        text = scrolledtext.ScrolledText(window, wrap='word', font=('Segoe UI', 10), padx=15, pady=15)
        text.pack(fill='both', expand=True)
        names = ('LICENSE.txt', 'THIRD-PARTY-NOTICES.md', 'runtime-licenses/ALL-NOTICES.txt',
                 'native/vendor/imgui/LICENSE.txt', 'native/vendor/minhook/LICENSE.txt',
                 'native/vendor/miniz/LICENSE')
        for name in names:
            path = PACKAGE / name
            if path.is_file():
                text.insert('end', name + '\n\n' + path.read_text('utf-8', errors='replace') + '\n\n')
        text.configure(state='disabled')

    def say(self, text):
        self.status.configure(state='normal')
        self.status.delete('1.0', 'end')
        self.status.insert('1.0', text)
        self.status.configure(state='disabled')

    def start(self, action):
        from tkinter import messagebox
        if self.busy:
            return
        if not self.game.get().strip():
            self.say('Choose the Conviction game folder first. It contains the src folder.')
            return
        if action == 'remove' and not messagebox.askyesno(
                'Remove Michael Works mod?',
                'Remove the owned mod files and restore their recorded originals?\n\n'
                'Your saves, backups and this manager will be kept.', parent=self.root):
            return
        game = Path(self.game.get().strip())
        source = Path(self.save.get().strip()) if self.save.get().strip() else None
        baseline = Path(self.baseline.get().strip()) if self.baseline.get().strip() else None
        presets = (['comfort'] + (['tactical'] if self.tactical.get() else [])
                   + (['deniable'] if self.deniable.get() else [])) if self.presets.get() else None
        self.busy = True
        for control in self.controls:
            control.configure(state='disabled')
        self.say({'check': 'Checking game, Fusion Fix, installer and save protection...',
                  'install': 'Preparing save protection and installing the mod...',
                  'launch': 'Checking the mod and preparing a fresh save backup before launch...',
                  'remove': 'Removing the owned mod files and retaining your saves and backups...',
                  'recover': 'Checking recorded interrupted operations...'}[action])

        def work():
            try:
                if getattr(sys, 'frozen', False):
                    verify_payload()
                if action == 'check':
                    result = standalone_manager.inspect(game, source)
                elif action == 'install':
                    if not getattr(sys, 'frozen', False):
                        raise public_setup.SetupError('Use the built standalone EXE to install the persistent manager.')
                    result = standalone_manager.install(game, Path(sys.executable), source, baseline, presets)
                elif action == 'launch':
                    result = standalone_manager.launch(game, source)
                elif action == 'remove':
                    result = standalone_manager.remove(game)
                else:
                    result = standalone_manager.recover(game)
                self.events.put((action, result, None))
            except Exception as error:
                self.events.put((action, None, friendly_error(error, source)))
        threading.Thread(target=work, name='MichaelWorksSetup', daemon=False).start()

    def poll(self):
        try:
            action, result, error = self.events.get_nowait()
        except queue.Empty:
            pass
        else:
            self.busy = False
            for control in self.controls:
                control.configure(state='normal')
            if error:
                self.say(error)
            elif action == 'check':
                issues = result.get('issues', [])
                self.say(('Checks passed. The supported Fusion Fix is present and save protection is ready. '
                          'You can install or launch the mod.') if result.get('ready_for_install')
                         else '\n\n'.join(issues) or 'Exit Conviction normally before changes.')
            elif action == 'install':
                self.say('Installation complete. Use Launch game here. For future launches, open '
                         'ConvictionEnhanced / Michael Works Mod.exe in your game folder. '
                         'Your controller settings and original saves are preserved.')
            elif action == 'launch':
                self.say('Save protection is ready and launch was requested through Steam. '
                         'In the game, pause with Options, hold L3 + R3, then release to open the mod menu.')
            elif action == 'remove':
                self.say('The owned mod files were removed and recorded originals restored. '
                         'Your saves and backups are retained. This manager stays available for reinstall; '
                         'you may delete its EXE normally after closing it.')
            else:
                self.say('Recorded setup recovery completed. Check the installation before continuing.')
        self.root.after(100, self.poll)

    def close(self):
        from tkinter import messagebox
        if self.busy:
            messagebox.showinfo('Setup is working', 'Please let the current step finish before closing.', parent=self.root)
            return
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def gui_self_check(game):
    """Exercise this app's own Tk controls on an empty fixture, never a game."""
    if game is None:
        raise ValueError('The GUI self-check requires an explicit empty test folder.')
    game = Path(os.path.abspath(game))
    save_protection._safe_directory(game)
    if any(game.iterdir()):
        raise ValueError('The GUI self-check refuses a nonempty folder.')
    window = ManagerWindow(game)
    report = {'ok': False, 'distribution_version': VERSION}
    ticks = 0

    def finish(error=None):
        if error:
            report['error'] = friendly_error(error)
        window.root.destroy()

    def completed():
        nonlocal ticks
        ticks += 1
        if window.busy and ticks < 200:
            window.root.after(100, completed)
            return
        try:
            if window.busy:
                raise ValueError('The read-only GUI check did not finish.')
            status = window.status.get('1.0', 'end')
            report['unsupported_folder_refused'] = 'actual Conviction game folder' in status
            window.licenses()
            notices = [child for child in window.root.winfo_children() if child.winfo_class() == 'Toplevel']
            def contents(widget):
                text = widget.get('1.0', 'end') if widget.winfo_class() == 'Text' else ''
                return text + ''.join(contents(child) for child in widget.winfo_children())
            text = ''.join(contents(child) for child in notices)
            report['license_viewer'] = 'Michael Works' in text and ('Python' in text or not getattr(sys, 'frozen', False))
            for child in notices:
                child.destroy()
            report['empty_test_folder_unchanged'] = not any(game.iterdir())
            report['ok'] = all(report.get(key) for key in ('window_visible', 'controls_fit', 'unsupported_folder_refused',
                                                          'license_viewer', 'empty_test_folder_unchanged'))
            finish()
        except Exception as error:
            finish(error)

    def begin():
        try:
            window.root.update_idletasks()
            width, height = window.root.winfo_width(), window.root.winfo_height()
            x, y = window.root.winfo_rootx(), window.root.winfo_rooty()
            report['window_visible'] = bool(window.root.winfo_viewable())
            report['controls_fit'] = all(0 <= control.winfo_rootx() - x
                and 0 <= control.winfo_rooty() - y
                and control.winfo_rootx() - x + control.winfo_width() <= width
                and control.winfo_rooty() - y + control.winfo_height() <= height
                for control in window.controls)
            report['controls_checked'] = len(window.controls)
            window.tactical.set(True)
            window.deniable.set(True)
            window.presets.set(False)
            window.preset_master_changed()
            if window.tactical.get() or window.deniable.get():
                raise ValueError('Disabled preset selection did not clear its child choices.')
            window.start('check')
            window.root.after(100, completed)
        except Exception as error:
            finish(error)

    window.root.after(150, begin)
    window.run()
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--save-source', type=Path)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--self-check', type=Path, metavar='NEW_JSON_REPORT')
    action.add_argument('--ui-self-check', type=Path, metavar='NEW_JSON_REPORT',
                        help='Test the manager UI only against an explicit empty --game folder.')
    action.add_argument('--launch', action='store_true', help='Prepare saves and launch without opening the manager.')
    args = parser.parse_args(argv)
    if args.self_check or args.ui_self_check:
        try:
            if getattr(sys, 'frozen', False) or args.self_check:
                result = verify_payload()
            if args.ui_self_check:
                result = gui_self_check(args.game)
            code = 0 if result['ok'] else 1
        except Exception as error:
            result = {'ok': False, 'error': friendly_error(error)}
            code = 1
        try:
            write_check(args.self_check or args.ui_self_check, result)
        except (OSError, ValueError, save_protection.SaveProtectionError):
            return 1
        return code
    try:
        if getattr(sys, 'frozen', False):
            verify_payload()
        if args.launch:
            games = discover_games() if args.game is None else [args.game]
            if len(games) != 1:
                raise public_setup.SetupError('Choose the intended game folder in the manager before launching.')
            standalone_manager.launch(games[0], args.save_source)
        else:
            ManagerWindow(args.game, args.save_source).run()
        return 0
    except Exception as error:
        from tkinter import messagebox
        messagebox.showerror('Michael Works Mod', friendly_error(error, args.save_source))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
