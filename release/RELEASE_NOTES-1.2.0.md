# Noble Archimedes v1.2.0 — Android Widescreen, Render Scaling & Touch Controls Editor

Noble Archimedes **v1.2.0** fixes Android widescreen and portrait-panel render
scaling (adding edge-to-edge Hor+ **Full screen** support for `19.5:9` / `20:9` /
`21:9` phones), adds a full interactive **Touch Controls Layout & Button
Remapping Editor**, cleans up the Android Display/Graphics menus, and fixes
single-tap navigation in the in-game Settings menu. All downloads are
bring-your-own-game: no disc image, game executable, game assets or generated
game code is included.

> **Desktop users (Windows & Linux):** Windows x64 and Linux x86_64 binaries
> are unchanged from **v1.0.0** and remain available on the
> [v1.0.0 release page](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0).
> Only the Android arm64 APK and companion source archive were rebuilt for
> v1.2.0 (so v1.0.0 desktop auto-updaters stay up to date cleanly, while
> v1.0.0 / v1.1.0 Android installs will automatically detect and offer this
> v1.2.0 APK update).

## Downloads

- **Android arm64 (new):** `Noble-Archimedes-1.2.0-Android-arm64.apk`
- **Source:** `Noble-Archimedes-1.2.0-Source.zip`
- **Windows x64 (unchanged):** download `Noble-Archimedes-1.0.0-Windows-x64.zip` from [v1.0.0](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0)
- **Linux x86_64 (unchanged):** download `Noble-Archimedes-1.0.0-Linux-x86_64.AppImage` from [v1.0.0](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0)
- Verify every download with `SHA256SUMS.txt`.

## Highlights in v1.2.0

### Fixed Android Widescreen & Portrait-Panel Render Scaling

- **Widescreen now works properly on portrait-panel phones:** previously, phones
  reporting portrait display modes (`1080×2408`, etc.) caused `Widescreen()` to
  permanently latch `false` regardless of settings. Android now normalizes live
  `WindowPixels()` to landscape (`width >= height`) and evaluates widescreen
  dynamically.
- **New Hor+ Full-screen aspect mode (`Full screen`):** fills ultra-wide
  `19.5:9` / `20:9` / `21:9` phones edge-to-edge without stretching, alongside
  `16:9`, `16:10`, and `Off (4:3)`.
- **Fixed massive 75% → 100% FPS drop on Mali & mid-range GPUs:** previously,
  `RenderScale()` scaled against the portrait height (`2408`) instead of the
  landscape height (`1080`), causing `75%` to render at `1806p` and `100%` to
  render at `2408p` (~5× too many pixels) and dropping Mali-G57 devices from
  60 FPS to 30 FPS. Render scale now targets the true landscape height (e.g.
  `67%` = `724p`, `100%` = `1080p`), with a live internal `WxH` pixel readout
  right beside the dropdown.
- **Cleaned up Android Display & Graphics tabs:** removed non-functional
  desktop-only controls (`Display mode`, `Resolution`, `HDR`, and PC-D3D12-only
  `TAA`/`Upscaling`/`Sharpening`/`Ambient occlusion`/`Ray tracing`) and added a
  clear V-Sync note explaining 60 → 30 FPS half-rate drops on mobile.

### Interactive Mobile Touch Controls Layout & Button Remapping Editor

- **Easy access:** open from the top of **Settings → Controls → `Edit Layout &
  Remap Buttons`** or by **long-pressing the `HIDE / TOUCH` bubble** in-game.
- **Full layout customization:** drag any of the 17 controls individually (each
  face button `A/B/X/Y`, `LT/LB/RB/RT`, `BACK/MENU/START`, `L3/R3`, Left/Right
  sticks, D-pad, and the `HIDE/TOUCH` bubble) anywhere on screen over a subtle
  alignment grid.
- **Per-control sizing & global opacity:** resize each control individually from
  `55%` to `165%`, and cycle overall touch opacity (`100% / 80% / 60% / 40%`).
- **Button & D-pad remapping + stick swap:** remap any button or individual
  D-pad direction to any Xbox action (or `Disabled (Hide)`), swap Left/Right
  sticks, and reset either the selected control or the entire layout.
- **Persistent & unobtrusive:** saves automatically in `SharedPreferences`; the
  editor toolbar floats cleanly below `BACK / MENU / START` and flips to the
  bottom whenever an upper-half control is selected.

### Touchscreen Settings Menu Fix

- **Single-tap responsiveness:** fixed Dear ImGui's touch hover-before-click
  issue (`ImGuiMouseSource_TouchScreen`) so tabs, dropdowns, checkboxes, and
  buttons in the Android Settings menu respond reliably on the first tap.

## Android notes

- **Requirements:** Android 8.0 (API 26) or later, `arm64-v8a`, Vulkan 1.1+
  (ARM Mali, Qualcomm Adreno, etc.).
- **Upgrading:** installing over v1.0.0 or v1.1.0 preserves your copied disc
  image, settings and saves. (If upgrading from an older `castlevania.toml`,
  open **Settings → Display** to pick `Full screen` widescreen and your
  preferred render scale.)
- Sideload build signed with the project's local debug key.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v1.2.0/android/README.md),
[CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.2.0/CHANGELOG.md)
and [README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.2.0/README.md)
for full details.
