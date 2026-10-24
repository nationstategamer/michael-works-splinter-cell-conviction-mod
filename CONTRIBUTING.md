# Contributing

Please open an issue describing the supported game/Fusion versions, solo mode,
steps to reproduce, expected behavior and observed behavior. Do not attach save
files, game archives, account identifiers, private paths or unreviewed full logs.
A short redacted diagnostic excerpt is usually enough.

Keep changes focused. Preserve the exact-build checks, controller ownership/release
barriers, collision checks, original-save protections and ownership-based rollback.
Do not broaden game compatibility from a version label alone. Add meaningful
isolated regressions for behavior changes and keep GPU/physical play testing separate
from headless CI claims.

Use synthetic fixtures in tests. Do not add game-derived baseline INIs, game
scripts, save payloads, binary dependencies, extracted dumps or personal state.
Original contributions are submitted under the project's personal-use license
unless agreed otherwise. Vendor code keeps its own license.
