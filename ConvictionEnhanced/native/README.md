# Native component

Original x86 ASI source for Michael Works Splinter Cell Conviction Mod. See the
[project guide](../README.md) and [build guide](../../docs/BUILDING.md).
Public Release keeps all runtime compatibility, controller release,
collision, scene and save-health guards. NDEBUG removes vendor/development
assertions, while explicit runtime safety checks remain enabled.

Only InitializeASI is a production export. CETest fixtures/exports belong only
to the separate test DLL, which is never a release asset. No game configuration,
account directory, backup or runtime dump belongs in Git.
