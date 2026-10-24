# Validation and limits

## Standalone setup 0.8.4

The recommended download is now a self-contained Windows 10/11 x64 installer/manager.
Players do not install Python. The native x86 plugin remains the reviewed 0.8.3
binary below; gameplay, controller, Fusion hooks and save-isolation code are
unchanged by this installer update.

- 194 Python tests passed, with three optional symlink checks skipped on this host.
  These include the existing backend suites and 52 new installer, ownership,
  recovery, discovery, payload and build-boundary tests.
- Synthetic end-to-end installs prepared original-save backups before installing
  the core, copied only the owned manager, and retained originals/backups on
  removal. Account selection is remembered privately and revalidated on launch.
- Ownership conflicts refused replacement. Failed manager updates rolled back
  exactly; interrupted journals reject corrupt backups or external edits. The
  standalone operation lock was tested across processes and abrupt process exit.
- The frozen EXE passed its resource/plugin integrity check with PATH restricted
  to Windows and an invalid PYTHONHOME. It uses its bundled runtime.
- The application's Tk self-test checked a mapped window, 14 control bounds,
  the license viewer and read-only refusal of an empty unsupported game folder.
  The test folder remained empty. This is an application UI harness check;
  independent desktop automation was unavailable in this session.
- The build inspects embedded resources, Python code paths/constants and nested
  runtime ZIPs for private paths, credentials and excluded game/test data. Exact
  executable hash, runtime version and payload counts are published in its
  checksum/build-metadata sidecars.

These installer checks use isolated synthetic folders, not the working game or
original save account. The existing 12-file protected installation baseline still
matched after development. GitHub CI repeats tests/builds, payload checks and the
empty-folder UI harness. A clean-PC or fresh physical-controller acceptance test
is separate from the preserved gameplay evidence below.

## Public preview 0.8.3

Public preparation removes distributed game baselines, adds clean setup and
per-launch save preparation, makes builds portable and removes embedded local
assertion paths. It retains the 0.8.2 gameplay, HUD, camera, controller ownership
and save-protection backends. Settings explains core-only/preset availability.

The prepared Windows public build passed:

- 142 Python tests, with three optional symlink checks skipped on this host.
- 16 native CPU/host suites with 6,214 checks; four optional reparse checks skipped.
- Eleven focused publication-audit tests (included in the Python total).
- A 969-presentation D3D9 harness, followed by successful HUD preference recovery
  in a separate process, using a short isolated test path.
- An isolated integration round trip using the locally supported game assets:
  preset installation, switching and exact rollback passed, and source files
  remained unchanged.

Production ASI SHA-256:
`2474f590850cce82735972025bc99cf12897f468568a57128c2053de801cb4e0`.
It exports only `InitializeASI`; no test entry points or absolute builder paths
were found. Archive and per-file hashes are supplied with each release. Builds
from a different compiler environment may have different binary hashes.

These checks do not constitute a clean-PC or physical-game acceptance test of
every public dependency combination. The gameplay acceptance below belongs to
the earlier build and supported installation where it was recorded.

## Prior gameplay acceptance

The underlying 0.8.2 build passed 413 Exploration backend, 60 landing-math, 257
policy, 143 controller, 143 marker-math, 108 camera, 67 context, 221 storage,
450 checkpoint/part and 94 Python checks, plus a 967-presentation local D3D9
harness and separate-process HUD preference verification.

Solo Infiltration and Last Stand were manually accepted on C01_MafiaClub: marker,
teleport, return and PS5 movement/aim worked. Last Stand captured a standing anchor
and successful crouched return on its floor. Infiltration's reported stance checks
had one independently recorded round trip. Earlier campaign/Hunter acceptance
belongs to their respective earlier builds.

Original save containers matched while practice was active. Normal exit preserved
native gameplay payloads; client metadata/upload-marker finalization differed.
No original save data or private runtime recordings are published.

Other arenas, full matches/every wave, co-op/network, split screen, Proton and
every controller device were not separately certified. Hosted CI runs CPU/installer
tests and does not claim a working game, GPU rendering or physical PS5 acceptance.
