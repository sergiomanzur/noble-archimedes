# Publication scope

For v0.9.0b the existing sanitized history is retained. The Xbox donor snapshot,
Noble branding overlay, generic host changes, license texts and original
`assets/noble/` artwork/icon/synthetic audio are public inputs. Those original
assets are the explicit exception to the historical blanket media exclusion
below. The runtime ZIP uses an explicit allowlist and omits the game DLL and
compiler work directory. A separate corresponding-source archive includes the
release commit plus actual SDK/dependency source used by the build.

Unreviewed private PS2/native-render research and local submodule work are
preserved in a recovery archive during directory cleanup, not bulk-published.

GitHub receives a new repository and fresh history. Original Git objects, ignored files and old commits are not copied. Selection uses explicit allowlists rather than trusting `.gitignore`.

Included: hand-authored host/UI/input/camera/VFS and compatibility glue, configuration metadata, build/local generation tools, selected synthetic tests, a pinned PS2Recomp gitlink, licensed runtime changes as a source patch, and GS-only adapter source/build scripts. `PUBLICATION_MANIFEST.json` records hashes of ordinary published files excluding itself.

Excluded: ISO/extracted files, ELF/IRX/ROM/BIOS data, translated guest implementations, game microcode and literal translations, game-specific native VU/GPU research kernels, artwork/screenshots/audio/video/textures, memory/GIF/VU captures and oracle data, saves/cards, binaries/objects/archives, dependency checkouts/build products, local logs, credentials and original history.

The runtime patch contains licensed upstream changes and selected hand-authored runtime files. Generic interpreter/compiler source is distinct from game microcode translations; the latter stay local. PCSX2 is fetched separately, not vendored.

Checks cover text-only payloads, path/extension allowlisting, credential patterns, patch paths and application against the pinned upstream tree. They are engineering checks rather than a legal opinion. Future changes must retain these exclusions. Aggregate development results are labeled as such; the gameplay target is explicitly unmet.
