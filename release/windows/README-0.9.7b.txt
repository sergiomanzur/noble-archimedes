Noble Archimedes Beta 0.9.7b for Windows x64
============================================

Extract the ZIP to a writable folder and run NobleArchimedesLauncher.exe.
Put your original Xbox disc image or extracted files in assets. For extracted
files, keep default.xbe and the original folder layout. The compiler creates
the game library locally on first run, which can take several minutes.

The Windows ZIP contains the tested USA host. The project retains the
European compatibility sources, but this package does not include an EU
executable; EU gameplay and translated game data still need testing with
complete PAL files.

Open F10 > Controls for separate movement and camera stick dead zones.
Both default to 10%. Set either to 0% to disable that additional filter.
F5 cycles camera modes and F6 toggles mouse capture.

The v0.9.7b release also includes native builds for Linux x86_64 (AppImage)
and Android arm64 (APK). They use the same recompiled game and advanced
renderer; see the release page for their requirements and guides.

Windows requires Direct3D 12. Optional NVIDIA features require compatible
hardware and a separately installed Streamline runtime.

To carry saves forward, copy hdd from your previous build while both builds
are closed. Keep a backup of that folder.

See CHANGELOG.md for this release's changes. The companion source archive
contains the corresponding project, patched SDK and dependency sources.
See the licenses folder and the compiler's LLVM license for terms and credits.
