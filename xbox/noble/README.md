# Noble Archimedes product overlay

This directory maintains the primary Xbox product's changes without altering
the pinned upstream snapshot in `../third_party/castlevania-cod-pc`.

`overlay/src/launcher/` adapts the donor's GPL-2.0-only launcher and translations.
It loads only our own artwork as branding, persists the music preference and
selects the appropriate branded USA/EU executable. These adaptations retain
the donor's GPL-2.0-only terms; see the root LICENSE scope and donor credits.

`overlay/noble-branding.cmake` embeds the original application icon and stages
the original assets. `../tools/jmafort_backend.py` reproducibly adapts CMake
targets, recompiler config DLL names, game-info image names and test-tool
executable names to `NobleArchimedes*`. A source-state inventory allows updates
to our prior output while rejecting unrecognized manual edits.

Keep the recompiler config, target, import library, DLL, release table and
runtime image names aligned. Renaming only the executable would break
first-run compilation. Internal window classes, settings and logs currently
retain upstream names for protocol compatibility.

Original assets, provenance and score are in `../../assets/noble/`. The
soundtrack is synthesized by `../../tools/branding/generate_music.py`.
Do not add game artwork, audio, replacement packs or generated game code here.
