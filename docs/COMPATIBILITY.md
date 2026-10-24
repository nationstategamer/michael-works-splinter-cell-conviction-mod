# Supported configuration

Windows/x86, Steam build 11503660, version marker C-455_L-340_S-76. Exact game and
Fusion fingerprints are checked because native hooks depend on verified layouts.

| Component | Tested identity / SHA-256 |
|---|---|
| conviction_game.exe | beebb33559e064ec2f78065a4e9bc8dfe8844fa8711ea4afbcb880616170936f |
| Fusion Fix 04.10.2026 ASI | 41291418dd5b5e69c6758f81b06dff6ced8f54344661efc148a6146c5c084ce3 |
| Ultimate ASI Loader 9.7.4 as version.dll | d5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6 |
| Xidi dinput8.dll | 5561a6f76bad4bc7af66e04771d7c61e7ce4d97edd08151aaea06aa7cf6f5559 |
| Xidi.32.dll | 57859aba7fc67adf039119a05937fee40b53c53041b63645b761337abb98da77 |

Install the complete official [Conviction Fusion Fix bundle](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/scc)
([ZIP download](https://github.com/ThirteenAG/WidescreenFixesPack/releases/download/scc/SplinterCellConviction.FusionFix.zip)).
The verified bundle already contains all four matching components above, including
Ultimate ASI Loader and the tested Xidi build. You do not need separate controller
DLL downloads. Follow the bundle's own installation instructions and retain its
notices. The tested Xidi resource version is 0.0.1-dev.1.0+cafe9af9, with
`[Mapper] Type=XInputNative` in Xidi.ini. Standalone Xidi stable releases differ
from the bundled build and do not satisfy this preview's exact checks.

The official bundle checked for this preview was updated on 4 October 2026,
8,770,194 bytes, SHA-256
`ebc392c5f5e1ade0d160dfcf0325c53cc528b8683e1c3c7df115f12e91015e3c`.
The upstream download can change; compare hashes if setup refuses a newer bundle.
Prerequisite binaries are not redistributed with this mod. Their upstream projects
are [Fusion Fix](https://github.com/ThirteenAG/WidescreenFixesPack),
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) and
[Xidi](https://github.com/samuelgr/Xidi).

Do not disable checks to force an unknown build. Report the version/fingerprint
for review. A matching version label alone does not prove native compatibility.

PS5 uses Steam Input/XInput and the game's controller option. A saved off choice
stays off. Optional Blacklist controls need the matching companion assets checked
by compatibility.json; all 49 pinned companion assets were verified in the same
official Fusion Fix bundle. Core setup does not change Steam Input or controller files.

Solo campaign and guarded solo Deniable Ops are supported. Co-op/network,
competitive and split-screen/Fusion two-instance use are excluded. Other arenas,
Linux/Proton and other translators are not certified by this preview.

No game, original save, baseline INI or compatibility-component binary is bundled.
This is independent community software, with no official endorsement implied.
