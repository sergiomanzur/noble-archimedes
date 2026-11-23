# Noble Archimedes v1.1.0 — Mali GPU Support & Native Vulkan 1.1 for Android

Noble Archimedes **v1.1.0** brings **ARM Mali GPU support** to Android by
replacing `vkd3d 2.1` with a direct native **Vulkan 1.1** renderer and
in-process `glslang 16.0.0` HLSL-to-SPIR-V compiler. All downloads are
bring-your-own-game: no disc image, game executable, game assets or generated
game code is included.

> **Desktop users (Windows & Linux):** Windows x64 and Linux x86_64 binaries
> are unchanged from **v1.0.0** and remain available on the
> [v1.0.0 release page](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0).
> Only the Android arm64 APK and companion source archive were rebuilt for
> v1.1.0 (so v1.0.0 desktop auto-updaters stay up to date cleanly, while v1.0.0
> Android installs will automatically detect and offer this v1.1.0 APK update).

## Downloads

- **Android arm64 (new):** `Noble-Archimedes-1.1.0-Android-arm64.apk`
- **Source:** `Noble-Archimedes-1.1.0-Source.zip`
- **Windows x64 (unchanged):** download `Noble-Archimedes-1.0.0-Windows-x64.zip` from [v1.0.0](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0)
- **Linux x86_64 (unchanged):** download `Noble-Archimedes-1.0.0-Linux-x86_64.AppImage` from [v1.0.0](https://github.com/sergiomanzur/noble-archimedes/releases/tag/v1.0.0)
- Verify every download with `SHA256SUMS.txt`.

## Highlights in v1.1.0

### ARM Mali GPU support & direct native Vulkan 1.1 renderer on Android

- **We now support ARM Mali GPUs** (including budget 4 GB devices like
  Dimensity / Mali-G57 phones that previously crashed immediately at launch),
  alongside Qualcomm Adreno and other mobile Vulkan GPUs.
- **No `vkd3d` on Android:** replaced `vkd3d 2.1` with a direct native Vulkan
  1.1 `gpu::Backend` (`vulkan.cpp`) and static `glslang 16.0.0` HLSL-to-SPIR-V
  compiler (`vk_shader.cpp`), plus native `ImGui_ImplVulkan` for the in-game
  settings overlay. The APK now ships only `libSDL3.so` and `libmain.so` (both
  16 KB page-aligned, zero `libvkd3d*.so`).
- **Mobile Vulkan 1.1 fallbacks & Mali driver compatibility:**
  - Automatic `VkRenderPass` + `VkFramebuffer` and `VkFence` fallbacks when
    Vulkan 1.3 dynamic rendering or timeline semaphores are unavailable.
  - CPU DXT1 / DXT3 / DXT5 → BGRA8 decoding when hardware BC texture
    compression is absent on mobile GPUs.
  - Mali-safe SPIR-V generation (entry point `"main"`, single 12-binding
    descriptor layout, `SV_ClipDistance0` stripping, `PointSize` builtin, and
    opaque `Texture*`/`Sampler*` specialization avoiding VUID 06924).
  - Ignores `VK_SUBOPTIMAL_KHR` (preventing per-frame swapchain teardown on
    Android), avoids `VK_EXT_extended_dynamic_state`, checks per-format linear
    filtering, and skips rejected pipelines cleanly.
  - Preserves all Castlevania renderer extensions: widescreen, HUD edge
    pinning, menu spread, overscan fix, reversed-Z depth, and HD texture packs
    with material maps.

### 4 GB RAM & reliability improvements on Android

- **Lower memory footprint:** pooled 32 MB Vulkan device memory allocator,
  bounded 256 MB / 1024-entry LRU texture cache eviction, direct swapchain
  presentation, persistent `shadercache/*.spv` + `pipelines.bin` caching, and a
  4 GB-friendly first-run default render scale (`render_percent = 67`,
  adjustable in Settings).
- **Removed obsolete 60 s cold-boot watchdog:** removed the legacy
  `present.cpp` black-screen restart timer that could terminate slow first-run
  shader compiles on Android 14+.
- **Crash & LMK diagnostics (`CrashLogs.java`):** automatically saves Android
  11+ `ApplicationExitInfo` native tombstones, Java crashes, ANRs, and
  low-memory (LMK) kills into `files/logs/crash_*.txt` on the next launch.

## Android notes

- **Requirements:** Android 8.0 (API 26) or later, `arm64-v8a`, Vulkan 1.1+
  (ARM Mali, Qualcomm Adreno, etc.).
- **First run:** opens Android's file picker for your Castlevania: Curse of
  Darkness `.iso` / `.xiso` disc image (keep ~3.5 GB free). Upgrading in place
  from v1.0.0 preserves your copied disc image, settings and saves.
- Sideload build signed with the project's local debug key.

See [`android/README.md`](https://github.com/sergiomanzur/noble-archimedes/blob/v1.1.0/android/README.md),
[CHANGELOG.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.1.0/CHANGELOG.md)
and [README.md](https://github.com/sergiomanzur/noble-archimedes/blob/v1.1.0/README.md)
for full details.
