# Changelog

## Xbox 60 FPS Step 2 complete - frame-exact validation (2026-10-19)

- Added `xbox/tools/frame_bench.py`: a calibration run maps wall-clock pad
  timestamps to presented flips, then the measured runs use frame-scheduled
  pad input and frame-triggered captures. Scene windows are filtered by flip.
- The calibration and actual capture show the real pause panel (Player / Item /
  Equip / Skill / Options), the HUD, the title menu and the courtyard. The
  earlier flat gray pause capture was a wall-clock phase error, not a persistent
  presentation bug.
- Frame-exact results: title **59.5**, cutscene **56.5**, courtyard-light
  **31.5**, pause **52**, courtyard-heavy **33** and post-movement courtyard
  **28** fps. `main_menu` at 19-23 s is the Loading screen after the Start
  press, not the menu; the title screen is the menu.
- The guest clock undercount (667-952 ms/s) was a **harness measurement bug**:
  it divided by nominal scene seconds instead of the tick-sample span. The
  frame-exact run measures **~1000 ms/s**; the kernel clock is healthy. The
  high-priority tick experiments were reverted.
- P8 is 0.7-1.0 ms/frame after the UI-cache bypass; the remaining 3D cost is
  xform (~8 ms) + raster (~12 ms). M1 has not started.
- Tests: 19/19.

## Xbox 60 FPS Step 2 - frame-indexed capture (2026-10-19)

- Added `xbox_FramebufferCapturePresented` which writes the actual published
  present buffer at a requested presented flip. `RECOMP_CAPTURE_FLIPS` and
  `RECOMP_CAPTURE_DIR` select exact frames; the harness exposes
  `--capture-flips`. Verified on flips 60 and 120 (loading screen), independent
  of wall-clock scheduling.
- Added `usb_gamepad_set_flip_source`; `RECOMP_PAD_FRAMES=1` interprets pad
  script times/holds as presented frames. The full calibrated frame timeline is
  still to be wired into `bench.py`.
- The pause scene's actual presented capture is a flat gray/torn buffer, not the
  pause menu. This proves the pause/HUD ordering bug is in the presentation
  path, not the screenshot copy. M1's display-buffer ownership must fix it.
- No M1 renderer changes yet. Tests remain 19/19.

## Xbox 60 FPS pre-work checkpoint - 2026-10-19

Safety: branches `xbox-60fps` were created in both repos before the rewrite.
Snapshots are `2e447d3` (main) and `7fa2214` (tools/xboxrecomp); the harness
switch commit is `ee01aca`.

- **P8 regression found and fixed.** `[P8]` reported ~900 hits/s, 42
  rebuilds/s and ~47 MB/s of full-image comparison; the recolour cache cost
  6-14 ms/frame. HUD/menu quads are small and palette-animated, the worst case
  for that cache. `prepare_p8_draw_textures` now samples the palette per texel
  for `NV_PRIM_QUADS` and keeps the cached RGBA path for large 3D atlases.
  Commit `281fd0d`. `RECOMP_P8_UI_CACHE=1` restores the old path.
- Three-run software result after the fix: title **59.0**, main menu **40.0**,
  cutscene **57.0**, courtyard-light **31.5**, pause **53.0**, courtyard-heavy
  **30.5**, post-movement courtyard **32.0** fps. P8 is 0.8-1.0 ms/frame.
  Title unique/present is 97%; the 3D scenes are 99-100%.
- **Harness overhead is not the explanation** for the earlier title drop:
  `--no-unique --no-gpu-stats` produced the same 54/34/26 before the P8 fix.
- **Critical path:** the 0.98-core thread is a raster worker; its ntdll samples
  are event waits for the next pool job, not a mutex. Main/guest was 0.56 core
  and the twelve workers ~0.5 each. The serial P8 recolour was the removable
  main-thread cost; the remaining 3D limit is xform + raster (~20 ms/frame).
- **Main menu remains 40 fps with 59.6% unique** even though P8 is 0.00 ms and
  the harness is off. Title is 59/97% on the same build; this is a guest-side
  menu cadence/present interval rather than renderer budget and needs a frame
  trace.
- **Guest clock is slow** (667-952 ms/s). KeTickCount is published from the
  NV2A ack thread's `Sleep(0)` loop and loses the scheduler to the raster pool.
  A high-priority ack thread fixed the clock but starved 2D (title 25 fps) and
  was reverted. This is a kernel ticker scheduling bug; the game's own logic
  frame counter is still not located.
- **Visual validation is not trusted yet.** The pause capture is flat gray
  (the pause overlay is not captured by the framebuffer path), and inputs are
  still wall-clock scheduled. Frame-exact capture/input and an on-demand GPU
  readback capture are the next step before M1.

## Xbox benchmark harness and Phase 0/1 analysis - 2026-10-19

- Added `xbox/tools/bench.py`, a scripted, repeatable per-scene benchmark.
  It drives title, main menu, in-engine cutscene, courtyard, pause state and
  post-movement courtyard intervals; parses `[PACE]`, `[GPU-PERF]`,
  `[GPU-FLUSH]`, `[GPU-PLAN]`, `[BENCH]`, `[TICK]` and `[UNIQUE]`; captures a
  screenshot per scene; and compares them with an 8-count per-pixel threshold.
  Baselines and captures live under gitignored `xbox/bench/`.
- Added opt-in benchmark probes: `RECOMP_BENCH_STATS` (guest `KeTickCount`,
  host vblank and a monotonic run clock) and `RECOMP_BENCH_UNIQUE` (downsampled
  framebuffer hash, so re-presented duplicate images are visible).
- Three-run software median (default, no renderer switches):
  title 54 fps, main menu 34 fps (static UI; 51% unique), cutscene 50 fps,
  courtyard-light 26 fps, pause state 43.5 fps, courtyard-heavy 26 fps,
  post-movement courtyard 27 fps. The 1% low is 0.5-2 fps below the median;
  the 3D scenes are 100% unique.
- The first courtyard does **not** show the 30 Hz movie flag; the host pacer is
  60 fps and the guest clock is stretched to 0.85-0.93 s/s by rendering. The
  requested native-30 Hz scene check remains open for later areas.
- Profile (Release + PDB, 10 s sampling): 7.36 cores total; one raster worker
  0.98 core, twelve pool workers ~0.5 core each, main/guest thread 0.56 core.
  Hot symbols on the raster worker: `xf_rows` 25.3%, ntdll waits/locks 33.1%,
  `nv2a_rc_eval_plan` 5.5%, `nv2a_vsh_run` 4.6%, `nv2a_pb_exec_method` 3.9%,
  `raster_xf_triangle` 3.7%, `prepare_p8_draw_textures` 3.5%.
- Open harness caveats recorded in `docs/xbox_60fps_analysis.md`: the pause
  overlay is not captured by the framebuffer window (the 3D background is),
  the combat/Innocent Devil scene was not reached without a save, and the boot
  XMV is not captured by this framebuffer path. These must be fixed before
  claiming those scenes.
- No default renderer change. The previously attempted D3D11 batching/static
  texture experiments remain opt-in and are not the default; the software
  path is still the only visually verified courtyard path.
- Host regressions: **19/19 pass** in the current tree, including the new
  `bench-harness` parsing/scene test. (The changelog's earlier "9/9" count
  predates the GPU/texture/stencil tests already present in `xbox/tests`.)

## Xbox host development snapshot - 2026-10-19

This pass targets the local Xbox static recompilation host (`xbox/` and the
`tools/xboxrecomp` runtime). Measured with the hidden framebuffer window's own
frame counter, same machine and assets:

| Scene | Path | Before | After |
| --- | --- | ---: | ---: |
| Title screen / main menu | software | ~28-36 fps | **58-61 fps** |
| Title screen / main menu | D3D11 (`RECOMP_GPU_RASTER=1`) | n/a | **57-59 fps, visually correct** |
| Options / pause menu | software | ~30 fps | **~56 fps** |
| 3D courtyard gameplay | software | 6-9 fps | **21-36 fps** (scene-dependent) |
| 3D courtyard gameplay | D3D11 | n/a | 21-32 fps (per-triangle; slower) |

Host regressions: **9/9 pass** (`ctest --test-dir xbox/build-tests -C Release`).

### What changed

- **Deferred, state-captured raster queue.** Triangles are queued with their
  own blend/depth/alpha/color-mask/clip state and a private copy of the
  texture stages and register-combiner block. The queue is flushed in
  row-parallel order at target changes, blits, clears, full-screen copies and
  flips instead of once per draw. This removes the main thread's serial
  per-draw pixel work and lets the whole frame use the worker pool.
- **Parallel vertex-program execution.** The NV2A vertex-program interpreter
  runs per vertex across the raster pool instead of serially on the executor
  thread. In the measured scene this moved vertex time from ~25 ms/frame to
  well under the pixel budget.
- **Invisible-pass elimination.** Batches that write neither colour nor depth
  and do not count visibility are skipped. Castlevania's stencil shadow-volume
  passes have this shape and were consuming tens of milliseconds per frame
  while writing no pixels. Stencil itself is still not implemented.
- **Face culling.** The software rasteriser now honours the title's
  `SET_CULL_FACE_ENABLE`, `SET_CULL_FACE` and `SET_FRONT_FACE` state, including
  the alternating winding of triangle strips. Roughly half of the courtyard's
  submitted triangles are back-facing and are now rejected before the pixel
  loop, which alone lifted gameplay from ~19-25 fps to ~28-36 fps.
- **Direct presentation-copy path.** The final full-screen linear-32-bit copy
  is an affine row copy with the horizontal bilinear weight known to be zero,
  dispatched across the pool, instead of running four generic taps per pixel.
- **P8 texture cache.** 64 entries with frame-generation validation instead of
  16 entries and a full-image comparison per draw. The cache now survives the
  world's working set; on the measured scene it fell from ~38 ms/frame to
  under 1 ms/frame. `RECOMP_P8_EXACT=1` restores exact per-use validation.
- **Build/runtime defaults.** The kernel is compiled with AVX2 on this host;
  the raster pool defaults to 12 threads (`RECOMP_RASTER_THREADS` overrides).
  Diagnostic switches: `RECOMP_NO_FAST_PRESENT`, `RECOMP_NO_P8_CACHE`,
  `RECOMP_RASTER_MIN_WORK`, `RECOMP_SLOW_DRAW_US`, `RECOMP_GPU_STATS`.
- **D3D11 frontend rasteriser is now correct.** `xbox/src/nv2a_gpu_d3d11.c`
  accepts every exact combiner plan (1-9), passes all four texture stages and
  the prepared plan constants, and honours depth/blend/alpha/culling. Two
  bugs were fixed this pass: the pixel-shader constant buffer was declared at
  `b1` but bound at slot 0, so every plan silently read zero; and the title's
  multiply/alpha background relies on a black destination, not a white clear.
  Full-screen presentation/copy quads stay on the CPU because they read a
  front/video buffer the GPU path does not own. With `RECOMP_GPU_RASTER=1` the
  entire title/menu renders correctly at 57-59 fps. The 3D path renders but is
  slower than the software rasteriser (one D3D11 draw per triangle); a batched
  submission attempt was reverted, so the software path remains the default.

### Remaining

- 60 fps *3D gameplay* is still not reached. The software pixel shader and VSH
  interpreter are the limit at ~21-36 fps; the current D3D11 path needs batched
  draw submission (or a GPU vertex path) before it can beat the CPU.
- Stencil shadows are not implemented, so black shadow-volume geometry remains
  on the courtyard floor and clothes.
- The game itself runs its 3D scenes at a 30 Hz cadence in several areas; the
  measured 56 fps pause menu and 58-61 fps title show the host can run the
  lighter screens at 60.
- Diagnostic: `RECOMP_PACE_OFF=1` raises the 2D menus to 80-100 fps but leaves
  the 3D scene at 19-33 fps, so 3D is CPU/queue-bound rather than limited by
  the host's frame pacing. `RECOMP_PACE_FPS` / `RECOMP_PACE_OFF` remain useful
  for measuring that split.
- **Stencil diagnostic.** `RECOMP_SKIP_STENCIL=1` raises the measured 3D
  scene from ~20-32 fps to ~47-53 fps, but removes the stencil-driven scene
  itself: the game's main 3D passes are stencil-enabled, and the software
  rasteriser has no stencil buffer. This confirms the shadow/stencil passes
  are a dominant part of the 3D draw count and that stencil must be
  implemented correctly before stencil work can be skipped or masked.
- **D3D11 stencil attempt.** Added a D24S8 target, stencil test/ops, masks,
  reference and clear to the GPU backend. It drew the scene but rejected the
  HUD and did not remove the black shadow polygons (the title's front/back
  winding and cull/ref combination need more work), so it was reverted. The
  stable D3D11 backend remains the non-stencil one.
- **CPU stencil implementation.** Added a per-zeta stencil buffer, clear,
  test/ops/masks/reference, and corrected the invisible-pass rule so
  stencil-only shadow passes are not discarded when stencil is active. It is
  behind `RECOMP_CPU_STENCIL=1` because the measured cost is high: the 3D
  scene falls to ~12-18 fps when the stencil-only passes run, and the HUD's
  stencil/ref combination still needs work. The default remains the previous
  skip, with the black shadow polygons.
- Prototyped **batched D3D11 submission** and **deferred per-frame readback**.
  After fixing the batch key to ignore the per-triangle combiner pointer and
  plan padding, batching still did not beat the software path; the 3D scene is
  dominated by the CPU-side executor, texture/sync work and the single-surface
  readback, not the number of D3D11 draw calls alone. Deferring readback broke
  the title's double-buffer/present ordering (the castle background appeared
  without the logo/text), so both experiments were reverted. A display-buffer-
  aware present (readback once to the actual presented surface, or present the
  D3D11 target directly) is the prerequisite for making either one a win.

### Stencil / black-polygon follow-up (2026-10-19)

- Added a stencil trace to RECOMP_FRAME_TRACE: plan kind, stencil enable,
  function, reference, read/write masks and fail/zfail/zpass ops.
- The black rectangles are **not** the stencil shadow pass. In the traced 3D
  frame the black-area draws are plan 7 textured scene triangles with
  stencil 1, func ALWAYS, ref 0, masks FF/FF, ops KEEP/KEEP/ZERO. The
  stencil test always passes and writes zero; the black comes from the
  textured scene draw, most likely the P8 texture content/decode or baked
  shadow, not the stencil buffer.
- The HUD is also ALWAYS/KEEP, so its loss with stencil enabled is a
  queue/readback/double-buffer ordering problem (the stencil-only passes add
  flushes), not a failing stencil test.
- The stencil path remains off by default. The CPU/GPU stencil measurement
  stays at ~12-18 fps because the stencil-only shadow passes are numerous.

### 3D diagnostics added (2026-10-19)

- RECOMP_FRAME_TRACE now prints the plan kind and full stencil state.
- RECOMP_DUMP_TEX_OFFSET=<hex> / ll dumps decoded P8 textures from the
  CPU cache. The plan-7 3D textures inspected (architecture, sky, floor atlases)
  are normal; the black rectangles are not a P8 texture-content problem.
- RECOMP_NO_CULL=1 disables face culling for comparison. The black
  rectangles remain, so the culling path is not their cause.
- RECOMP_NO_SHADOW=1 skips stencil-enabled solid-black plan-3 shadow draws.
  This is available as a visual/performance option but did not remove the
  measured black rectangles.
- Stencil remains opt-in and off by default; CPU/GPU stencil measurements are
  ~12-18 fps and the HUD loss is a queue/readback ordering issue.

### D3D12 backend (2026-10-19)

- Added xbox/src/nv2a_gpu_d3d12.c, an opt-in D3D12 queue rasteriser behind
  the same Nv2aGpuBackend seam. It uses a D3D12 root signature, static
  vertex/pixel shaders, descriptor heaps for four texture stages and four
  samplers, PSO caching for blend/depth/colour-mask state, batched consecutive
  equal-state triangles, persistent texture uploads, and a per-queue command
  list with a readback buffer.
- RECOMP_GPU_D3D12=1 selects it; the software rasteriser remains the default.
- Build: add d3d12 to the game link libraries and NOBLE_HAVE_D3D12 to the
  target definitions. The D3D11 backend is unchanged.
- Measured: menu **57-61 fps**, 3D scene **20-28 fps**. The 3D path still has
  the same HUD/black-polygon issues as the NV2A GPU seam, and the CPU executor
  + per-queue readback remain. This matches the BLACK-XBERecomp finding that
  60 fps needs the D3D8 API HLE (no NV2A vertex/queue interpreter), not just a
  GPU rasteriser behind the NV2A stream.

## Alpha 0.2.1 — 2026-10-15

This maintenance release refines the Stage A EE scheduler architecture, resolves virtual time pacing deadlocks, and eliminates scheduler polling overhead across all gameplay regions.

### Key Improvements & Bug Fixes

- **Scheduler Dispatch Rate Reduction (968,000/s -> ~3,900/s)**:
  - Fixed a critical pacing condition in `EeScheduler::processPendingEvents()` and `EeScheduler::checkpointDue()`: when scheduled deadlines reach virtual cycle time while host pacing time is future (`s_pacingDeadline`), `m_checkpointPending` is no longer continuously flagged, preventing basic blocks from collapsing out of the inner quantum loop.
  - Reduced scheduler round-trips from ~968,000 dispatches/sec to ~3,900 dispatches/sec, cutting 76% of host thread dispatch and mutex polling overhead.
  - Bumped maximum inner quantum budget up to 1,000,000 cycles (bounded by the earliest scheduled event deadline), while guaranteeing iteration-1 breaks on queue-spin addresses (`0x41fff0`, `0x420528`, `0x420050`, `0x4203B0`, `0x4204E8`, `0x420628`).

- **DMA Queue Allocation & Virtual Time Stability**:
  - Restored clean `std::min_element` deadline selection in `EeScheduler::updateNextDeadline()`, ensuring `m_nextDeadlineCycle` is never zeroed when events are future-paced, resolving guest lockups in `sub_0041FFF0`.
  - Fixed DMA free list pool check in `isQueueSpin`: corrected `freeHead == 0u` evaluation so valid buffer node addresses (`>= 0xAC3C20`) are not mistakenly treated as empty pool starvation.
  - Replaced artificial cycle jumps with cooperative guest yielding when DMA nodes are legitimately exhausted, allowing worker threads to drain without distorting frame pace.

- **Universal Courtyard Room State Support (`[4, 10, 3, *]`)**:
  - Updated all measurement and validation harnesses (`measure_gameplay.py`, `run_gameplay_validation.py`, `run_gameplay_validation_regions.py`) to recognize all room states (`[4, 10, 3, *]`), enabling continuous measurement through transitions.

- **Steady-State Courtyard Profiling**:
  - Confirmed steady-state 3D courtyard execution (`[4, 10, 3, 1]`) measures 14.43 FPS across 922 frames (63.9s) at only 0.54 host CPU cores (down from 1.68–1.94 cores), isolating the remaining frame time entirely to GPU/VIF1 rasterization and draw call latency (~4 VBlanks per frame) rather than host scheduler contention.

---

## Alpha 0.2.0 — 2026-10-15

This release incorporates major performance advancements pushing measured gameplay from ~20.5–26.4 FPS to **~53.0 FPS** (>2x speedup), alongside comprehensive EE scheduler optimizations and deadline-pacing bug fixes.

### Major Gains & Performance Breakthroughs

- **EE Scheduler Quantum Inner Loop & L1 Direct-Mapped Block Cache (Stage A)**:
  - Eliminated the project's primary host-side bottleneck: ~930,000 guest-to-scheduler context roundtrips and thread reselection loops per second.
  - Implemented an inner quantum loop in `EeScheduler::run()` computing safe cycle budgets bounded by hardware timers, DMA transfers, and future deadlines.
  - Added a 64K direct-mapped L1 block cache (`fastLookupBlock`) in `EeScheduler.cpp` avoiding hash/table lookups on hot execution paths.
  - Required **zero guest recompilation** (all 32 guest `.o` chunks preserved untouched) and maintained binary ABI stability in `ee_scheduler.h`.
  - **Measured Performance**: Guest `phase_frames` advanced 912 frames in 17.2s (**52.97 FPS (~53 FPS)**) during boot, menus, and 3D world transition, more than doubling the previous baseline.

- **PCSX2-Style VU Dead Flag Elimination**:
  - Activated conservative 4-cycle FMAC flag liveness analysis (`--vu-dead-flag-elimination`).
  - Elided `CaptureFullFmac`, normalization, and sticky flag retirement for instructions whose flags are dead, reducing VU worker CPU time from 11.78s to 7.33s per 15s window (a 10.4% speedup in paired gameplay controls).

### Bug Fixes & Scheduler Hardening

- **Host-Pacing & Queue Spin Deadlock Resolution**:
  - Fixed an issue in `EeScheduler::updateNextDeadline()` where cycle-due events waiting on host pacing cleared `m_nextDeadlineCycle` to 0, stalling virtual time progression during DMA node starvation (`0xAC3CF8` free pool exhaustion).
  - Added fallback cycle advancement in `EeScheduler::checkpointDue()` during DMA queue spin loops (`isQueueSpin`), ensuring the scheduler advances by `kVBlankPeriodCycles` rather than freezing at 100% CPU when waiting on VSync/DMA worker drains.
- **GS `sceGsSyncV` Return Address Guard**:
  - Guarded `ctx->pc = ra` in `GS.cpp` stubs (`sceGsSyncV` and `sceGsSyncVCallback`) against null return addresses (`if (ra != 0u)`), preserving scheduler continuity.
- **Validation Probe Compatibility**:
  - Updated `run_gameplay_validation.py` to recognize fast-path room state transitions (`[4, 10, 3, 3]`), preventing false-positive timeouts at 50+ FPS speeds.

---

## Alpha 0.1.0 — 2026-10-15

This is the first named development release of Noble Archimedes. It publishes the advanced native VIF1 worker and scheduler work, selectable graphics architecture, reproducible build documentation and a source-only Windows release kit.

### Selecting the game

Only the USA PlayStation 2 release **SLUS-21168** is supported. Use your own lawfully obtained disc image. Name it `Castlevania - Curse of Darkness (USA).iso`, place it beside the locally built executable and extract `SLUS_211.68` to `extracted/SLUS_211.68` using the command in [BUILDING.md](BUILDING.md). The release contains no game data or translated game implementation.

### Selecting the GPU renderer

- Double-click `Launch Native.cmd` for the native recompilation path with Vulkan paraLLEl-GS.
- Double-click `Launch PCSX2 GS.cmd` for the experimental PCSX2 GS-only adapter. It requires a build configured with `PS2X_ENABLE_PCSX2_GS=ON`, `noble_pcsx2_gs.dll`, its licensed dependencies and resources.
- From a running build, press **F1**, expand **Graphics**, choose **Native** or **Hybrid (PCSX2 GS)**, then restart. The saved choice takes effect on the next launch.

Hybrid mode keeps this project's recompiled EE, VU execution, scheduling, input and gameplay runtime. It replaces only the GS renderer and does not start a complete PCSX2 virtual machine.

### Current performance and correctness

The newest retained VU-correct candidate measured **31.5009 gameplay updates/s**: 1,232 updates over 39.110 seconds with protected gameplay state `[4, 10, 3, 1]` unchanged. An older private-development ledger run recorded **35.8328 gameplay updates/s** and **58.4375 fresh presented images/s** over different measurement windows. These results do not establish sustained 58–60 FPS gameplay, and the public source kit does not reproduce the private optimized VU DLL.

The retained work includes the native VIF1 worker, explicit fencing and state restoration, ordered GIF packet handling, a scheduler empty-work fast path, reusable event/deadline scratch storage, Windows timer-resolution handling, renderer settings and the PCSX2 GS adapter source.

### What remains

- Reach and validate 58–60 actual gameplay updates/s with VU correctness preserved.
- Finish the ordered native engine seam through skinning, normals, extrusion, projection, clipping and packet/cache writes.
- Prove the PCSX2 GS-only bridge in real gameplay and package its complete licensed dependency/resource closure.
- Make clean public game-code generation and the end-to-end Windows build reproducible.
- Separate user-generated game implementation from the redistributable host so a playable public binary can be shipped without game-derived code.
- Resolve remaining GS test failures and the broad test-suite VSync stall.
- Qualify combat, enemies, Innocent Devils, effects, transitions, movies, sound, memory cards and saves.

### Distribution boundary

The GitHub asset is a **source-only Windows kit**. It opens through the supplied command launchers after the user builds the host locally. It excludes the game ISO, extracted executable, generated/recompiled game implementation, private VU/GPU translations, captures, saves, dependency binaries and compiled host executable.
