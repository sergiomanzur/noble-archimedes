Noble Archimedes Beta 0.9.1b - Windows x64
========================================

Extract this ZIP to a writable folder. Run NobleArchimedesLauncher.exe.
The USA original Xbox release of Castlevania: Curse of Darkness is the
qualified target for this beta. An EU build has not been qualified for
this release; do not assume an EU disc will work. See manifest.json for
the exact executable regions packaged.

Supply your own legally obtained game in the empty assets folder:
  - an original Xbox disc image (.iso), or
  - the extracted disc, including default.xbe and all data/movie files.
Keep the complete folder layout when extracting a disc. No game content
is supplied by this project. On first run the included compiler tools
create the game DLL locally from your files; this can take several minutes.
Do not close the launcher while it is compiling. The compiler folder and
its headers, configuration, import library and xdk layouts are required.

Modern PC controls
------------------
This build adds direct relative mouse look and three camera modes:

  F5         cycle Over-the-shoulder -> First person -> Classic
  F10        open settings; use the Camera tab for sensitivity and options
  F6         release or recapture the mouse
  Left mouse X (attack)
  Right mouse hold = aim / slow walk
  Middle mouse = Y

Over-the-shoulder is the default. Movement follows the camera heading, so
W goes where the mouse points. Classic keeps the original title camera and
does not apply free look. Mouse look captures automatically during normal
gameplay and releases on menus, cutscenes and focus loss.

This is an experimental beta. Graphics, sound and gameplay may have
defects. Windows x64 and a Direct3D 12 capable GPU/driver are required.
The needed Microsoft C++ runtime DLLs are supplied beside the application
and compiler tools. Empty textures is reserved for user-provided content.
The branding folder contains original project artwork and synthetic audio.
Keep your own backup of saves before testing another build.

Source and licenses
-------------------
Corresponding source is supplied as the companion source archive on the
same v0.9.1b GitHub prerelease. Keep that archive with this binary release.
Project: https://github.com/sergiomanzur/noble-archimedes
The release manifest records the runtime file SHA256 hashes and source pins.
The source archive includes the complete project, imported donor source,
patches and build scripts. Donor CastlevaniaCOD-PC source is pinned at
1d9edf6b2bdbb683cb95dff5b317916aec31db84. XboxXBE-Recomp SDK is pinned at
91146ff6e2a218b19c1109dc665d06020472ab57, with the donor SDK patch applied.
Consult the companion source's build instructions for dependency acquisition.

The licenses directory includes project terms, donor GPLv2 license and
README credits, SDK MIT/third-party notices, actual fetched dependency
notices, and the original branding dedication. Compiler LLVM terms are
also in compiler/LLVM-LICENSE.txt. Preserve all these notices. This project
is not affiliated with Konami and grants no rights to proprietary game data.
