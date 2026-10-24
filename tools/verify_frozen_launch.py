"""Build a private frozen host/native probe and verify external-launch isolation.

This never starts Steam or Conviction, and neither test binary is a release file.
Use the same private hash-pinned Python environment as build_standalone.py.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys
import tempfile

import build_standalone

ROOT = Path(__file__).resolve().parents[1]


def visual_studio() -> Path:
    for variable in ('ProgramFiles(x86)', 'ProgramFiles'):
        value = os.environ.get(variable)
        if not value:
            continue
        where = Path(value) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        if where.is_file():
            completed = subprocess.run(
                [str(where), '-latest', '-products', '*', '-requires',
                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                capture_output=True, text=True, check=True)
            path = Path(completed.stdout.strip()) / 'Common7/Tools/VsDevCmd.bat'
            if path.is_file():
                return path
    raise ValueError('Visual Studio x64 C++ build tools were not found')


def verify(work: Path) -> dict:
    build_standalone.require_build_environment()
    if work.exists():
        raise ValueError('Choose a new private test work directory')
    work.mkdir(parents=True)
    probe = work / 'external_launch_probe.exe'
    command = work / 'compile-probe.cmd'
    source = ROOT / 'tools/fixtures/external_launch_probe.cpp'
    command.write_text(
        '@echo off\r\ncall "' + str(visual_studio())
        + '" -no_logo -arch=x64 -host_arch=x64 >nul\r\n'
          'if errorlevel 1 exit /b 1\r\n'
          'cl /nologo /DNDEBUG /std:c++17 /EHsc /O2 /MT /W4 /WX "'
        + str(source) + '" /Fe:"' + str(probe) + '"\r\nexit /b %errorlevel%\r\n', 'utf-8')
    subprocess.run([str(Path(os.environ['SystemRoot']) / 'System32/cmd.exe'),
                    '/d', '/c', str(command)], cwd=work, check=True)
    args = [sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile',
            '--windowed', '--noupx', '--log-level', 'WARN', '--name', 'frozen_external_launch_host',
            '--distpath', str(work / 'dist'), '--workpath', str(work / 'build'),
            '--specpath', str(work), '--paths', str(ROOT / 'ConvictionEnhanced'),
            str(ROOT / 'tools/fixtures/frozen_external_launch_entry.py')]
    environment = dict(os.environ, PYINSTALLER_CONFIG_DIR=str(work / 'cache'))
    subprocess.run(args, cwd=work, env=environment, check=True)
    executable = work / 'dist/frozen_external_launch_host.exe'
    report = work / 'frozen-child-report.json'
    environment = dict(os.environ)
    environment['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32') + ';' + os.environ['SystemRoot']
    environment.pop('PYTHONHOME', None)
    environment.pop('PYTHONPATH', None)
    subprocess.run([str(executable), str(probe), str(report)], cwd=work,
                   env=environment, timeout=60, check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW)
    result = json.loads(report.read_text('utf-8'))
    if result.get('ok') is not True:
        raise ValueError('Frozen external-launch child or parent restoration check failed')
    print(json.dumps(result, indent=2))
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args(argv)
    try:
        if args.work_dir:
            verify(args.work_dir.absolute())
        else:
            # tempfile owns only its new container; verify creates a child in it.
            with tempfile.TemporaryDirectory(prefix='mw-frozen-launch-test-') as directory:
                verify(Path(directory) / 'fixture')
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('Frozen external-launch verification stopped: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
