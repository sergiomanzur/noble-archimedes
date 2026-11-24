Noble Archimedes 1.2.0 for Windows x64
======================================

Extract the ZIP to a writable folder and run NobleArchimedesLauncher.exe.
Put your original Xbox disc image or extracted files in assets. For extracted
files, keep default.xbe and the original folder layout. The compiler creates
the game library locally on first run, which can take several minutes.

The Windows ZIP contains the tested USA host. The project retains the
European compatibility sources, but this package does not include an EU
executable; EU gameplay and translated game data still need testing with
complete PAL files.

WHAT IS NEW IN 1.2.0 / 1.1.0 / 1.0.0
  - Android (v1.2.0): fixed widescreen and portrait-panel render scaling (with
    Hor+ Full screen for 19.5:9 / 20:9 / 21:9 phones and live WxH pixel
    readout), cleaned Android Display/Graphics menus, single-tap Settings menu
    fix, and an interactive Touch Controls Layout & Button Remapping Editor.
  - Android (v1.1.0): full ARM Mali GPU support via a direct native Vulkan 1.1
    backend and in-process glslang shader compiler (no vkd3d).
  - Windows / Linux (unchanged from v1.0.0): AMD GCN vertex-stride fix for
    stretched/melted 2D quads and built-in GitHub auto-updater.

Open F10 > Controls for separate movement and camera stick dead zones.
Both default to 10%. Set either to 0% to disable that additional filter.
F5 cycles camera modes and F6 toggles mouse capture.

Windows requires Direct3D 12. Optional NVIDIA features require compatible
hardware and a separately installed Streamline runtime.

To carry saves forward from an older build manually, copy hdd while both
builds are closed. Keep a backup of that folder.

See CHANGELOG.md for this release's changes. The companion source archive
contains the corresponding project, patched SDK and dependency sources.
See the licenses folder and the compiler's LLVM license for terms and credits.
