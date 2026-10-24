"""Run CPU/native-host fixtures only; no D3D9 window, game or controller needed."""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TESTS = (
    'controller_state_test', 'free_camera_math_test',
    'exploration_marker_math_test', 'exploration_landing_math_test',
    'free_camera_test', 'gameplay_context_test', 'hud_control_test',
    'interface_preferences_test', 'controller_startup_policy_test',
    'checkpoint_request_test', 'practice_storage_test',
    'checkpoint_archive_test', 'checkpoint_library_test',
    'checkpoint_control_test', 'exploration_policy_test',
    'exploration_control_test',
)


def run(build: Path) -> None:
    with tempfile.TemporaryDirectory(prefix='mw-scc-native-fixtures-') as directory:
        work = Path(directory).resolve()
        # The only recursive cleanup is tempfile's own newly created directory.
        if work.parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('Unexpected fixture directory')
        for name in TESTS:
            executable = build / (name + '.exe')
            if not executable.is_file():
                raise RuntimeError('Build the native test programs first: ' + name)
            subprocess.run([str(executable), str(work)], cwd=work, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path,
                        default=ROOT / 'ConvictionEnhanced/native/build')
    args = parser.parse_args()
    run(args.build.resolve())
    print('All CPU/native-host fixtures passed. D3D9 and physical game checks remain separate.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

