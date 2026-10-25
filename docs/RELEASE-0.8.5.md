# Public release 0.8.5

The player download is
[Michael-Works-Splinter-Cell-Conviction-Mod-Setup-0.8.5.exe](https://github.com/nationstategamer/michael-works-splinter-cell-conviction-mod/releases/download/v0.8.5/Michael-Works-Splinter-Cell-Conviction-Mod-Setup-0.8.5.exe).
It needs Windows 10 or 11 x64 and the supported game/Fusion Fix stack, without a
separate Python installation. See [installation](INSTALLATION.md).

| Published installer identity | Value |
|---|---|
| File size | 15,074,542 bytes |
| SHA-256 | `fcb128e39c29c9331cadaaba2c61817d65a3c6c29f47448960c58c01fea60cab` |
| Tested source commit | `ff3620622c14ca6abc0658339e3ce627f6a26f21` |
| Native plugin | 0.8.3, x86 |
| Native SHA-256 | `2474f590850cce82735972025bc99cf12897f468568a57128c2053de801cb4e0` |

The EXE is the exact artifact used in the completed live test. It was not rebuilt
to incorporate the later installation-documentation edits. The application opens
the current web installation guide; its embedded resource manifest remains the
one checked during that test. Rebuilds from a later documentation snapshot or a
different compiler/runtime environment may have a different hash.

The release includes the EXE's `.sha256` checksum and `.exe.json` build/audit record,
the current installation guide, a separate legacy Python package and its checksum,
and a `SHA256SUMS.txt` inventory for the downloadable assets. GitHub's automatic
source-code downloads do not contain a compiled plugin. Players should choose
the Setup EXE.

The build passed 204 Python tests (three optional skips), the native fixtures and
an actual frozen-parent/native-child environment check. Live acceptance confirmed
PS5 controls, sound and protected saving. [Validation](VALIDATION.md) describes
the evidence, client metadata changes on exit and remaining scope limits.

Copyright (c) 2026 Michael Works. Free personal use under
[LICENSE.txt](../LICENSE.txt); other rights reserved. Third-party licenses remain
separate. Game files, saves and prerequisite binaries are not included.
