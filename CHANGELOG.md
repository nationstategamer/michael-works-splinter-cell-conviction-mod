# Changelog

## 0.8.4 - standalone installer preview

- A self-contained installer and manager for 64-bit Windows. Players do not need
  to install Python. The installed manager provides compatibility checks,
  installation/update, protected launch and mod removal.
- Uses the same strict game/Fusion Fix checks and original-save preparation as
  the public script workflow. Launch prepares a fresh backup before requesting
  startup through Steam; ambiguous save accounts require explicit selection.
- Optional camera/control, Tactical stealth and Deniable Ops presets use a local
  baseline folder selected in the manager. No automatic dump or prerequisite
  download is performed.
- Removal keeps original saves, private backups and the manager EXE for safe
  inspection or reinstallation. The Python developer ZIP remains available.

The native game plugin remains 0.8.3; this release changes public installation
and management, preserving its camera, PS5 controls and protected solo tools.

## 0.8.3 - public preview

- Public setup, uninstall and launch helpers with compatibility checks and local
  original-save preparation before launch.
- Core camera, HUD, mission practice and Exploration tools can be installed
  without distributing game configuration files.
- Optional camera/control, Tactical and Deniable Ops presets import five verified
  pristine baselines locally from the user's own licensed installation.
- Public-source cleanup, portable x86 builds, synthetic tests, package manifests,
  checksums and Windows CI.
- Michael Works copyright and a free personal-use license; other rights reserved.

Retains the 0.8.2 gameplay backends: PS5 debug-menu controls, free camera, persistent
HUD choices, controller startup/focus recovery, protected recorded-checkpoint and
mission-part practice, and guarded solo Exploration with a landing marker and
floor-relative return point. See the compatibility and validation documents for
the supported installation and the limits of the recorded in-game checks.
