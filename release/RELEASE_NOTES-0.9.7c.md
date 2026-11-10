# Noble Archimedes v0.9.7c

The Android camera, shortcut and menu update. Native builds for **Windows
x64**, **Linux x86_64** and **Android arm64** are available. All are
bring-your-own-game: no disc image, game executable, game assets or generated
game code is included.

The Windows and Linux packages are unchanged from v0.9.7b and are attached to
this release for convenience. Only the Android APK and the source archive were
rebuilt for v0.9.7c.

## Downloads

- **Android arm64 (new):** `Noble-Archimedes-0.9.7c-Android-arm64.apk`
- **Windows x64 (unchanged):** `Noble-Archimedes-0.9.7b-Windows-x64.zip`
- **Linux x86_64 (unchanged):** `Noble-Archimedes-0.9.7b-Linux-x86_64.AppImage`
- **Source:** `Noble-Archimedes-0.9.7c-Source.zip`
- Verify every download with `SHA256SUMS.txt`.

## Android changes

- **Separate shortcuts.** On Android, **L3 + R3** now only shows or hides the
  touch controls, while **Start + Back** opens or closes the settings menu. The
  touch **MENU** button and the system **Back** button also toggle the menu. The
  PC build keeps L3 + R3 for its menu.
- **Camera parity.** The Controls tab now offers the same three camera systems
  as the PC build: **Classic**, **Over-the-shoulder (RE2 Remake)** and **First
  person**, with the same dead zones, shoulder offset, orbit distance and
  first-person eye offset.
- **Camera inversion.** The Camera section's **Invert camera left / right** and
  **Invert camera up / down** options now apply to gameplay immediately.
  Unchecked is the standard direction; checked reverses that axis.
- **One input path.** The touch pad's right stick is merged into the pad before
  the modern dead zones and camera processing, so touch and a physical
  controller drive the camera identically.
- **Readable menu.** The Android settings menu now uses a ~30 px system font
  with 1.5x control sizes, a wider panel, a large always-visible scrollbar, and
  touch drag-to-scroll. D-pad/stick navigation scrolls to the focused setting.
- **Overlay handling.** The touch pad now steps aside for menus opened by a
  controller, not only for its own MENU button, so touch menu input is never
  blocked.
- The branded splash and launcher icon, the single guest back-buffer flicker
  fix, controller auto-hide and the background/resume surface rebuild are
  retained.

## Controls on Android

- Touch left stick: move. Touch right stick: camera.
- Touch d-pad, A/B/X/Y, LB/RB, LT/RT, Back, Start, L3 and R3.
- **L3 + R3** (controller): show or hide the touch controls.
- **Start + Back** (controller), touch **MENU**, or system **Back**: open or
  close the settings menu.
- In the menu: tap a setting, drag the page to scroll, or use the controller's
  d-pad/stick, A, B, LB/RB and Start.

## Android notes

- Android 8.0 (API 26) or later, arm64-v8a.
- Keep the game in
  `/sdcard/Android/data/com.noblearchimedes.game/files/assets/`
  (`default.xbe` with `data/`, `movie/` and `sound/` beside it).
- The APK is a sideload build signed with the project's local debug key. It is
  not a Play Store package.
- HD movie packs and the Windows mouse look are not available on Android.
- The v0.9.7c APK compiles and packages successfully. The new camera and menu
  behavior still needs a final on-device pass on a Retroid Pocket 6; please
  report anything that does not match the PC build.

## Windows and Linux

These are the v0.9.7b packages, unchanged:

- **Windows:** extract the ZIP and run `NobleArchimedesLauncher.exe`; Direct3D
  12 is required. Put your game in `assets`.
- **Linux:** run the AppImage; the first start copies it to
  `~/.local/share/NobleArchimedes`. Put your game in that folder's `assets`
  directory. glibc 2.35 or later.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7c/android/README.md) for Android build and install details.

See [CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7c/CHANGELOG.md) for the complete list and
[README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7c/README.md) for platform guides and project details.
