# Benchmark validity audit and bounded candidate gate

Date: 2026-10-22.  Scope: `xbox/` and `tools/xboxrecomp/` read-only.
No renderer, guest-runtime, frame-pacing, HLE/render-capture, shared build, or
PS2 source was edited.  No gameplay, build, profiler, or campaign was run.
Added files are listed at the end.

No `AGENTS.md` applies under `xbox/` or `tools/xboxrecomp/` (the only
`AGENTS.md` files are under `tools/rea/`, which is out of scope).

## 1. Inputs audited

- `output/qualified_checkpoint/{results.md,env.txt,runs.json,frame_pad.txt,
  run_hle_interactive.cmd}` and the three runs under
  `xbox/bench/out/frame_exact/20261021_194702_d3d8gl/`.
- `output/cpu_checkpoint/manifest.md`, `output/render_capture_report.md`,
  `output/rcap_audit_final/`, `output/rcap_audit_final2/`.
- `xbox/tools/{bench.py,frame_bench.py,summarize_runs.py,build_d3d8gl.cmd,
  run_hle_blink_repro.cmd}`; `xbox/src/{frame_pacing.c,render_capture.c,
  d3d8_hle.c}`; `tools/xboxrecomp/src/kernel/{kernel_hal.c,
  xbox_memory_layout.c}`; `tools/xboxrecomp/src/video/fb_present.c`.
- `docs/xbox_flicker_followup.md`.

## 2. What the current evidence does support

These parts of the qualified checkpoint reproduce from the stored artifacts:

- The frozen calibration (`E0FAE694...`) and frame pad (`30CE2B61...`) are
  byte-identical to the qualified run, and all three runs used the same pad.
- The scene boundary flips are reproducible from the calibration:
  `title 880-1060`, `main_menu 1108-1323`, `cutscene 1413-1777`,
  `courtyard_light 2912-3218`, `pause 3325-3760`,
  `courtyard_heavy 3971-4562`, `combat 4666-5491`.
- `framestats.csv` p50/p95/p99/max reproduce `results.md` exactly (for example
  run 1 courtyard_light: p50 18.28 ms, p95 21.09 ms, n=306; run 2
  courtyard_heavy: p50 20.13 ms, n=591).
- Gameplay frames are genuinely over the 16.667 ms interval: 100% of
  courtyard_light/heavy and combat-window frames exceed it.
- The 10 ms DSP settle is real and mandatory: `xbox_KeStallExecutionProcessor`
  busy-waits exactly the requested microseconds, and in gameplay every PACE
  second reports `dsp10 == frames` with `actual == requested` to within
  0.5 ms. No environment variable shortens it.
- HLE remains opt-in (`RECOMP_D3D8GL`), software remains the default, and
  blinking remains open. Nothing in the measured runs changed those defaults.

Only the frame-interval statistics above are used in this report.  No sustained
FPS and no cost attribution is carried forward.

## 3. Findings that invalidate parts of the current reports

### 3.1 The capture denominator mixes two counters and two windows

`output/rcap_audit_final2/probe.json` reports `frames=153`
(`start_flip=2678`, `end_flip=2831`).  `render_capture.csv` reports exactly
**132** events for each of `copy`, `readback`, `present`, `swap`, and `clear`,
and the manifest reports `texture_frame_range 2900..3032` (span 132).
`rcap_audit_final` shows the same shape: probe `frames=152`, capture span
`2931..3063` = 132.

The presented-flip counter (`xbox_FramebufferFlipCount`, used by
`[BENCH] flips=` and by `RECOMP_CAPTURE_FLIPS`) and the renderer frame counter
(`xgpu_texture_cache_begin_frame` -> `texture_frame`) are different counters.
`recomp_render_capture_update_window` is driven by the presented-flip counter,
and the HLE swap is once per presented frame, so 132 swap rows means the
capture-active window contained 132 presented frames by construction.  The
probe's 153 is `end_flip - start_flip` from wall-clock polling of `[BENCH]`
(one-second snapshots), i.e. a different window and a poll-boundary estimate,
not an exact capture count.  The artifact does not record which window each
numerator belongs to.  Therefore:

- the "153 presented frames" used as the denominator for
  `851,415,040 / 153 = 5.565 MB/frame`,
  `50,331,648 / 153 = 321.3 KB/frame`,
  `91.86 ms / 153 = 0.600 ms/frame`, and
  `14.87 MB / 153 = 97.2 KB/frame` is not supported;
- if those tables are retained at all they must name a single counter (the 132
  captured swap rows) and say "renderer frames", not "presented frames".  The
  current numbers are 13.7% lower than the same arithmetic over 132
  (132/153 = 0.863), but neither version is a measured cost;
- the capture is record-only.  There is no replay consumer, no full replay, and
  no input-equivalence check.  No worker/overlap saving can be inferred from
  it.

### 3.2 Per-bind renderer deltas were measured at SetTexture, not at draw consumption

`d3d8_hle.c` calls `recomp_rcap_renderer_bytes()` around `p_texture()` in the
`0x003B7820` SetTexture HLE.  In `rcap_audit_final2`,
`renderer_events == texture_binds == 11574` and
`renderer_content_bytes == 0`: the deltas are SetTexture-stage deltas, not the
draw-stage `bind_textures`/`xgpu_texture_get` reads that consume texture
contents.

The report's reconciliation to "~15.45 MB/frame" comes from `[TEX-PERF]`
texture-frame windows in a different run (`rcap_audit_final`, 120-240,
240-360, 480-600).  Those windows do not overlap that run's capture range
(2931-3063), and `rcap_audit_final2` has no `[TEX-PERF]` output at all.  The
reconciliation is therefore across different runs and windows and cannot be
attached to the capture accounting.

A draw-consumption measurement would need a counter at the draw-stage texture
lookup, in the same run and window.  No such measurement exists.

### 3.3 "FPS" and "1% low" columns mean something coarser than their labels

`bench.summarize_flip` and `bench.summarize` fill the `fps` list with the
one-second PACE `flips/s` counter.  `low1()` returns `min(fps)`, the slowest
one-second bucket, not a 1% frame percentile.  Consequences:

- `results.md` "fps median" is the median of ~1-second integer buckets
  (for example run 1 courtyard_light 54.5, run 2 courtyard_heavy 49.5).
- "presentation rate 49.5-55.5 FPS" is the range of those bucket medians.  The
  interval-derived aggregate `n / sum(frame durations)` over the same windows
  is 49.2-54.4.  Both are window statistics, not a sustained-rate claim.
- PACE `fps=60` is the pacing target, not a measurement (the flicker follow-up
  already notes this).

Do not propagate either number as "FPS"; use p50/p95 frame intervals and, if a
rate is needed, label it "interval-derived for this window".

### 3.4 Binary identity is asserted but not demonstrated

`env.txt` records the frozen exe `E9C7D678...` and DLL `CF6DAF2C...`, but:

- neither binary is present anywhere in the worktree.  The current
  `xbox/build/Release/curse_of_darkness_xbox_recomp.exe` is `68624F23...` and
  both `d3d8gl.dll` copies are `63996B28...` (the later texture-audit build);
- the qualified logs contain no `[D3D8HLE] module=` line (it requires
  `RECOMP_D3D8GL_TRACE`), so the loaded DLL path/hash was never recorded;
- `frame_bench.py` has no `--exe` argument and uses `bench.EXE_DEFAULT`;
  `bench.py --exe` is ignored by `frame_bench.py`;
- `build_d3d8gl.cmd` copies `output\d3d8gl.dll` to the repo root, and relinking
  changes the DLL hash even with identical source.

Load order is deterministic: `LoadLibraryA("d3d8gl.dll")` finds the executable
directory first (there is no DLL next to the build exe), then cwd (repository
root), then `output\d3d8gl.dll` as a fallback.  That explains the frozen run's
root-DLL load but does not prove it.  The 194702 qualified runs have no
per-run identity record, and the earlier 151918 benchmark remains unverified.

### 3.5 The diagnostic-off environment is only partially off

`bench.run_one` correctly strips inherited `RECOMP_*` variables, then adds
`RECOMP_BENCH_UNIQUE` and `RECOMP_GPU_STATS` unless told not to.  The qualified
runs therefore hashed every presented framebuffer and requested GPU stats, and
wrote seven 921 KiB BMPs inside the measured run (capture frames at flips
970/1215/1595/3065/3542/4266/5078).

Other unconditional output remains: about 12 `[DEBUG 0039C000]` lines, about
20 `[D3D8HLE] entry=` startup lines, and the three `[D3D8HLE] init` lines
(127,395 bytes of stderr on run 1).  These are startup-only and identical
across arms, but they are part of the I/O floor, not "off".

The host always `freopen`s `xbox_stdout.log`/`xbox_stderr.log` in the
repository root.  Concurrent runs would overwrite each other's logs, so host
runs must be serialized (the gate enforces a lock file for its own runs).

The three qualified runs were executed sequentially, not alternated, and the
host drifted: in the gate window below, run 2 is 1.13 ms slower than run 1 and
the three-run span is 1.23 ms.  The recorder-overhead check is one 165-frame
enabled/disabled pair; treat it as weak evidence, not a guarantee.

### 3.6 The capture comparisons are not an equivalence test

The `*_diff.png` files written by the qualified run compare 2026-10-21 HLE
frames against the 2026-10-20 `xbox/bench/baselines/*.bmp` (a different build,
backend, and input run).  `compare_baseline` also writes into that shared
directory when `--update-baseline` is used.

A single presented BMP at one flip cannot establish equivalence for an
animated scene with blinking still open.  The earlier same-binary comparison
showed 0.47-2.01% differing pixels between baseline and candidate runs.  Any
capture gate must compare a matched pair at the same three consecutive flips
from the same frozen input and use baseline-vs-baseline as the null.

## 4. Baseline snapshot requirements

A candidate result is only comparable to a baseline that satisfies all of the
following.  Record them in `xbox/bench/out/gate/snapshot.json` (the tool writes
this file) and keep the bytes, not just hashes.

1. **Worktree snapshot.**  `git rev-parse HEAD` =
   `46001e017694a768c49d4c6dfd4904cbc808f0f8`;
   `git -C tools/xboxrecomp rev-parse HEAD` =
   `110447d072d319f9bb8fdc4e2f582a111753e284`.  The tree is dirty (920 main
   entries, 4 submodule entries at audit time): store a binary diff, the full
   porcelain list, and a hash manifest of untracked/generated inputs, including
   `xbox/src/recomp/gen/*` and `xbox/src/recomp_manual.c`.
2. **Retained binaries.**  Exact `curse_of_darkness_xbox_recomp.exe` and
   `d3d8gl.dll` bytes with SHA-256.  Relinking changes the hash even with
   identical source, so a hash alone is not reproducible; keep the files.
3. **Toolchain and build commands.**  clang-cl/MSVC and CMake versions, plus
   `cmake -S xbox -B xbox/build`,
   `cmake --build xbox/build --config Release --target
   curse_of_darkness_xbox_recomp`, and `xbox\tools\build_d3d8gl.cmd` (which
   copies the DLL to the root).
4. **Runtime inputs.**  `extracted/default.xbe` hash
   (`4BC21B87CE7BB39EC02BEEBF78FD2F5B59C234187FB9A62AC0A5CE4F8B0F9B7A` at
   audit time), the extracted asset set, the frozen calibration
   (`E0FAE694...`), the frame pad (`30CE2B61...`), the scene flip table above,
   and the exact environment allowlist.
5. **Host state.**  CPU model and physical/logical counts, GPU and driver, OS
   build, power plan, display resolution/refresh, visible window, and no
   concurrent workload.  Run arms in alternating order on one host session.
6. **Per-run identity.**  Exe hash, staged DLL hash, and the actual loaded
   module path and hash from a `RECOMP_D3D8GL_TRACE=1` identity run.

Current state: requirements 1-4 are partially recoverable from untracked
artifacts, but **2 is not met**: the frozen exe and DLL are gone.  Do not call
the current `68624F23.../63996B28...` pair the frozen baseline.

## 5. Candidate gate

### 5.1 Window and reference

- Scene: `courtyard_light`, presented flips `[2980, 3160)` (180 frames,
  about 3.6 s of gameplay, after the scene transition and before pause).
- Same frozen calibration and frame pad; no new calibration.
- Reference from the frozen `framestats.csv` over this exact window:
  p50 18.128 / 19.258 / 18.027 ms (runs 1-3), median 18.128 ms, span 1.231 ms.
  Interval-derived rates are 55.2 / 51.9 / 55.5 Hz for this window only; they
  are not sustained-FPS measurements.
- Captures: three consecutive presented flips 3069, 3070, 3071, in separate
  capture runs (three baseline runs for the same-binary null, one candidate
  run).

If the frozen pair cannot be restored, rebuild it from the retained source
snapshot and use that rebuilt pair as the baseline arm, recording its new
hashes.  The historical 18.1-20.1 ms figures then serve only as a sanity range;
they are not a comparator.

### 5.2 Environment

Set only:

```
RECOMP_D3D8GL=1
RECOMP_FB_WINDOW=1
RECOMP_PAD_FRAMES=1
RECOMP_PAD_SCRIPT=@<run>\pad_script.txt
RECOMP_PACE_STATS=1
RECOMP_BENCH_STATS=1
RECOMP_FRAME_STATS=1
RECOMP_FRAME_STATS_FILE=<run>\framestats.csv
RECOMP_FRAME_STATS_START=0
RECOMP_FRAME_STATS_END=3160
```

Capture runs additionally set `RECOMP_CAPTURE_FLIPS=3069,3070,3071` and
`RECOMP_CAPTURE_DIR=<run>`.  Everything else with the `RECOMP_` prefix is
absent, in particular `BENCH_UNIQUE`, `GPU_STATS`, `HLE_STATS`,
`READBACK_STATS`, `D3D8GL_TRACE`, `TEXTURE_TRACE`, `FRAME_TIMELINE`,
`GPU_QUERY`, `IO_TRACE`, `GAME_STACK_TRACE`, `NV2A_TRACE`, `FRAME_LOG`,
`FRAME_HIST`, `FB_DUMP*`, `RENDER_CAPTURE`, `GUEST_PROFILE`, `PAIR_TIME_42E40`,
`COUNT_42E40`.

`RECOMP_PB_EXEC=1`/`PB_WALK=dma` and `FB_WINDOW/USB/KEYBOARD` are main.c defaults
when absent (main.c sets them at startup); leaving them absent is the frozen
behavior, and the interactive launcher sets the same values explicitly.

### 5.3 Protocol and exact commands

Stage each binary pair in an isolated directory so Windows loads the intended
DLL from the executable directory; do not overwrite `xbox/build` or the root
DLL.  Run arms serially with a host lock.

```powershell
# 0. Read-only: frozen inputs, current identity, blockers
python xbox/bench/gate.py snapshot

# 1. Stage (examples; use the retained frozen pair and the candidate pair)
python xbox/bench/gate.py stage --name baseline  --exe <FROZEN_EXE> --dll <FROZEN_DLL>
python xbox/bench/gate.py stage --name candidate --exe <CANDIDATE_EXE> --dll <CANDIDATE_DLL>

# 2. Identity runs (trace on, not timing)
python xbox/bench/gate.py identity --arm baseline  --confirm-host-run
python xbox/bench/gate.py identity --arm candidate --confirm-host-run

# 3. Three timing pairs, order AB BA AB, serialized
python xbox/bench/gate.py run --arm baseline  --run 1 --confirm-host-run
python xbox/bench/gate.py run --arm candidate --run 1 --confirm-host-run
python xbox/bench/gate.py run --arm candidate --run 2 --confirm-host-run
python xbox/bench/gate.py run --arm baseline  --run 2 --confirm-host-run
python xbox/bench/gate.py run --arm baseline  --run 3 --confirm-host-run
python xbox/bench/gate.py run --arm candidate --run 3 --confirm-host-run

# 4. Separate capture runs: three baseline (same-binary null) and one
#    candidate, all at the same three consecutive flips
python xbox/bench/gate.py run --arm baseline  --run capture1 --captures --confirm-host-run
python xbox/bench/gate.py run --arm baseline  --run capture2 --captures --confirm-host-run
python xbox/bench/gate.py run --arm baseline  --run capture3 --captures --confirm-host-run
python xbox/bench/gate.py run --arm candidate --run capture1 --captures --confirm-host-run

# 5. Validate and compare
python xbox/bench/gate.py validate --dir xbox/bench/out/gate/arms/baseline/runs/run1
python xbox/bench/gate.py validate --dir xbox/bench/out/gate/arms/candidate/runs/run1
python xbox/bench/gate.py validate --mode capture --dir xbox/bench/out/gate/arms/baseline/runs/capture1
python xbox/bench/gate.py validate --mode capture --dir xbox/bench/out/gate/arms/candidate/runs/capture1
python xbox/bench/gate.py compare \
  --baseline  xbox/bench/out/gate/arms/baseline/runs \
  --candidate xbox/bench/out/gate/arms/candidate/runs
python xbox/bench/gate.py compare-captures \
  --baseline xbox/bench/out/gate/arms/baseline/runs/capture1 \
             xbox/bench/out/gate/arms/baseline/runs/capture2 \
             xbox/bench/out/gate/arms/baseline/runs/capture3 \
  --candidate xbox/bench/out/gate/arms/candidate/runs/capture1
```

Each run stops when the in-memory recorder writes at flip 3160.  Expected cost
is about 60 s per timing/capture run plus startup: 12 host runs total (2
identity, 6 timing, 4 capture), with the identity runs stopping as soon as the
module line appears.

### 5.4 Pass conditions for one run

The validator requires all of:

- staged exe hash and staged DLL hash match the arm, and the identity run's
  `[D3D8HLE] module=` path equals the staged DLL with the same hash;
- `env.json` contains exactly the allowlist above (no extra `RECOMP_*`);
- `pad_script.txt` hash equals `30CE2B61...`;
- `run_status.json` complete, `max_flip >= 3160`;
- `framestats.csv` contains every flip in `[2980, 3160)` (180 rows, no gaps);
- no forbidden diagnostic output, no periodic `frame.bmp*`, no capture BMPs in
  a timing run;
- in every PACE/STALL bucket around the window, `dsp10 == frames` and
  `|actual - requested| <= 0.5 ms`; guest clock 950-1050 ms/s.

### 5.5 Decision rule

Compare p50 frame intervals only:

- `effect = median(baseline p50) - median(candidate p50)` (positive means the
  candidate is faster);
- require non-overlapping arms (`max(candidate) < min(baseline)` or the
  reverse) and `|effect| >= 0.5 ms`;
- invalidate the gate if either arm spans more than 1.5 ms (the frozen arm
  spans 1.23 ms) or binary identity changes within an arm;
- otherwise report "inconclusive", not a win.

The 0.5 ms floor is the pre-registered minimum claimable effect.  The rejected
`sub_00042E40` experiment showed a 0.07-0.19 ms apparent saving against 0.19 ms
baseline variation; that is exactly the size this rule refuses to claim.

### 5.6 Equivalent captures

Run three baseline capture passes and one candidate capture pass at flips
3069/3070/3071 (separate from the timing runs).  Compare
candidate-vs-baseline only to the same flips from the same frozen input.  The
null distribution is baseline-vs-baseline at those flips.  A candidate diff
larger than the null maximum plus 0.5 percentage points of changed pixels is a
divergence to review, not an automatic failure.  `gate.py compare-captures`
implements exactly this.  A single BMP is not a correctness proof, and blinking
is still open.

## 6. Correctness blockers (in order)

1. **Frozen binaries absent.**  `E9C7D678...` and `CF6DAF2C...` are not on
   disk; the current build is `68624F23.../63996B28...`.  The 18.1-20.1 ms
   baseline cannot be re-measured until the frozen pair is restored from a
   retained copy or rebuilt from a byte-identical source snapshot.  If a
   rebuild is used, record its new hash and call it "rebuilt baseline"; never
   merge it with the historical table.
2. **No per-run identity in the historical runs.**  Exe/DLL hashes and the
   loaded module path were not logged.  The root-vs-`output` DLL fallback means
   a stale root DLL can silently substitute.  The gate's identity run fixes
   this for new runs only.
3. **Capture denominators and per-bind deltas are invalid as reported.**
   Withdraw `5.565 MB/frame`, `321.3 KB/frame`, `0.600 ms/frame`,
   `97.2 KB/frame`, and the `~15.45 MB/frame` reconciliation until they are
   re-measured with one counter, one window, and a draw-stage consumer.  Do not
   propagate them into budgets or savings.
4. **No full replay.**  The capture is record-only; textures, shader/pixel
   shader contents, GPU-target starting state/order, and palette dedup are not
   replayable.  No worker/overlap cost or benefit is validated.
5. **Stale capture baselines.**  The qualified diffs compare against 2026-10-20
   baselines from another build/backend; they are not candidate equivalence.
6. **Diagnostic and host-control gaps.**  The historical runs enabled UNIQUE
   hashing, GPU stats, and seven in-run BMP writes; they were sequential, not
   alternating; host drift is visible.  The gate's allowlist and AB BA AB
   ordering address this, but the historical baseline did not have them.
7. **Metric naming.**  PACE bucket medians and `min(bucket)` are not sustained
   FPS and not a 1% low.  Only frame-interval percentiles are defensible.
8. **No `--exe` in the existing frame-exact driver.**  `frame_bench.py` cannot
   select a candidate binary; staging (as above) is required, or the driver
   needs a future explicit `--exe/--dll` option (not changed here).

## 7. Added files and verification

- `xbox/bench/gate.py` - snapshot, staging, identity run, bounded run,
  validation, paired comparison, capture-denominator audit, and matched-flip
  capture comparison.  Read-only subcommands do not launch a game;
  `run`/`identity` require `--confirm-host-run`.
- `xbox/tests/test_benchmark_gate.py` - 20 tests covering frozen-input
  derivation, window contiguity, environment allowlist, the mixed-denominator
  capture artifact, staging identity, validation, the decision rule, and
  matched-flip capture equivalence.
  Result: **20/20 pass**; the existing `test_bench_harness.py` still passes
  **14/14**.  No source or shared build file was modified.
- `xbox/bench/gate.py audit-capture --dir output/rcap_audit_final2` reproduces
  the key finding directly: 132 renderer frames vs 153 probe presented frames,
  SetTexture-scoped renderer deltas, and no replay artifact.
- `xbox/bench/gate.py validate --dir
  xbox/bench/out/frame_exact/20261021_194702_d3d8gl/run1 --legacy` reproduces
  the gate-window p50 18.128 ms over 180 contiguous frames and flags the
  historical run's capture BMPs, full-run required flip, and missing identity
  manifests.
