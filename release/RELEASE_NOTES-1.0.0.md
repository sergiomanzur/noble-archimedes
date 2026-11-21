# Noble Archimedes v1.0.0

Native v1.0.0 release for **Windows x64**, **Linux x86_64** and **Android
arm64**. All downloads are bring-your-own-game: no disc image, game
executable, game assets or generated game code is included.

## Downloads

- **Windows x64:** `Noble-Archimedes-1.0.0-Windows-x64.zip`
- **Linux x86_64:** `Noble-Archimedes-1.0.0-Linux-x86_64.AppImage`
- **Android arm64:** `Noble-Archimedes-1.0.0-Android-arm64.apk`
- **Source:** `Noble-Archimedes-1.0.0-Source.zip`
- Verify every download with `SHA256SUMS.txt`.

## Highlights in v1.0.0

### AMD GCN rendering fix (stretched / melted 2D & full-screen draws)

- Ported the vertex-buffer re-striding fix from `jmafort/CastlevaniaCOD-PC`
  (`ccd5000`, issue #16) into all Noble Archimedes build paths (Windows D3D12,
  Linux vkd3d, Android vkd3d, and the legacy Xbox build path).
- Castlevania: Curse of Darkness's 2D quad draws declare an unused second
  texture coordinate at byte 28 of 28-byte vertices. On AMD GCN cards (RX
  400/500, Vega), having a declared vertex element reach past `StrideInBytes`
  corrupted the first texture coordinate on each quad's final corner,
  smearing the title screen, menus, HUD and full-screen frame pass into
  vertical streaks.
- `AssembleRaw` and `D3D12Backend::UploadVertexBuffers` now compute the
  element overhang (`VertexBuffer::tail`) and upload such buffers with a
  widened stride (`b.stride + b.tail`), keeping every declared input element
  strictly inside its own vertex record and buffer bounds.

### Automatic GitHub update checks across all platforms

- **Windows (`NobleArchimedesLauncher.exe`):** automatically checks GitHub
  Releases (`sergiomanzur/noble-archimedes`) when opened and shows an
  **Updates** card on the Home page (with a toggle on the **Game** page).
  Clicking **Update** downloads the latest Windows ZIP, verifies its size and
  SHA-256 digest against GitHub, atomically replaces the program files while
  preserving `assets/`, `textures/`, `hdd/`, `logs/`, `movies/`, `mods/`,
  `saves/`, `streamline/`, `compiler/work/` and `castlevania.toml`, and
  restarts the launcher automatically.
- **Linux (`NobleArchimedesLauncher` / AppImage):** checks GitHub Releases on
  launch, downloads and SHA-256-verifies the latest `.AppImage` (or `.tar.xz`),
  extracts it with `--appimage-extract` (no FUSE required), updates the
  installed files in `~/.local/share/NobleArchimedes` while preserving user
  data, restores executable permissions, and restarts the launcher.
- **Android (`Noble-Archimedes-1.0.0-Android-arm64.apk`):** checks GitHub
  Releases on launch in the background. When a newer stable release is
  available, it downloads and SHA-256-verifies the new APK into app storage
  and prompts the user with an in-app **Install Update** dialog that launches
  Android's package installer via a read-only `ContentProvider` (handling the
  Android 8+ "Install unknown apps" permission flow automatically).

## Platform notes

- **Windows x64:** extract the ZIP and run `NobleArchimedesLauncher.exe`;
  Direct3D 12 is required. Put your USA Xbox disc image or extracted disc in
  `assets/`.
- **Linux x86_64:** `chmod +x Noble-Archimedes-1.0.0-Linux-x86_64.AppImage`
  and run it; first start copies to `~/.local/share/NobleArchimedes`. glibc
  2.35+ and Vulkan required.
- **Android arm64:** Android 8.0 (API 26) or later, arm64-v8a. Opens a
  first-run file picker for your `.iso`/`.xiso` disc image (keep ~3.5 GB
  free). Sideload build signed with the project's local debug key.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v1.0.0/android/README.md),
[CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.0.0/CHANGELOG.md)
and [README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.0.0/README.md)
for full details.
