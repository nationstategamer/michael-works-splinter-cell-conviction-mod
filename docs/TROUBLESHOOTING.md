# Troubleshooting and rollback

Open **Michael Works Mod.exe**, select the game folder and choose
**Check installation** while Conviction is closed. Share only a short redacted
summary; keep save/account directories and backups private.

## Setup refused

Use the supported files in [COMPATIBILITY.md](COMPATIBILITY.md). Unknown hashes,
redirected paths, missing keys and externally changed owned files are refused.
Reconcile other mods instead of deleting ownership records to force writes.
The standalone setup/manager needs 64-bit Windows 10 or 11 and includes its runtime.
It does not need Python installed. The separate developer ZIP and legacy CMD
scripts require Python 3.10+; use the setup EXE for the player installation.

Use a reasonably short game/install path. Deeply nested Windows paths can exceed
the limit when temporary preference names are added. If HUD saving reports an
error, shorten the path and try again; the previous saved preferences are retained.

Choose the intended **Save folder** if multiple accounts exist.
If no product-2 save exists, create a local game profile/save normally, exit and
prepare saves again. Never copy another person's practice source marker.

## Controller or mouse

Enable Steam Input for Conviction and the game's Use Controller option. Use the
32-bit Xidi stack and XInputNative mapper listed in compatibility. Release controls
after menu closure, loading or focus changes.

Standard mouse input is the public preset default; raw input is optional.

For source/developer workflows only, `controller_layout.py` can explicitly select
`KT_X360` and retain a backup. With Python installed and the game closed, run
`python controller_layout.py apply` from the package folder; `restore` returns its
recorded previous value. This optional source tool is separate from standalone
setup. Normal PS5 installation uses Steam Input and the game's controller option.

## Exploration refused

Move away from obstructions, leave cover/aim/interactions, stop for three seconds
and pause again. The public build includes floor-relative returns, so standing and
crouching can share a point when body clearance allows it. Low ceilings, missing
fresh input, changed floors and changed scenes still refuse unsafe movement.

Keep the intended point; Replace return point deliberately changes the origin.
Resume with Options if paused. A scene/checkpoint change needs a new point.

## Practice saving and uninstall

Confirmed practice remains active until exit, including after a blocked move.
Uncached reads or local integrity/I/O failures fail closed; there is no fallback
to changing originals. Exit normally, prepare fresh saves and restart for normal
saving. Returning does not rewind world state.

**Remove mod** restores only recorded installation originals and retains save
backups. Externally edited files are refused. The manager EXE remains available;
close it before deleting it manually if no longer needed. The developer package's
`Uninstall.cmd` uses its Python workflow. Do not copy a save backup over newer
progress as a routine removal step.
