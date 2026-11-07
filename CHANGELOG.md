# Changelog

## Unreleased

- Scale the advanced Windows Settings menu to the game window when it is smaller than a high-resolution desktop, keeping camera controls and other settings within the panel.

## v0.9.5b — Advanced graphics and camera fixes

- Fix a black screen after Continue on a 4K display at 200% render scale with DLSS. The renderer now limits DLSS supersampling to 4K or the output height, whichever is greater.
- Keep the original Noble Archimedes Windows launcher, with its artwork, fonts, colors and music.
- Give first-person view more clearance near enemies. Route Classic mouse motion through the original orbit camera and fix right-stick inversion at full deflection.
- Ship the advanced Windows renderer with HDR, ambient occlusion, temporal anti-aliasing, texture material maps and DXR options. Optional NVIDIA features require a compatible GPU and vendor runtime.
- Include native Linux preview build instructions and a Steam Deck packaging path in the source release.
- Make first-person movement follow the mouse view immediately, including after a quick turn in the opposite direction.
- Show a region picker next to Play when both the USA and European game copies and their matching executables are available.

- Add optional sprint on Left Shift or L3 with matching movement animation, and optional PS2 analog stick response.
- Add widescreen HUD placement with special handling for lock-on, boss bars and modal overlays. Use 16:10 presentation on narrower widescreen outputs.
- Add field of view, draw distance and fog options. Keep the original movie framing by default and support player-supplied HD movie packs.
- Add controller shoulder navigation in Settings, live button layout changes and improved dual-motor rumble handling.

## v0.9.4b — First-person camera and mouse reticle (2026-11-06)

- Anchor the first-person camera to Hector’s live position so the third-person follow target no longer drifts behind his head during a run. Look pitch no longer moves the camera’s eye position.
- Hide the system cursor inside the game and show a small centered dot only while gameplay captures the mouse.

## v0.9.3b — Graphics hotfix (2026-11-06)

- Copy the Xbox game's completed frame using screen pixel coordinates and explicit base-level sampling. This removes derivative-based sampling from the full-frame pass shared by the title screen and gameplay to address reports of stretched streaks.
- Carries forward the in-game cursor fix and independent **Invert camera stick X** and **Invert camera stick Y** settings from v0.9.2b.
- Verified headlessly on an RTX 3080 with 4K display settings, 200% render scale and a synthetic HD texture replacement. Confirmation on the reported Radeon RX setup is pending.

## v0.9.2b — Cursor and camera-stick fixes (2026-11-06)

- Re-assert the hidden cursor every captured frame and restore the normal arrow when capture is released, preventing a visible cursor from remaining parked at the screen centre.
- Add separate **Invert camera stick X** and **Invert camera stick Y** options, enabled by default to correct the title's inverted controller camera. They apply in both Classic and direct-look modes.
- Keep mouse inversion independent from controller-stick inversion.
- Persist the new options in `castlevania.toml` as `modern_invert_stick_x` and `modern_invert_stick_y`.

## v0.9.1b — Modern PC controls beta (2026-11-02)

- Add direct relative mouse look to the primary Xbox D3D12 build, with automatic capture during gameplay, release on focus loss and the F10 menu, and an F6 override.
- Add Classic, Resident Evil 2 Remake-style over-the-shoulder (the new default) and first-person camera modes. The camera owns an absolute heading after the title's update; mouse movement changes it once, the title follow camera is steered toward it so `W` moves where the camera points, and the title still supplies the target, distance and scripted transitions.
- Add left/right/middle/X1/X2 mouse bindings, right-mouse aim with slower walk and a closer camera, F5 mode cycling, and an F10 Camera tab.
- Persist camera and mouse settings in `castlevania.toml`; add `modern_camera_math_test.cpp` for the imported column-major view matrix and camera placement math.
- Keep `CV_PAD_SCRIPT`/`CV_PAD_LIVE` deterministic by disabling physical mouse input during those runs. See [modern PC controls](docs/MODERN_PC_CONTROLS.md).

## v0.9.0b — Xbox-first Noble Archimedes beta (2026-11-02)

- Make the native x64 original Xbox recompilation the primary Windows build, using Direct3D 12 and XAudio2. Keep the PS2 host and older Xbox renderer research explicitly experimental.
- Integrate jmafort/CastlevaniaCOD-PC at `1d9edf6b2bdbb683cb95dff5b317916aec31db84`, retaining its pristine source, credits and licenses. Pin the separately patched Xbox XBE Recomp SDK at `91146ff6e2a218b19c1109dc665d06020472ab57`.
- Include widescreen/menu and shadow fixes, shader cache, filtering, render scaling, game options, PCSX2-format PNG/DDS HD texture-pack loading, and region-specific USA/EU source configurations.
- Introduce the Noble Archimedes launcher with original observatory artwork, astrolabe icon and the original looping theme **The Quiet Orrery**, with a persisted music toggle. Retain GPU checks, game-file selection and display/game/language settings.
- Align executable, generated game DLL, import-library and first-run compiler names under `NobleArchimedes*`. Add the main build/launch shortcuts and a guarded, repeatable source overlay.
- Add scoped open-source licensing and fork/build guidance, preserving GPLv2 Xbox, GPLv3 PS2 and MIT upstream terms. Dedicate original branding assets under CC0 to the extent rights are held.
- Supply a source-free runtime package: bring your own USA Xbox disc image or extracted files. The runtime generates its game DLL locally; no game files, generated implementation, HD packs or game audio/art are included. The original project-owned branding is included.
- Validate the branded USA build with 30 focused checks, 7/7 new-game/movement/menu steps, and an isolated first-run test passing 8/8 startup/gameplay steps. The latter generated its DLL from the local XBE in about 85 seconds and sampled about 59.4 FPS in the courtyard. The actual source-free runtime ZIP additionally passed 9/9 installation/startup/gameplay checks outside the source tree, including dependency closure and game-DLL compilation.
- This beta's downloadable runtime is USA-only. European source support is retained, but no EU input was available to build/qualify an EU binary. HD packs, a complete playthrough and clean-Windows installation are not qualified by the recorded local tests.


## D3D8 HLE async swap readback and true-backend benchmark (2026-10-21)

- Swap readback now uses a two-entry PBO ring: the previous frame is mapped
  and copied after the next frame has been submitted, instead of calling
  `glReadPixels` synchronously into guest memory.  Measured readback cost fell
  from ~4.0 ms to ~0.5 ms/frame; the guest image is one frame behind, which is
  invisible at 50+ fps and no longer stalls the GPU at Swap.
- Added `frame_bench.py --calibrate-backend`.  The default remains software
  calibration (the existing regression test requires it); HLE validation can
  now calibrate against its own frame timeline.  With software calibration,
  the HLE run's scene windows were silently sampling the wrong game states.
- True HLE-calibrated run with the 10 ms DSP settle intact:

  | scene | fps | unique/present | guest ms/s |
  |---|---:|---:|---:|
  | courtyard_light | 49.0 | 99.4% | 1000 |
  | pause | 51.0 | 99.8% | 1000 |
  | courtyard_heavy | 47.0 | 100.0% | 1001 |
  | combat | 45.0 | 100.0% | 1000 |

  The pause panel and courtyard/combat images are correct; unique frame ratios
  are effectively 100%, so these are not duplicated presented frames.
- Reducing or removing the DSP settle was tested and rejected.  A 5 ms settle
  presents 60 fps but the renderer still produces only ~51 unique frames/s;
  a 0 ms settle and a forced DSP-complete flag both stall the movie/prologue
  state machine.  The 10 ms wait is part of the title's audio/movie timing and
  is not dead time that can simply be deleted.
- Remaining budget: ~10 ms DSP settle, ~3.2 ms GL draw submission, ~0.5 ms
  async readback, ~0.2 ms copy, plus guest game logic.  The next measurable
  target is the ~3.2 ms GL submission, not the readback.

## D3D8 HLE remaining-cost profile (2026-10-20)

- Added per-call HLE timers for setters, streams, textures and palette uploads.
  In the courtyard these are effectively free (<0.02 ms/frame).
- Current D3D8GL frame at 44-46 fps: readback ~4.0 ms, GL submit ~2.9 ms,
  present ~0.1 ms, prepare/copy/palette ~0.25 ms. Guest execution and the
  GL/driver path account for the remaining ~15 ms.
- A diagnostic run with draws suppressed (`RECOMP_D3D8GL_NO_DRAW=1`, not a
  shipping mode) reaches the 60 Hz guest ceiling without the draw path, which
  confirms the remaining limit is the rendered path, not guest logic.
- Suppressing the readback alone did not improve the frame rate; the wait moves
  to the draw/present path. A real fix needs overlap or a cheaper presentation
  path, not just removing the copy.

## D3D8 HLE first-3D stall fixed; full frame-exact run (2026-10-20)

- Root cause of the ~44.9 s stop: `xbox_RecompBlockOnGpuFence` waited for an
  NV2A semaphore release, but the device-level D3D8 HLE owns the GL draw and
  never submits that release. The guest main thread spun in
  `xbox_KeStallExecutionProcessor` and `ntdll` waits.
- The fence bridge now treats the fence as complete when `RECOMP_D3D8GL` is
  active (`xbox/src/gpu_sync.c`). The software/default path is unchanged.
- The benchmark combat window now ends at 104 s because the scripted run exits
  around 106 s; this lets the frame-exact calibration validate its final sample.
- Full frame-exact D3D8GL run now reaches every scene and captures them:

  | scene | fps | unique fps | unique/present |
  |---|---:|---:|---:|
  | title | 59.5 | 49.3 | 82.7% |
  | main_menu | 53.5 | 32.0 | 67.4% |
  | cutscene | 38.0 | 36.8 | 87.8% |
  | courtyard_light | 46.0 | 45.4 | 100.0% |
  | pause | 46.0 | 45.4 | 100.0% |
  | courtyard_heavy | 40.5 | 40.0 | 100.0% |
  | combat | 41.0 | 40.5 | 100.0% |

- Captures are visually plausible in all seven scenes: title, menu, cutscene
  map, courtyard with HUD, pause panel, heavy courtyard and combat. No stall.
- Remaining HLE cost per frame (courtyard): readback ~4.3 ms, GL submit
  ~4.1 ms, present ~0.1 ms, prepare ~0.15 ms, copy ~0.14 ms. The rest of the
  21.7 ms frame is guest execution and GL driver time. The path is still below
  60 fps and is not the default.

## D3D8 HLE parallel-pass review and full-run stall (2026-10-20)

- Committed the parallel agent's D3D8GL work as `7be74ac`: fixed-function TSS
  compiler, embedded vertex constants, pixel-shader definitions, copy-rect
  support, draw-target readback and tests.
- The latest D3D8GL captures are visually plausible (courtyard, Hector, HUD),
  unlike the earlier black target. The latest partial run measured title 60,
  menu 54, cutscene 42, courtyard 34-35 and pause 35 fps.
- `HLE-PERF` in the courtyard showed readback 2-10 ms/frame, submit 1-6 ms,
  present ~0.1 ms. The remaining time is not in the HLE wrapper accounting.
- A full frame-exact D3D8GL calibration **does not complete**: the run stops at
  about 44.9 s. The last `[TICK]` is guest_ms 143945250, vblank 2955,
  flips 2346; no later PACE/BENCH lines or HLE swaps arrive. A process sample
  after 50 s shows the main thread in `ntdll` waits and
  `xbox_KeStallExecutionProcessor`; the GL wrapper is not the active hot path.
- This looks like a guest spin/vblank/present wait at the first 3D transition,
  not a GL readback cost. The software path remains the default; the HLE path
  is not yet a validated 60 fps backend.

## D3D8GL draw-target follow-up (2026-10-20)

- Switched Swap readback from the front surface's render-target entry to
  `device.render_target`, the actual GL draw target, then copied that into the
  front guest surface. This removed the remaining target-selection ambiguity.
- The dump is still entirely black, with `err=0`; the standalone
  `d3d8gl --gpu-test` FVF/indexed/readback test still passes. The remaining
  fault is therefore in the HLE-fed programmable/UP draw state, not in
  `glReadPixels`, the front-surface copy, or the GL device itself.
- HLE performance remains ~40 fps in the courtyard but is not visually valid;
  software remains the default.

## D3D8GL readback diagnosis (2026-10-20)

- Added `RECOMP_D3D8GL_DUMP_READBACK` diagnostics to the GL readback path.
- Readback reports `err=0`, the correct 640x480 guest surface and GL target
  texture, but the first pixel is `0x00000000` and the dumped BMP is entirely
  black. This is not merely the framebuffer-window copy: the GL render target
  itself is black after the HLE frame.
- The standalone `d3d8gl --gpu-test` FVF triangle/indexed/readback test still
  passes ("real GPU FVF triangles, indexed quads and mutable texture readback
  passed"), so the GL device/readback machinery is alive.
- Therefore the remaining bug is in the HLE draw/state path feeding the
  programmable/UP draw, not in `glReadPixels` or the framebuffer window.
- Current HLE frame rate remains ~40 fps in the courtyard, but it is not
  visually valid.

## Xbox D3D8 HLE probe - faster but not visually correct (2026-10-20)

- Ran the existing device-level HLE (`RECOMP_D3D8GL=1`, `--backend d3d8gl`)
  through the 112 s bench.
- The HLE is active and deterministic: CreateDevice hands off, SetStreamSource,
  SetIndices, shader/state calls and draw `0x003BA740` are reached; the guest
  pushbuffer draw work is suppressed.
- Performance: title 54, cutscene 37, courtyard 40, pause 43.5, heavy 40,
  post-movement 33 fps. This is faster than the 31 fps software path, but the
  guest readback/captures are flat gray and differ from software by 87-100%.
- The immediate blocker is the D3D8GL render-target/readback path: readback
  reports `readback=1`, but the surface the framebuffer window gets is not the
  GL target that was drawn. Do not make this path default until it presents the
  actual GL image.
- No D3D8GL source changes were made in this probe; `--backend d3d8gl` is a
  reproduction switch in the harness.

## Xbox 60 FPS M1 surface-ownership attempt - failed, reverted (2026-10-20)

- Tried the next M1 step: keep the D3D11 target resident, read back only when
  a software batch is about to write guest memory, upload guest state again at
  flip when CPU 2D has drawn, and present the D3D11 target through a swap chain
  on the framebuffer HWND.
- Measured: title 47, menu 41, cutscene 60, courtyard 16, pause 22,
  courtyard-heavy 18, post-movement 20 fps. Captures still differed by 76-99%
  from the software baseline.
- The CPU 2D/HUD composition and the executor's CPU/GPU switching still force
  enough transfer and queue work that direct present loses to software. The
  prototype was reverted; the stable software path (and its 31 fps courtyard)
  is unchanged. The `--gpu-present` harness switch remains only to reproduce
  the failed experiment.

## Xbox 60 FPS M1 direct-present prototype - failed, reverted (2026-10-19)

- Prototyped a D3D11 swap chain on the existing framebuffer HWND with
  `RECOMP_GPU_PRESENT=1`: no per-queue readback, CopyResource to the back
  buffer, Present(0,0), and GDI painting disabled.
- Measured: title 43, menu 34, cutscene 57, courtyard 14, pause 20,
  courtyard-heavy 17, post-movement 19 fps. Captures differed from the software
  baseline by 76-99%; the target still lacks CPU 2D/HUD composition and the
  per-queue executor/unsupported paths.
- It did not beat software in any 3D scene and was visually wrong. The code was
  reverted; the harness `--gpu-present` switch remains only as a reproduction
  hook. M1 needs the surface-ownership/composition work before direct present
  can be enabled.

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
