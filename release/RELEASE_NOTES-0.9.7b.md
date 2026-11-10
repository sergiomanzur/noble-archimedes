# Noble Archimedes v0.9.7b

Native builds for **Windows x64**, **Linux x86_64** and **Android arm64**. All
are bring-your-own-game: no disc image, game executable, game assets or
generated game code is included.

## Downloads

- **Windows x64:** `Noble-Archimedes-0.9.7b-Windows-x64.zip`
- **Linux x86_64:** `Noble-Archimedes-0.9.7b-Linux-x86_64.AppImage`
- **Android arm64:** `Noble-Archimedes-0.9.7b-Android-arm64.apk`
- **Source:** `Noble-Archimedes-0.9.7b-Source.zip`
- Verify every download with `SHA256SUMS.txt`.

## Android

Native arm64 host using the advanced renderer on Vulkan through vkd3d. It adds
a branded splash and launcher icon, a complete touch pad, and SDL3 support for
USB, Bluetooth and built-in handheld controllers.

- Android 8.0 (API 26) or later, arm64-v8a.
- The touch pad and its show/hide bubble hide automatically when a controller
  is connected. **L3 + R3** shows or hides the touch pad.
- Background/resume recreates the Vulkan surface and swap chain.
- Keep the app's game data in
  `/sdcard/Android/data/com.noblearchimedes.game/files/assets/`.
- The APK is a sideload build signed with the project's local debug key. It is
  not a Play Store package.
- HD movie packs and the Windows mouse camera are not available.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7b/android/README.md) for build and install details.

## Linux

Portable x86_64 AppImage of the same recompiled game and advanced renderer.
It includes vkd3d, FFmpeg and the compiler used for the first-run game build.

- glibc 2.35 or later (Ubuntu 22.04+, Debian 12, Fedora, Arch, SteamOS).
- The first start copies the package to `~/.local/share/NobleArchimedes` so
  the game has a writable folder for `assets`, saves, settings and logs. Put
  your game in that folder's `assets` directory.
- The tested configuration is the USA release. The European executable, ray
  tracing, DLSS, frame generation, AMD FSR and the prepared shader cache are
  not included in this Linux build.
- The game's own XMV movies play; HD MP4 movie packs are not included.

See `README.txt` inside the AppImage for the full Linux notes.

## Windows

The Windows x64 package contains the tested USA host. Open
`NobleArchimedesLauncher.exe`, put your game in `assets`, and let the first
run build the game library. Direct3D 12 is required. F10 > Controls has the
separate movement and camera stick dead zones.

## Fixes in this build

- **Android flicker:** the Xbox double buffer alternated two guest back
  buffers; on the mobile Vulkan path the second host target presented black,
  producing a constant good/black blink. Android now keeps one back-buffer
  address, and the invalid-resource warnings are gone.
- **Android resume:** the Vulkan surface and swap chain are made again on
  return, with queued menu input reset so the first frame does not abort.
- **Controller-aware touch controls:** the pad and bubble hide when a gamepad
  is present; L3 + R3 toggles the pad.

See [CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7b/CHANGELOG.md) for the complete list and
[README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7b/README.md) for platform guides and project details.
