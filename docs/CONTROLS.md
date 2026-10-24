# Controls and tools

PS5 support uses Steam Input/XInput and the tested Xidi stack, rather than direct
DualSense HID. Enable the game's Use Controller option.

| Action | PS5 control |
|---|---|
| Pause/resume game | Options |
| Open/close debug menu | Hold L3 + R3 for 0.6 seconds, then release |
| Menu navigation | D-pad or left stick |
| Select / cancel | Cross / Circle |
| Change page | L1 / R1 |
| Free camera move / look | Left stick / right stick |
| Camera lower / raise | L2 / R2 |
| Camera speed | D-pad down / up |
| Exit camera | Circle |
| Retain Exploration destination and open menu | Pause, then hold L3 + R3 |

Keyboard: Ctrl + Shift + F10 toggles the menu; arrows/Tab and Enter/Space navigate;
Escape closes/cancels. F1-F7 select pages while open. Free camera requires an owning
connected controller. The menu does not pause gameplay automatically.

## Exploration

Stop on clear supported floor for three seconds before pausing. Save a return
point, confirm protected practice and release the controls. Choose a nearby camera
destination, resume briefly to fly, then pause and hold L3 + R3 to retain it.
Teal marks checked ground, red means blocked, and amber means checking. An edge
arrow indicates a checked off-screen landing. Confirm Teleport and release the
controls while native checks complete. Resume with Options when needed.

Return rechecks the saved floor and fits the current standing or crouched body
above it. Walls, low ceilings, changed floors and invalid scenes can refuse a move.
The marker stays independent of hidden HUD elements and the optional camera hint.
Actual movement repeats validation instead of trusting an old marker.

Later teleports retain the first return point. Replace return point deliberately
chooses a new origin. Return restores position; objectives, enemies and time keep
progressing. A scene/profile/checkpoint change clears the old point. Points are
not retained across game launches.

## Interface and Mission

HUD switches apply immediately and persist for later launches. Restore all saves
visible defaults; master hide preserves individual choices.

Mission uses recorded campaign checkpoint data and is intentionally unavailable
in Deniable Ops. Supported mission A/B parts use native replay. Loading practice
replaces current unsaved progress in the running scene and enables protected
practice for the process.

## Saves

Saving a return point, teleporting or loading Mission practice enables isolated
practice saving until exit. Opening the menu/flying alone does not. Exit normally
and relaunch through the mod launcher to resume ordinary saving.
