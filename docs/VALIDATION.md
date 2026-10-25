# Validation and limits

## Standalone launch correction 0.8.5

The standalone manager now temporarily clears its bundled Windows DLL directory
and removes only bundle-owned PATH entries when requesting Steam. It explicitly
requests a normal window, then restores the manager's DLL directory and PATH even
if the request fails. Compatibility, ownership, queue and per-launch save-backup
checks still run before the external request. The native 0.8.3 plugin is unchanged.

- 204 Python tests passed locally, with three optional symlink checks skipped.
  Eight new tests cover the external-launch boundary, real Windows DLL-directory
  state, exact restoration on failure, and refusing Steam when inspection or
  environment preparation fails.
- A separate one-file frozen test host launched an owned native x64 probe. The
  actual child saw an empty custom DLL directory and no bundle-owned PATH entry;
  the parent restored its exact prior state after success and an injected error.
  Neither fixture launches Steam, touches a game, nor ships in the player EXE.
- The source launcher keeps its ordinary environment. The existing test that
  blocks Steam when save preparation fails still passes.

In the live 0.8.4 upgrade check, the installed plugin/manager matched their release
hashes, Fusion Fix and controller files were unchanged, and PS5 movement, aiming,
L2 release and debug-menu navigation were accepted. Intermittent launches did not
leave a visible game window; a clean Steam request opened it. This evidence and
the documented frozen-runtime inheritance motivated the correction; the old
graphics-probe message alone does not establish the cause of a game exit.

See [PyInstaller's external-program guidance](https://pyinstaller.org/en/stable/common-issues-and-pitfalls.html#launching-external-programs-from-the-frozen-application).
The isolated child test establishes environment handling, not a guarantee that
Steam/Ubisoft completes each game launch. A launch retry was still needed in the
live session below; the installer reports that a request was sent to Steam.

### Live acceptance on 25 October 2026

The installed 0.8.5 manager matched the exact published installer:
`fcb128e39c29c9331cadaaba2c61817d65a3c6c29f47448960c58c01fea60cab`
(15,074,542 bytes). The native ASI retained the 0.8.3 fingerprint below. The loaded
Fusion Fix, loader and Xidi modules matched the supported fingerprints, and all
ten compared configuration/controller files stayed unchanged during this session.

PS5 movement, aiming, L2 hold/release and menu navigation were accepted in the
upgrade test. In the resumed campaign check, saving a return point worked after
letting the character settle for three seconds in unpaused gameplay before pausing.
The saved preference to hide the free-camera hint remained off.

The user confirmed sound in gameplay and after returning to the main menu. A
20-second read-only audio trace found consistent, unmuted output through the
intended playback device in every sample. Earlier audio had been routed to a
different output when that device was unavailable; no global sound settings or
game audio code were changed by the installer.

The protected-save check used separate coherent snapshots after loading and after
activation. It did not republish the practice-source marker while the game ran.
With isolation installed, active and healthy, the game completed one redirected
save on returning to its main menu. All five original files were still exactly
unchanged, with no additions or removals, while the process remained open.

After normal exit, all three gameplay save payloads remained unchanged. Two
containers changed only their 32-byte ASCII-hex metadata field at offsets 8-39:
lengths, the supported header, and all bytes from offset 40 onward were identical.
Their two empty upload markers were removed. The third container was byte-identical.
No original saves were restored or overwritten by the test helpers, and verified
private backups were retained. Account directories, saved data and raw recordings
are not published.

This is one campaign acceptance session on the supported installation, not a
certification of every arena, device or full playthrough. The published EXE is
retained unchanged from tested source commit
`ff3620622c14ca6abc0658339e3ce627f6a26f21`; the release's later documentation-only
commit records the completed test and revised player instructions.

## Standalone setup 0.8.4

The recommended download is now a self-contained Windows 10/11 x64 installer/manager.
Players do not install Python. The native x86 plugin remains the reviewed 0.8.3
binary below; gameplay, controller, Fusion hooks and save-isolation code are
unchanged by this installer update.

- 196 Python tests passed, with three optional symlink checks skipped on this host.
  These include the existing backend suites and 54 new installer, ownership,
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
