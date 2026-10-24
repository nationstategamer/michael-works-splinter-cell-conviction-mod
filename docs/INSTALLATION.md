# Installation

Use the compiled release ZIP, not GitHub's source-code ZIP. Windows and Python
3.10+ are required. Close Conviction before setup, upgrades or removal.

## Core tools

1. Install the exact supported prerequisites in [COMPATIBILITY.md](COMPATIBILITY.md).
2. Extract the release so Install.cmd is at <game>/ConvictionEnhanced/Install.cmd,
   beside the game's existing src directory.
3. Run Install.cmd. Setup verifies compatibility, installs the owned mod ASI and
   prepares a private save backup/source marker. It does not enable Steam Input
   or change the game's saved controller preference.
4. For PS5, enable Steam Input for Conviction and enable Use Controller in the game.
5. Use Launch Michael Works Mod.cmd for later launches so save protection is
   prepared and valid queued preset changes are applied with the game closed.

Core camera, HUD, Mission practice and Exploration install independently of the
optional game-configuration presets. If multiple Ubisoft save accounts are found,
choose the intended local product-2 save directory explicitly. From the game folder:

    ./ConvictionEnhanced/Install.cmd --save-source "<launcher>/savegames/<account>/2"

Substitute your real local path and keep account directories private. If no save
exists, create a profile/save in an ordinary game launch, exit, and rerun setup.
Never use another game's save group. Source checkouts outside the game folder must
pass --game explicitly and need a reviewed compiled build manifest.

## Optional presets

Preset editing needs complete pristine baselines from your own game. These files
are not distributed here. A compatible existing local dump can be imported directly.
Otherwise the supported Fusion Fix provides this one-time local extraction route:

1. With the game closed, back up its Fusion Fix INI and set DumpPackedFiles=1 in
   [FILELOADER] in src/system/scripts/SplinterCellConviction.FusionFix.ini.
2. Run the game once, allow the dump to finish, then exit normally. Dumping all
   archives may need substantial disk space and time. Setup does not enable it.
3. Set DumpPackedFiles=0 again. Do not rename the whole dump to update.
4. From the game folder, run:

    ./ConvictionEnhanced/Install.cmd --baseline-dir "./src/system/unpacked" --presets comfort tactical deniable

Only the five hash-matching root INIs are imported into the private state cache.
Incomplete, conflicting or redirected sources are refused. The dump is left alone.
Preset installation selects standard mouse input explicitly. Later choices can be
queued on Settings and applied at the next closed-game mod launch.

Core tools remain usable without optional presets. Their Settings queue remains
unavailable until local baseline import and preset setup succeed.

## Upgrade and remove

Exit the game, replace the mod folder with the new release and run Install.cmd.
Keep ConvictionEnhanced-state and its ownership records/backups. Changed owned
files are refused rather than silently overwritten.

Run Uninstall.cmd with the game closed to remove the owned ASI/presets and restore
their recorded originals. Private save backups are retained; uninstall never
restores campaign saves over newer progress. See [troubleshooting](TROUBLESHOOTING.md).
