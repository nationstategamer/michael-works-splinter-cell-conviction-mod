# Validation and limits

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
