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
python tools/public_audit.py --zip ConvictionEnhanced/dist/Michael-Works-Splinter-Cell-Conviction-Mod-0.8.3.zip
```

The build discovers Visual Studio with vswhere, or uses an initialized x86
developer shell. Set `MW_MOD_PYTHON` to a Python executable if it is not on PATH.
The build writes the production ASI, isolated test hosts and a generated
`ConvictionEnhanced/BUILD.json`. Packaging validates the x86 binary, exports and
public payload, and records the release version and binary hash before creating
the ZIP and SHA-256 sidecar.

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
