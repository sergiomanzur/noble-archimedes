Noble Archimedes Beta 0.9.6b for Windows x64
============================================

Extract the ZIP to a writable folder and run NobleArchimedesLauncher.exe.
Put your original Xbox disc image or extracted files in assets. For extracted
files, keep default.xbe and the original folder layout. The compiler creates
the game DLL locally on first run, which can take several minutes.

The Windows ZIP contains the USA host and a PAL host. The USA build is tested.
The PAL host was built for the supported European executable. PAL gameplay
and translated game data still need testing.

Open F10 > Controls for separate movement and camera stick dead zones.
Both default to 10%. Set either to 0% to disable that additional filter.
F5 cycles camera modes and F6 toggles mouse capture.

Windows requires Direct3D 12. Optional NVIDIA features require compatible
hardware and a separately installed Streamline runtime.

To carry saves forward, copy hdd from your previous build while both builds
are closed. Keep a backup of that folder.

See CHANGELOG.md for this release's changes. The companion source archive
contains the corresponding project, patched SDK and dependency sources.
See the licenses folder and the compiler's LLVM license for terms and credits.
