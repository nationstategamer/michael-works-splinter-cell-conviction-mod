"""Import verified configuration baselines from the user's own local dump.

The public package contains hashes only. Full game configuration content stays
in ConvictionEnhanced-state/baselines and is never a distributable dependency.
This module never starts the game, enables dumping, or writes a game override.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import uuid

PACKAGE = Path(__file__).resolve().parent
FILES = ('convictioncamera.ini', 'aiconfig.ini', 'healthtuning.ini',
         'huntermatchmanager.ini', 'laststandmatchmanager.ini')
MAX_BYTES = 2 * 1024 * 1024
REPARSE = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)


class BaselineError(Exception):
    pass


def _absolute(path):
    return Path(os.path.abspath(path))


def _plain(path, *, directory=False, missing=False):
    """Refuse symlinks/junctions in every existing path component."""
    path = _absolute(path)
    parts = list(reversed(path.parents)) + [path]
    for index, part in enumerate(parts):
        try:
            info = part.lstat()
        except FileNotFoundError:
            if missing:
                continue
            raise BaselineError('Local baseline path is missing: ' + part.name) from None
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & REPARSE:
            raise BaselineError('Local baseline paths must not contain symlinks or junctions')
        is_directory = index < len(parts) - 1 or directory
        if ((is_directory and not stat.S_ISDIR(info.st_mode))
                or (not is_directory and not stat.S_ISREG(info.st_mode))):
            raise BaselineError('Local baseline path has an unexpected file type')
    return path


def _read(path):
    path = _plain(path)
    info = path.stat()
    if not 0 < info.st_size <= MAX_BYTES:
        raise BaselineError('Local baseline file is empty or exceeds the supported size: ' + path.name)
    with path.open('rb') as stream:
        data = stream.read(MAX_BYTES + 1)
    _plain(path)
    if not 0 < len(data) <= MAX_BYTES:
        raise BaselineError('Local baseline file changed or exceeds the supported size: ' + path.name)
    return data


def _catalog(catalog_path=None):
    path = Path(catalog_path) if catalog_path is not None else PACKAGE / 'baselines/hashes.json'
    try:
        value = json.loads(_read(path).decode('utf-8'))
    except (UnicodeError, ValueError) as error:
        raise BaselineError('The trusted baseline hash catalog is invalid') from error
    if (not isinstance(value, dict) or set(value) != set(FILES)
            or any(not isinstance(sha, str) or re.fullmatch('[a-f0-9]{64}', sha) is None
                   for sha in value.values())):
        raise BaselineError('The trusted baseline hash catalog is invalid')
    return value


def _cache(game_root):
    game = _plain(game_root, directory=True)
    return _plain(game / 'ConvictionEnhanced-state/baselines', directory=True, missing=True)


def read_baseline(game_root, name, *, catalog_path=None):
    """Return one pristine local baseline; never fall back to public content."""
    if name not in FILES:
        raise BaselineError('Unsupported local baseline name')
    catalog = _catalog(catalog_path)
    try:
        data = _read(_cache(game_root) / name)
    except (OSError, BaselineError) as error:
        raise BaselineError('Local baselines are not ready. Import the five pristine INIs '
                            'from your own Fusion Fix unpacked folder before using presets. '
                            'The core debug menu can be installed without presets.') from error
    if hashlib.sha256(data).hexdigest() != catalog[name]:
        raise BaselineError('Local baseline cache integrity failed: ' + name
                            + '. Preserve the changed cache and import pristine local files again.')
    return data


def baselines_ready(game_root, *, catalog_path=None):
    try:
        for name in FILES:
            read_baseline(game_root, name, catalog_path=catalog_path)
        return True
    except (OSError, BaselineError):
        return False


def import_baselines(game_root, source_dir=None, *, catalog_path=None):
    """Preflight all five local sources, then atomically copy missing cache files.

    ``catalog_path`` is a trusted caller/test injection point, not a user-facing
    option to weaken compatibility. A failed copy may leave only valid private
    cache files; readiness still requires all five, and retry completes safely.
    """
    game = _plain(game_root, directory=True)
    try:
        source = _plain(source_dir if source_dir is not None else game / 'src/system/unpacked',
                        directory=True)
    except (OSError, BaselineError) as error:
        raise BaselineError('Choose a plain local Fusion Fix unpacked folder containing the five '
                            'pristine INIs. Create that dump from your own game first; '
                            'the core debug menu does not require presets.') from error
    cache = _cache(game)
    catalog = _catalog(catalog_path)
    payload = {}
    for name in FILES:
        try:
            data = _read(source / name)
        except (OSError, BaselineError) as error:
            raise BaselineError('Missing or unsafe pristine local baseline: ' + name
                                + '. Choose your complete Fusion Fix unpacked folder; '
                                'do not use _incomplete or _conflicts.') from error
        if hashlib.sha256(data).hexdigest() != catalog[name]:
            raise BaselineError('Local baseline does not match the supported pristine file: ' + name
                                + '. No game overrides were changed.')
        payload[name] = data
    existing = {}
    for name in FILES:
        target = _plain(cache / name, missing=True)
        if target.exists():
            data = _read(target)
            if data != payload[name]:
                raise BaselineError('Existing local baseline cache was changed: ' + name
                                    + '. Preserve it outside the cache before re-importing.')
            existing[name] = data
    cache.mkdir(parents=True, exist_ok=True)
    _plain(cache, directory=True)
    imported = 0
    for name, data in payload.items():
        if name in existing:
            continue
        temporary = cache / ('.import-' + uuid.uuid4().hex + '.tmp')
        try:
            _plain(cache, directory=True)
            with temporary.open('xb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            _plain(cache / name, missing=True)
            os.replace(temporary, cache / name)
            imported += 1
        finally:
            temporary.unlink(missing_ok=True)
    if not baselines_ready(game, catalog_path=catalog_path):
        raise BaselineError('Local baseline import did not pass its final integrity check')
    return {'ready': True, 'files': len(FILES), 'imported': imported,
            'source': str(source), 'cache': str(cache)}
