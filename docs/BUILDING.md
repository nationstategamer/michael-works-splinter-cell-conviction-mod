# Build and test

These commands target a Git source checkout of the repository. The player release
ZIP has an installation layout rather than the complete repository/test layout.

Build on Windows with Python 3.10 or newer, Visual Studio Build Tools with
Desktop development with C++, and a Windows SDK. The native plugin is x86 even
on 64-bit Windows. No Python packages or game installation are required for the
synthetic tests and native build. Vendored libraries include their licenses.

Use a short checkout path, such as `D:\MWMod`. Windows path limits also apply to
temporary preference-file names. Deeply nested test/install paths can prevent
HUD choices from being saved; a save failure is reported without replacing the
previous preferences. Keep generated test directories outside the checkout.

From the repository root:

```powershell
python -m unittest discover -s ConvictionEnhanced/tests -p "test_*.py"
ConvictionEnhanced/native/build.cmd
python tools/run_native_tests.py
python tools/public_audit.py --source
python ConvictionEnhanced/package.py
python tools/public_audit.py --zip ConvictionEnhanced/dist/Michael-Works-Splinter-Cell-Conviction-Mod-0.8.5.zip
```

The build discovers Visual Studio with vswhere, or uses an initialized x86
developer shell. Set `MW_MOD_PYTHON` to a Python executable if it is not on PATH.
The build writes the production ASI, isolated test hosts and a generated
`ConvictionEnhanced/BUILD.json`. Packaging validates the x86 binary, exports and
public payload, and records the release version and binary hash before creating
the ZIP and SHA-256 sidecar.

Public distribution 0.8.5 corrects external Steam launch from the standalone
setup/manager. The native gameplay plugin remains version 0.8.3; its compatibility
and save-protection checks are unchanged. An installer release version does not
imply new game hooks.

Production exports only `InitializeASI`. Test exports belong exclusively to
`overlay_test.dll`; never install or distribute that DLL. Release builds define
NDEBUG to avoid vendor assertion strings containing a builder's local paths.
Explicit runtime compatibility, input and save-protection checks remain active.

## Local rendering verification

Close Conviction before running the D3D9 harness. A Windows desktop with D3D9 is
required. Choose a new, short, disposable test root outside any real game folder:

```powershell
ConvictionEnhanced/native/build/harness.exe ConvictionEnhanced/native/build/overlay_test.dll D:\MWModTest
ConvictionEnhanced/native/build/harness.exe ConvictionEnhanced/native/build/overlay_test.dll D:\MWModTest --verify-preferences
```

The harness uses synthetic camera, session, save and controller data. Its second
process verifies persisted HUD choices. A pass does not establish physical
controller behavior or compatibility with a different game build. Hosted CI runs
the Python and native CPU/host tests, builds the release and audits its contents;
it does not run the graphics harness or claim in-game acceptance.

## Standalone installer/manager

Players on Windows 10/11 x64 can use the public Setup EXE without installing Python. Building that EXE
requires 64-bit Windows Python, including Tcl/Tk, and the exact hash-pinned build
dependencies. The reviewed release and CI use **CPython 3.15.0 x64**; use that
exact runtime to rebuild the public installer. Its Windows x64 package is present
in the official [actions/python-versions manifest](https://github.com/actions/python-versions/blob/main/versions-manifest.json).
Other Python runtime builds have not passed this release's embedded privacy
audit, even when their application-level tests pass. Use a private environment
outside the checkout:

```powershell
python -m venv ../mw-mod-installer-env
& '../mw-mod-installer-env/Scripts/python.exe' -m pip install --require-hashes --only-binary=:all: -r tools/standalone-requirements.txt
& '../mw-mod-installer-env/Scripts/python.exe' tools/build_standalone.py
```

The builder creates the one-file/windowed
`Michael-Works-Splinter-Cell-Conviction-Mod-Setup-0.8.5.exe`, its SHA-256 sidecar,
and a JSON build/audit record in `ConvictionEnhanced/dist`. PyInstaller work and
cache files use an OS temporary directory; an explicit new `--work-dir` is also
available. The installer requests no administrator privileges. Use a user-writable
game folder instead of elevating a one-file application.

Only allowlisted mod data, the reviewed x86 ASI, documentation and license notices
are added as resources. Python modules and the interpreter/Tcl/Tk runtime are
collected by PyInstaller. A resource hash manifest is checked before frozen
operations. The builder inspects the executable archive, nested runtime ZIPs and
Python code objects for private paths/credentials, missing or changed resources,
and forbidden game, test and prerequisite content. These are focused publication
checks, not a general malware certification or a code-signing service.

When requesting Steam, the frozen launcher temporarily clears its bundled DLL
search directory and removes bundled-runtime entries from PATH, uses a normal
window-show command, and restores the manager's runtime environment afterward.
This keeps the external launch separate from the manager's bundled runtime.

To verify a built EXE without touching a game, use a new JSON output path:

```powershell
& './ConvictionEnhanced/dist/Michael-Works-Splinter-Cell-Conviction-Mod-Setup-0.8.5.exe' --self-check '../standalone-self-check.json'
```

The result records the public version, reviewed native version/hash and resource
count. CI builds and runs this check with Python removed from the launched EXE's
PATH. CPython, Tcl/Tk, OpenSSL and PyInstaller notices are bundled and available
in the app's license viewer. The Python runtime/compiler environment affects the
installer's hash; each release publishes its own checksum.

If an official Windows Python installation omits its Tcl/Tk license files, the
builder can use the separately hash-pinned upstream Tcl 8.6.15 and Tk 8.6.15
notices. It checks the actual Tcl patch level and Tk DLL version before selecting
either notice. Other missing-notice versions fail the build; Tcl 9 notices are
never substituted for Tcl 8. Complete local runtime notices remain preferred.

## Optional local integration check

`ConvictionEnhanced/tests/integration_build.py --help` describes a separately
opted-in, isolated-copy check against a user's supported installation. It requires
an explicit game folder, locally extracted pristine baseline folder and a new
work folder. It copies only the required licensed files into that work folder,
checks preset installation/rollback and verifies source hashes. Its private work
files must never be committed or included in a release.

## Publication

Commit source, synthetic fixtures and documentation only. Generated BUILD.json,
binaries, release archives, personal preferences, dumps, saves and test work are
ignored. Audit the Git index before pushing. Release ZIPs include only the
reviewed production ASI and allowlisted management/source/documentation files,
plus a per-file manifest. Prerequisite components and game data are separate.
