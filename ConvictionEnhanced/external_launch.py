"""Keep frozen runtime DLLs out of programs launched through Windows.

The public launcher invokes Steam only after its existing compatibility,
ownership, queue and save-backup checks. This module changes process-local
search state only for that external request, then restores it even on failure.
"""
from __future__ import annotations

import contextlib
import ctypes
from ctypes import wintypes
import ntpath
import os
import sys
import threading

STEAM_URI = 'steam://rungameid/33220'
_launch_lock = threading.RLock()


class LaunchEnvironmentError(OSError):
    pass


def _kernel():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetDllDirectoryW.argtypes = (wintypes.DWORD, wintypes.LPWSTR)
    kernel.GetDllDirectoryW.restype = wintypes.DWORD
    kernel.SetDllDirectoryW.argtypes = (wintypes.LPCWSTR,)
    kernel.SetDllDirectoryW.restype = wintypes.BOOL
    return kernel


def current_dll_directory():
    """Read the process DLL directory without loading code from the bundle."""
    if os.name != 'nt':
        return None
    kernel = _kernel()
    ctypes.set_last_error(0)
    size = kernel.GetDllDirectoryW(0, None)
    if not size:
        if ctypes.get_last_error():
            raise LaunchEnvironmentError('Could not inspect the external launch DLL search path.')
        return None
    if size > 32768:
        raise LaunchEnvironmentError('The external launch DLL search path is invalid.')
    buffer = ctypes.create_unicode_buffer(size + 1)
    ctypes.set_last_error(0)
    count = kernel.GetDllDirectoryW(len(buffer), buffer)
    if not count and not ctypes.get_last_error():
        return None
    if not count or count >= len(buffer):
        raise LaunchEnvironmentError('The external launch DLL search path changed while checking.')
    return buffer.value


def _set_dll_directory(value):
    if not _kernel().SetDllDirectoryW(value):
        raise LaunchEnvironmentError('Could not prepare the external launch DLL search path.')


def _bundle_root():
    if os.name != 'nt' or not getattr(sys, 'frozen', False):
        return None
    value = getattr(sys, '_MEIPASS', None)
    if not isinstance(value, str) or not value or not ntpath.isabs(value):
        raise LaunchEnvironmentError('The bundled runtime directory is invalid; the game was not launched.')
    return value


def _without_bundle_paths(path, bundle):
    """Remove only bundle-anchored entries, preserving all unrelated PATH text."""
    prefix = ntpath.normcase(ntpath.normpath(bundle)).rstrip('\\/')
    retained = []
    for entry in path.split(';'):
        candidate = os.path.expandvars(entry.strip('"'))
        normalized = ntpath.normcase(ntpath.normpath(candidate)) if candidate else ''
        if normalized == prefix or normalized.startswith(prefix + '\\'):
            continue
        retained.append(entry)
    return ';'.join(retained)


@contextlib.contextmanager
def clean_external_environment():
    """Temporarily undo PyInstaller's Windows DLL/PATH preference.

    PyInstaller documents that SetDllDirectoryW affects external child processes.
    Keep this scope small: all launch dependencies must already be imported.
    Source/legacy Python launches already use the ordinary Windows environment.
    """
    bundle = _bundle_root()
    if bundle is None:
        yield
        return
    with _launch_lock:
        previous_directory = current_dll_directory()
        had_path = 'PATH' in os.environ
        previous_path = os.environ.get('PATH', '')
        sanitized_path = _without_bundle_paths(previous_path, bundle)
        # If inspection or reset fails, the callback is never entered.
        _set_dll_directory(None)
        try:
            if had_path:
                os.environ['PATH'] = sanitized_path
            yield
        finally:
            try:
                _set_dll_directory(previous_directory)
            finally:
                if had_path:
                    os.environ['PATH'] = previous_path
                else:
                    os.environ.pop('PATH', None)


def request_steam_launch():
    with clean_external_environment():
        # SW_SHOWNORMAL is explicit; the manager may itself have started hidden.
        os.startfile(STEAM_URI, show_cmd=1)
