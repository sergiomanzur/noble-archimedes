# Noble Archimedes v0.9.7d

The Android first-run and black-title update. Native builds for **Windows
x64**, **Linux x86_64** and **Android arm64** are available. All are
bring-your-own-game: no disc image, game executable, game assets or generated
game code is included.

The Windows and Linux packages are unchanged from v0.9.7b and are attached to
this release for convenience. Only the Android APK and the source archive were
rebuilt for v0.9.7d.

## Downloads

- **Android arm64 (new):** `Noble-Archimedes-0.9.7d-Android-arm64.apk`
- **Windows x64 (unchanged):** `Noble-Archimedes-0.9.7b-Windows-x64.zip`
- **Linux x86_64 (unchanged):** `Noble-Archimedes-0.9.7b-Linux-x86_64.AppImage`
- **Source:** `Noble-Archimedes-0.9.7d-Source.zip`
- Verify every download with `SHA256SUMS.txt`.

## Android changes

- **First-run file picker.** With no game installed, the app opens Android's
  file picker. Choose your Castlevania: Curse of Darkness `.iso` or `.xiso`;
  the app copies it once into its own storage with a progress bar, checks that
  it is an Xbox disc image, and starts the game. Pushing extracted files with
  adb still works.
- **Black title screen fixed on Adreno.** The Adreno Vulkan driver frees one of
  its own objects inside `vkCmdPushDescriptorSetKHR` and then writes to it,
  which corrupted a texture or render target the game was about to draw. The
  title menu (and sometimes later screens) then came out black on some starts.
  The Android build now hides `VK_KHR_push_descriptor` from vkd3d, which falls
  back to ordinary descriptor sets. Verified on a Retroid Pocket 6
  (Snapdragon 8 Gen 2, Adreno 740): the title menu showed in 12 of 12 clean
  first runs and New Game reaches Name Entry.
- **Safety net.** If the game surface still shows no picture a minute after
  launch, the app restarts itself (twice at most).
- **Debugging aids.** F3 (adb: `input keyevent 131`) saves the frame's targets
  and shaders to `files/captures/`; crashes reach Android's own handler so
  tombstones and GWP-ASan reports appear in `logcat`.

## Android notes

- Android 8.0 (API 26) or later, arm64-v8a. Keep about 3.5 GB free for the
  disc image copy.
- The APK is a sideload build signed with the project's local debug key. It is
  not a Play Store package.
- HD movie packs and the Windows mouse look are not available on Android.
- Uninstalling the app removes its storage, including the copied disc image
  and saves; back up `files/hdd` first if you want to keep them.

## Windows and Linux

These are the v0.9.7b packages, unchanged:

- **Windows:** extract the ZIP and run `NobleArchimedesLauncher.exe`; Direct3D
  12 is required. Put your game in `assets`.
- **Linux:** run the AppImage; the first start copies it to
  `~/.local/share/NobleArchimedes`. Put your game in that folder's `assets`
  directory. glibc 2.35 or later.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7d/android/README.md) for Android build and install details.

See [CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7d/CHANGELOG.md) for the complete list and
[README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v0.9.7d/README.md) for platform guides and project details.
