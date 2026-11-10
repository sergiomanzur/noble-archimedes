# Noble Archimedes for Android

The Android host for Noble Archimedes: the same statically recompiled Xbox game,
the advanced Vulkan/D3D12 renderer, widescreen, HD texture packs, saves, sound
and the in-game settings menu. It runs natively on ARM64 Android and adds a
branded splash, the Noble Archimedes launcher icon, a touch pad, and full SDL3
controller support.

This is a **bring-your-own-game** build. No game data is included in the APK or
in this repository.

## What is in the APK

- The Xbox recompilation runtime and the advanced renderer, built for
  `arm64-v8a` against Android API 26.
- vkd3d 2.1 (Direct3D 12 on Vulkan), with the mobile/virtual-descriptor-heap
  path selected for Adreno and other mobile drivers.
- SDL3 3.4.16 for the native window, Android audio (AAudio), touch and
  controllers.
- A branded Android 12+ system splash and an in-app splash, using the original
  Noble Archimedes astrolabe artwork.
- A complete touch pad: two sticks, d-pad, A/B/X/Y, LB/RB, LT/RT, Back, Start,
  L3/R3 and a settings button, plus a floating show/hide bubble.
- A 1.9 GB-class game-data layout under the app's external files directory.

## Requirements

- Windows x64 with Python 3.10+, CMake 3.25+, Ninja, LLVM Clang/LLD, the
  Visual Studio C++ Build Tools, and the **advanced** PC build prepared once.
- Android SDK with platform 34, build-tools 34, CMake 3.22.1 and **NDK
  26.2.11394342**.
- **JDK 17 or 21**. Android Studio's bundled Java 25 is too new for Gradle
  8.10; point `JAVA_HOME` at a JDK 17/21 (the build helper looks in
  `output/android-work/jdk` too).
- Gradle 8.10.x, via the included wrapper.
- Your own extracted USA Xbox disc in `extracted/` (`default.xbe`, `data/`,
  `movie/`, `sound/`).

## Build

Prepare the advanced PC tree and the recompiled game code once:

```powershell
python xbox/tools/noble_next.py --assets extracted --build --full-game
```

Then build the APK:

```powershell
python tools/build_android.py
```

The helper copies `android/overlay` over the prepared SDK/port, writes the
per-XBE support files, and invokes Gradle. The APK is written to:

```
android/app/build/outputs/apk/debug/app-debug.apk
```

Use `--release` for the release variant (debug-key signed for local testing),
and `--install` to install it with adb.

If the advanced `output/noble-next/build-windows` tree is missing, the helper
stops and asks you to run the preparation command above. The Android build
reuses its ImGui, toml++, xxhash and SDL3 source checkouts so no second copy of
those dependencies is downloaded.

### vkd3d prebuilt libraries

`android/third_party/vkd3d` contains the arm64 `libvkd3d*.so` libraries and
headers used by the APK. They are LGPL-2.1-or-later; the exact source, license
and the one mobile patch are documented in
[`third_party/vkd3d/README.md`](third_party/vkd3d/README.md). Rebuild them with
`tools/build_vkd3d_android.sh` if needed.

## Install the game data

The app reads from its external files directory:

```
/sdcard/Android/data/com.noblearchimedes.game/files/assets/
```

Push the game once (about 3 GB; USB transfer is fastest):

```powershell
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
& $adb shell mkdir -p /sdcard/Android/data/com.noblearchimedes.game/files/assets
& $adb push "extracted\default.xbe" "/sdcard/Android/data/com.noblearchimedes.game/files/assets/"
& $adb push "extracted\data"       "/sdcard/Android/data/com.noblearchimedes.game/files/assets/"
& $adb push "extracted\movie"      "/sdcard/Android/data/com.noblearchimedes.game/files/assets/"
& $adb push "extracted\sound"      "/sdcard/Android/data/com.noblearchimedes.game/files/assets/"
```

If `adb push` cannot create the nested folders, make them first with
`adb shell mkdir -p`, or push a tar and extract it in the app's `assets`
directory. Saves, settings, logs and the generated `hdd/` live beside
`assets/`; HD texture packs go in `files/textures/` if you use them.

## Controls

### Controllers

SDL3 opens USB, Bluetooth and built-in gamepads. The Retroid Pocket 6's
`Retroid Pocket Controller` is detected as a standard pad; AYN Thor and other
handhelds use the same Android gamepad path. Controllers can be connected or
removed while the game runs. **Start + Back** opens/closes the in-game settings
menu, the touch pad's **MENU** button does the same, and **L3 + R3** shows or
hides the touch pad. Connecting a pad hides the touch pad automatically;
disconnecting it brings the touch pad back.

### Touch pad

- Left stick: movement. Right stick: camera.
- D-pad, A/B/X/Y, LB/RB, LT/RT, Back, Start, L3/R3.
- The **TOUCH / HIDE** bubble on the left edge shows or hides the pad and
  remembers your choice. It is only shown when no physical gamepad is
  connected. With a pad, the touch controls and the bubble stay hidden; press
  **L3 + R3** to bring the pad back or put it away again.
- **MENU** opens the game's ImGui settings menu and hides the pad so the menu
  is fully usable by touch. The same happens when a controller opens the menu
  with **Start + Back**, or when the system **Back** button is used. Closing the
  menu restores the pad.
- The menu uses large text and controls sized for a phone screen. Its pages
  always show a wide scrollbar and scroll by touch drag, mouse wheel or the
  controller's D-pad/stick. Tapping a setting still works normally; dragging
  the page scrolls it.
- The **Controls** tab has the three camera systems: **Classic** (the original
  follow camera), **Over-the-shoulder** (RE2 Remake style) and **First person**.
  The right stick turns the selected camera; the **Camera** section's
  **Invert camera left / right** and **Invert camera up / down** flip its axes
  immediately. Unchecked is the standard direction.
- A touch in the menu needs a normal, brief press; a very quick tap may be
  treated as a hover by SDL's touch-to-mouse bridge.
- The Android **Back** button toggles the settings menu instead of leaving the
  game.
- **HIDE** is a small overlay and never covers the HP bar or the game's
  top-left HUD.

## Current qualifications and limits

Tested on a Retroid Pocket 6 (Snapdragon 8 Gen 2 / Adreno 740, Android 13):
splash, title, intro movies, name entry, save creation, courtyard gameplay,
touch movement and camera, the in-game menu, and touch menu navigation all
work. The game boots from the external `assets` directory and runs at the
window's 1920x1080 output.

Known differences from the Windows build:

- **Background/resume**: Android can destroy the native surface when the app
  is backgrounded. On return the Vulkan surface and swap chain are made again
  before the next frame, so the picture comes back. The display rotation
  itself is handled correctly; the picture is upright in landscape.
- **One back-buffer address**: the Android host keeps the game's render target
  on one address instead of rotating the Xbox's two guest buffers. Alternating
  them presented every other frame black on the mobile Vulkan path, which
  looked like constant flicker in the title menu, cinematics and gameplay.
  The PC host keeps the original rotation.
- **Mouse look**: the imported PC mouse capture is Windows-only. Android gets
  the same Classic, over-the-shoulder (RE2 Remake) and first-person cameras,
  driven by the right stick from a controller or the touch pad, with the same
  dead zones, shoulder/distance offsets and camera inversion. Mouse and
  relative-look controls are not available.
- **HD movie packs**: HD movie replacement needs FFmpeg. The game's own XMV
  movies play normally; an MP4 movie pack is ignored until an Android FFmpeg
  build is added.
- **EU release**: the current APK is built for the USA XBE. The EU executable
  path is not packaged yet.
- **Windows-only graphics**: DLSS, DXR ray tracing, frame generation and HDR
  output are not available on Android. The advanced renderer's basic path,
  widescreen, HD textures, anisotropic filtering, FSR-style TAA/upscaling
  settings and the settings menu are present.
- **Display mode/resolution** rows in the Display tab describe desktop
  behaviour; Android always presents full-screen. V-Sync and render scale
  remain meaningful.

The APK and generated game code are for your own use with your own disc. Do
not distribute an APK containing game code or game data.
