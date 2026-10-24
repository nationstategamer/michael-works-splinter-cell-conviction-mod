# Installation

For players, download **Michael-Works-Splinter-Cell-Conviction-Mod-Setup-0.8.5.exe**
from [Releases](https://github.com/nationstategamer/michael-works-splinter-cell-conviction-mod/releases).
This standalone installer and manager include their runtime: no separate Python
installation is needed. Setup runs on 64-bit Windows 10 or 11; the game plugin is x86.
Close Conviction before installation, updates, save preparation or removal.

## Install the core tools

1. Install your licensed Steam copy of Conviction and the complete supported
   official Fusion Fix bundle in [COMPATIBILITY.md](COMPATIBILITY.md). The bundle
   already includes the matching loader and controller components.
2. Run the downloaded setup EXE. Select **Game folder**, or choose **Browse** and
   select the Conviction folder containing its existing `src` directory. A single
   discovered Steam installation can be selected automatically; ambiguous choices
   require your selection.
3. Choose **Check installation**. This checks the supported game/Fusion Fix files,
   bundled plugin and local save discovery without changing your game or saves.
4. Leave **Save folder** empty only when one local Conviction save account is found.
   Otherwise browse to the intended Ubisoft `savegames/<account>/2` folder. See
   the save-account guidance below.
5. Choose **Install / update**. Setup prepares and verifies a private backup of
   the original save group, installs the owned plugin and records its ownership.
6. For PS5, enable Steam Input for Conviction and enable **Use Controller** in the
   game's own settings. Setup preserves existing controller preferences.
7. Choose **Launch game**. Use the installed
   `<game>/ConvictionEnhanced/Michael Works Mod.exe` for later launches. Each
   launch prepares fresh save protection and applies valid queued preset choices
   with the game closed before requesting startup through Steam.

Version 0.8.5 sends the Steam request with normal external library-search settings
and a normal window-show command, then restores the manager's own runtime
environment. Wait for the game to open before making another launch request.

The manager does not create desktop/start-menu shortcuts or launch the game
automatically after installation. Its four actions are **Check installation**,
**Install / update**, **Launch game** and **Remove mod**.

Camera, HUD, Mission practice and Exploration install independently of the
optional game-configuration presets. They require no local baseline extraction.
Core setup never downloads or replaces Fusion Fix, its loader or Xidi.

## Select the right save account

The save folder is the local Ubisoft Conviction product-2 group, commonly inside
`<Ubisoft launcher>/savegames/<account>/2`. Select that final `2` folder, rather
than the entire launcher or account folder. Account paths are private; avoid
posting them in bug reports.

If more than one account exists, setup does not guess. Choose the intended folder
with **Save folder** before installation or launch. If no local save exists,
start the unmodified game once, create a profile/save normally, exit, then retry.
Do not select another game's save group or another person's practice files.

Confirmed Exploration or mission practice uses isolated saving for that game
process. Exit normally and relaunch through the manager to resume ordinary saving.
Returning to a saved position changes position; it does not rewind enemies or
objectives. The manager retains original-save backups during removal and never
restores a backup over newer progress automatically.

## Optional presets

Open the manager's optional presets section. **Use camera and control presets**,
**Tactical stealth** and **Deniable Ops** use five verified pristine baseline
INIs from your own game. These files are not distributed with the mod. Select an
existing compatible local baseline folder, or create a local dump using the
supported Fusion Fix:

1. With the game closed, back up its Fusion Fix INI and set `DumpPackedFiles=1` in
   `[FILELOADER]` in `src/system/scripts/SplinterCellConviction.FusionFix.ini`.
2. Run the game once, allow the dump to finish, then exit normally. Dumping all
   archives may take substantial disk space and time. The manager does not enable
   the dump or start it automatically.
3. Set `DumpPackedFiles=0` again. Do not rename the whole dump to `update`.
4. In the manager, select the local baseline folder
   `<game>/src/system/unpacked`, choose the desired presets and select
   **Install / update**.

Only the five hash-matching root INIs are imported into the private state cache.
Incomplete, conflicting or redirected sources are refused. The dump is left alone.
Preset installation selects standard mouse input. Later preset choices can be
queued on the in-game Settings page and applied at the next manager launch.

The Settings queue remains unavailable until local baseline import and preset
setup succeed. Leaving presets unselected still installs the core tools.

## Update and remove

Exit Conviction and close the installed manager. Run the new downloaded setup,
select the existing game folder and choose **Install / update**. Keep
`ConvictionEnhanced-state` and its ownership records/backups. Unexpected changes
to owned files are refused instead of silently overwritten.

Choose **Remove mod** with the game closed to restore only recorded installation
originals and remove owned plugin/preset changes. Original saves and private
backups are retained. The manager EXE stays available for inspection or
reinstallation; after closing it, you may delete that EXE normally if no longer
needed. Removal does not delete save backups or restore them over newer progress.

## Developer and legacy packages

The separate `Michael-Works-Splinter-Cell-Conviction-Mod-0.8.5.zip` retains the
Python management scripts for developers and existing script workflows. Those
scripts require Python 3.10 or newer. Extract its `ConvictionEnhanced` folder
beside the game's `src` folder, then use `Install.cmd` and
`Launch Michael Works Mod.cmd`. Source checkouts outside the game folder require
an explicit `--game` path and a reviewed compiled build manifest. GitHub's
automatic source-code ZIP has no compiled plugin. See [BUILDING.md](BUILDING.md)
for source builds; players should use the standalone setup EXE above.
