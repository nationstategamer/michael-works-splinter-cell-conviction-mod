# Third-party notices

Michael Works Splinter Cell Conviction Mod includes original mod work and uses separately owned game content and third-party software. The notice **Copyright (c) 2026 Michael Works. All rights reserved.** applies only to the project's original contributions, as described in [COPYRIGHT.txt](COPYRIGHT.txt).

## Dear ImGui

Dear ImGui v1.91.9b is Copyright (c) 2014-2025 Omar Cornut and is provided under the MIT License. Its source, copyright notice and complete license remain in [native/vendor/imgui/LICENSE.txt](ConvictionEnhanced/native/vendor/imgui/LICENSE.txt). The project's copyright notice does not replace the MIT permission terms.

## MinHook and Hacker Disassembler Engine

MinHook v1.3.4 is Copyright (C) 2009-2017 Tsuda Kageyu. It uses BSD-style redistribution terms. Included Hacker Disassembler Engine portions are Copyright (c) 2008-2009 Vyacheslav Patkov and retain their own BSD-style notices. The complete MinHook, HDE32 and HDE64 notices and disclaimers remain in [native/vendor/minhook/LICENSE.txt](ConvictionEnhanced/native/vendor/minhook/LICENSE.txt). Preserve that license when redistributing the corresponding source or binary components.

## miniz

The checkpoint archive validator uses the in-memory inflater from **miniz 3.0.2**, pinned to commit `293d4db1b7d0ffee9756d035b9ac6f7431ef8492`. Its MIT-style license and complete notices remain in [native/vendor/miniz/LICENSE](ConvictionEnhanced/native/vendor/miniz/LICENSE), with the source identity recorded in [native/vendor/miniz/PINNED.json](ConvictionEnhanced/native/vendor/miniz/PINNED.json).

The license credits **Copyright 2013-2014 RAD Game Tools and Valve Software** and **Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC**. The original notices, permission grant and disclaimer are preserved. This integration uses memory inflation only; it does not enable miniz file or ZIP archive APIs. Michael Works' original-work copyright does not replace these terms.

## Standalone runtime

The optional standalone Setup/Manager EXE embeds CPython and its standard library,
Tcl/Tk for its Windows interface, and required runtime libraries including OpenSSL.
Their licenses remain separate from the Michael Works personal-use terms. Exact
Python and build-tool versions are recorded with each standalone build. Complete
runtime notices are bundled under `runtime-licenses/` and can be read in the app's
License and notices viewer. CPython's notice includes its separately credited
components; OpenSSL retains its Apache License 2.0 terms.

The executable is assembled with PyInstaller, whose bootloader exception permits
distribution of generated applications under their own applicable terms. Its
unmodified license/exception text is included with the runtime notices. Build
dependencies are pinned by version and official PyPI wheel hashes in
`tools/standalone-requirements.txt`.

## Game content and compatibility components

Tom Clancy's Splinter Cell Conviction, its game code, assets, original configuration data and associated trademarks belong to their respective owners. Game-derived baselines and extracted data are not distributed. They are imported locally from the user's own game and are not claimed as original Michael Works property. This mod's copyright notice does not grant rights to redistribute game content.

Fusion Fix, Xidi and the ASI loader are separate compatibility components with their own authors, copyright notices and license terms. They are prerequisites of the inspected installation; the portable mod package does not include those binaries. Their existing notices and terms remain applicable.

This is an independently authored mod. The project name does not imply ownership of the underlying game or official endorsement by its owners or the third-party component authors.
