Noble Archimedes Beta 0.9.7b for Linux x86_64
============================================

This is a native Linux AppImage of Noble Archimedes, built from the same
recompiled Xbox game and advanced renderer as the Windows and Android builds.

GETTING STARTED
  1. Make the AppImage executable and run it:
       chmod +x Noble-Archimedes-0.9.7b-Linux-x86_64.AppImage
       ./Noble-Archimedes-0.9.7b-Linux-x86_64.AppImage
  2. On the first start the AppImage copies itself to a writable folder:
       ~/.local/share/NobleArchimedes
     (or $XDG_DATA_HOME/NobleArchimedes). The launcher opens there.
  3. Put your own game in that folder's assets directory: the disc image
     (.iso), or the extracted disc (default.xbe with data, movie and sound).
  4. Run the AppImage again. The first start builds the game code from your
     copy, which takes a few minutes; after that the game starts directly.

The AppImage is a bring-your-own-game build: no disc image, game executable,
game assets or generated game code is included.

REQUIREMENTS
  A 64-bit Linux with glibc 2.35 or later (Ubuntu 22.04 or later, Debian 12,
  Fedora, Arch, SteamOS and similar), X11 or Wayland, and a Vulkan driver
  (Mesa for AMD/Intel, or NVIDIA's driver). The package includes vkd3d,
  FFmpeg and the compiler used for the first-run game build.

WHAT IS NOT IN THIS LINUX BUILD
  Ray tracing, DLSS/DLSS Ray Reconstruction, frame generation and the
  Windows mouse/over-the-shoulder camera are Windows-only or unavailable
  through vkd3d. AMD FSR and the launcher's prepared shader cache are not
  part of this build. The tested configuration is the USA release; the
  European executable is not included.

CONTROLS
  The launcher and in-game menu cover graphics, audio and controls.
  F10 or clicking both sticks (L3 + R3) opens the in-game settings.
  Start + Back opens the settings menu with a controller.

Saves are in the writable folder under hdd/E/UDATA/4b4e002d; logs are in
logs. Keep the writable folder if you replace the AppImage with a newer
release, and keep a backup of your saves.

This is a personal, non-commercial fan project. It is not affiliated with,
endorsed or sponsored by Konami or Microsoft. Castlevania is a trademark of
Konami Digital Entertainment. See THIRD_PARTY.txt and LICENSE.txt inside the
package for licenses and credits.
