# Michael Works Splinter Cell Conviction Mod

A free personal-use Windows mod by **Michael Works**, with a controller-friendly
debug menu, camera tools, interface controls and protected solo practice.

**Public preview 0.8.3.** This is a community mod for a specific supported game
and Fusion Fix build. It is independent of Ubisoft and the prerequisite authors.
Download the compiled ZIP from this repository's **Releases** section. GitHub's
source-code ZIP is for building and does not contain the compiled mod.

## Features

- PS5 menu navigation through Steam Input/XInput: L3 + R3 opens the menu;
  D-pad/sticks navigate, Cross selects, Circle returns, and L1/R1 change pages.
- Free camera, persistent HUD choices and controller startup recovery.
- Exploration with checked landing markers, teleport and a saved return point.
- Floor-relative returns that accommodate standing or crouching when there is room.
- Protected campaign checkpoint/mission-part practice on supported recorded data.
- Optional camera/control, Tactical and Deniable Ops presets using locally imported
  configuration baselines from the user's own game.

## Install

1. Install your licensed Steam copy of Conviction and the supported official
   Fusion Fix bundle, which includes its matching loader and Xidi controller
   components. See [compatibility and download](docs/COMPATIBILITY.md).
2. Exit the game. Extract the release's **ConvictionEnhanced** folder directly into
   your Conviction game folder, beside the existing `src` directory.
3. Install Python 3.10 or newer if needed, then run **ConvictionEnhanced/Install.cmd**.
   Setup checks compatibility, installs the owned mod ASI and prepares a private
   original-save backup. If it finds more than one save account, select the intended
   save directory explicitly as explained in the [installation guide](docs/INSTALLATION.md).
4. For PS5, enable Steam Input for the game and enable **Use Controller** in its
   own settings. The mod respects a saved controller-off choice.
5. Start through **ConvictionEnhanced/Launch Michael Works Mod.cmd** so save protection
   is prepared and any valid queued preset changes are applied while the game is closed.

Core camera, interface and Exploration features do not need copied game configuration
files. Optional presets require a one-time local baseline import; the guide explains
Fusion Fix's local dump feature. No game data or save files are bundled here.

## Play and return safely

Pause with Options, hold L3 + R3 for 0.6 seconds, then release both sticks.
Use L1/R1 to reach Exploration. Save a return point, choose a destination with free
camera, check its teal landing ring, and confirm the teleport. A red marker means
the spot is blocked; amber means the check is pending. Look down to see the ground
beneath the camera; an edge arrow indicates an off-screen checked spot.

Release movement for three seconds before pausing for an Exploration action. Return
checks the saved floor and current body clearance, so standing and crouching are
supported. A return point belongs to the loaded scene and resets when that scene or
the game changes. Return restores position, while enemies and objectives continue.

Confirming a return-point save, teleport or mission practice load enables isolated
practice saving for that game process. Exit normally and relaunch to resume ordinary
saving. The public launcher prepares local save protection before launch; no backup
or account directory is included in this repository.

## Tested scope

The underlying 0.8.2 gameplay backends were tested in solo campaign, Hunter,
Infiltration and Last Stand on the recorded supported installation. The two latter
mode checks used `C01_MafiaClub`. Other arenas and full-match/wave completion were
not separately certified by those short Exploration checks. Co-op, network and
split-screen use is excluded. Public packaging/bootstrap changes are tested separately;
see [validation](docs/VALIDATION.md) for the exact evidence and limitations.

## Documentation

- [Installation and optional presets](docs/INSTALLATION.md)
- [Controls and tools](docs/CONTROLS.md)
- [Compatibility](docs/COMPATIBILITY.md)
- [Troubleshooting and rollback](docs/TROUBLESHOOTING.md)
- [Building and testing](docs/BUILDING.md)
- [Contributing](CONTRIBUTING.md)

## License

Copyright (c) 2026 Michael Works. Free personal use is permitted under
[LICENSE.txt](LICENSE.txt); other rights are reserved. This public-source project
is not offered under an open-source license. Third-party licenses remain separate
and are preserved in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
