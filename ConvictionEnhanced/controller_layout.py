"""Back up and select the game's Xbox layout for Steam Input; does not enable Steam Input."""
import argparse
import json
import os
from pathlib import Path
import enhanced as ce


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['apply', 'restore'])
    args = parser.parse_args()
    target = Path(os.environ['PROGRAMDATA']) / 'Ubisoft/Conviction/ConvictionUserPC.ini'
    backup = ce.PACKAGE.parent / 'ConvictionEnhanced-state' / 'controller-layout'
    record = backup / 'record.json'
    manager = ce.Manager(ce.PACKAGE.parent)
    with manager.locked():
        if ce.game_running():
            raise ce.Conflict('Close Conviction before changing its controller layout')
        current = target.read_bytes()
        saved = json.loads(record.read_text('utf-8')) if record.exists() else None
        if saved and saved['path'] != str(target):
            raise ce.Conflict('Backup belongs to a different settings file')
        if args.action == 'restore':
            if not saved:
                raise ce.Conflict('No controller-layout backup exists')
            original = (backup / 'original.ini').read_bytes()
            if ce.digest(original) != saved['before'] or ce.digest(current) != saved['after']:
                raise ce.Conflict('Settings changed since the fix; reconcile them before restoring')
            ce.atomic(target, original)
            record.unlink()
            print('Restored controller-layout settings exactly.')
        else:
            if saved:
                if ce.digest(current) == saved['after']:
                    print('Xbox controller layout is already selected.')
                    return
                raise ce.Conflict('Settings changed since the fix; no files changed')
            updated = ce.patch(current, 'Inputs', 'ControllerType', 'KT_LOGITECH', 'KT_X360')
            if updated == current:
                print('Xbox controller layout is already selected.')
                return
            ce.atomic(backup / 'original.ini', current)
            ce.atomic(record, ce.json_bytes(dict(path=str(target), before=ce.digest(current), after=ce.digest(updated))))
            ce.atomic(target, updated)
            print('Selected KT_X360; every other setting preserved. Backup:', backup)


if __name__ == '__main__':
    try:
        main()
    except (ce.Conflict, OSError, ValueError, KeyError) as error:
        raise SystemExit('STOP: ' + str(error))
