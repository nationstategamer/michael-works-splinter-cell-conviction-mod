"""Test-only frozen parent; runs an owned probe, never Steam or the game."""
from pathlib import Path
import json
import os
import subprocess
import sys

import external_launch


def main():
    if len(sys.argv) != 3 or not getattr(sys, 'frozen', False):
        return 2
    probe, report = (Path(value).absolute() for value in sys.argv[1:])
    if not probe.is_file() or report.exists() or not report.parent.is_dir():
        return 2
    bundle = Path(sys._MEIPASS)
    original_path = os.environ.get('PATH')
    original_dll = external_launch.current_dll_directory()
    # Reproduce both PyInstaller's DLL search path and a hook-prepended PATH entry.
    os.environ['PATH'] = str(bundle) + ';' + str(bundle / 'synthetic-bin') + ';' + (original_path or '')
    inherited_path = os.environ['PATH']
    try:
        with external_launch.clean_external_environment():
            completed = subprocess.run(
                [str(probe), '--expected-clean-root', str(bundle)],
                capture_output=True, text=True, check=True,
                creationflags=subprocess.CREATE_NO_WINDOW)
        child = json.loads(completed.stdout)
        restored_after_success = (os.environ.get('PATH') == inherited_path
                                  and external_launch.current_dll_directory() == original_dll)
        try:
            with external_launch.clean_external_environment():
                raise RuntimeError('synthetic-child-start-failure')
        except RuntimeError:
            pass
        restored_after_error = (os.environ.get('PATH') == inherited_path
                                and external_launch.current_dll_directory() == original_dll)
        result = {'ok': bool(child['dll_directory_empty'] and not child['path_contains_bundle']
                             and restored_after_success and restored_after_error),
                  'frozen': True, 'child': child,
                  'parent_dll_directory_was_bundle': str(original_dll).casefold() == str(bundle).casefold(),
                  'parent_restored_after_success': restored_after_success,
                  'parent_restored_after_error': restored_after_error}
        if not result['parent_dll_directory_was_bundle']:
            result['ok'] = False
        with report.open('x', encoding='utf-8') as output:
            json.dump(result, output, indent=2)
            output.write('\n')
        return 0 if result['ok'] else 1
    finally:
        if original_path is None:
            os.environ.pop('PATH', None)
        else:
            os.environ['PATH'] = original_path


if __name__ == '__main__':
    raise SystemExit(main())
