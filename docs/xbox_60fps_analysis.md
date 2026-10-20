# Xbox 60 FPS analysis - Phase 0/1

Date: 2026-10-19  
Host: AMD Ryzen 7 2700X (8 cores / 16 logical), NVIDIA GeForce RTX 3080  
Build: Release, AVX2, software rasteriser default

## Reproducible benchmark

The harness is `xbox/tools/bench.py`.

```powershell
python xbox/tools/bench.py --runs 3
python xbox/tools/bench.py --profile movie --runs 1
python xbox/tools/bench.py --backend d3d11 --runs 1
```

It starts one hidden title instance per run, feeds the deterministic
`RECOMP_PAD_SCRIPT` timeline in `xbox/bench/out/.../pad_script.txt`, enables
`RECOMP_PACE_STATS`, `RECOMP_GPU_STATS`, `RECOMP_BENCH_STATS` and
`RECOMP_BENCH_UNIQUE`, and parses `[PACE]`, `[GPU-PERF]`, `[GPU-FLUSH]`,
`[GPU-PLAN]`, `[BENCH]`, `[TICK]` and `[UNIQUE]`.

The title reaches gameplay with:

```text
16000:start
30000:start
40000:start
```

`[TICK] guest_ms` is the guest-visible `KeTickCount` read through the kernel
bridge; it is not the game's logic frame counter. `[UNIQUE]` hashes a 1/4-pixel
sample of each published framebuffer. It distinguishes re-presented duplicate
images from new images; `unique/present` is therefore reported separately from
the host flip rate.

Reference screenshots are written to `xbox/bench/out/.../capture_*.bmp`.
`xbox/bench/baselines` holds the current software-path snapshots; both
directories are gitignored. A baseline comparison is a per-pixel difference
with an 8-count channel threshold.

### 3-run result (software, default)

| scene | median fps | 1% low | unique fps | unique/present | draw | p8 | xform | raster | share | wait | setup | guest ms/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| title | 54.0 | 53.0 | 52.4 | 98.1% | 10.83 | 9.45 | 0.03 | 1.34 | 7.69 | 0.92 | 0.00 | 662 |
| main_menu | 34.0 | 34.0 | 16.4 | 51.0% | 0.08 | 0.07 | 0.00 | 0.01 | 5.18 | 0.68 | 0.00 | 677 |
| cutscene | 50.0 | 49.0 | 37.9 | 74.7% | 0.08 | 0.06 | 0.00 | 0.01 | 4.88 | 0.59 | 0.00 | 928 |
| courtyard_light | 26.0 | 24.5 | 23.7 | 100.0% | 19.66 | 6.33 | 9.41 | 13.31 | 14.03 | 1.38 | 0.00 | 849 |
| pause | 43.5 | 42.5 | 43.8 | 100.0% | 16.38 | 14.29 | 0.51 | 2.12 | 9.78 | 1.50 | 0.00 | 877 |
| courtyard_heavy | 26.0 | 24.0 | 25.8 | 100.0% | 24.70 | 11.38 | 9.52 | 13.44 | 13.13 | 0.96 | 0.00 | 927 |
| combat | 27.0 | 25.0 | 27.0 | 100.0% | 26.40 | 13.69 | 8.99 | 12.81 | 13.66 | 1.10 | 0.00 | 899 |

The run-to-run spread is within 1-2 fps for the stable 3D scenes. The title,
cutscene and menu are UI/transition scenes and are not 3D targets.

### Scene caveats

- `main_menu` is the *static* title menu. It presents 34 flips/s and only 51%
  of those are unique images. The guest menu loop appears to be capped or
  blocked independently of host pacing; a static screen cannot be used as the
  60-unique-frame proof.
- `pause` opens the game's pause state (PACE rises to 43-46 fps and the HUD
  disappears), but the framebuffer window retains the 3D background. The pause
  overlay is not captured by `RECOMP_FB_WINDOW`; the baseline is not a pause
  screenshot. The old `output/m-cpu-skip/frame_00005.png` shows the intended
  pause/status panel. This is an open presentation-ordering bug.
- `courtyard_heavy` and `combat` are the same first courtyard after forward
  movement; no enemies or Innocent Devil were reached. The requested later
  combat scene still needs either a save or a longer deterministic route.
- The boot/attract movie was not captured as video by the framebuffer window
  (`boot` is black); the movie output path is a separate window/presenter. It
  must be measured separately before making movie claims.

## Presentation cadence

The title's static `D3DPRESENT_PARAMETERS` block at guest `0x012B6600` is
initialized by `sub_003873BB` in `recomp_0069.c`. The fields include
`BackBufferWidth/Height=640/480`, format 6 and `+0x30 = 0x80000001`. The
30 Hz interval values are not present in this block. The host pacer is 60 Hz
by default; `RECOMP_PACE_OFF=1` is a diagnostic only.

The courtyard has `movie=0` and `flag=00000000` throughout, and the guest
`KeTickCount` advances at 0.85-0.93 s per host second. This is **not evidence
of a native 30 Hz cap**; it is evidence that guest time is stretched by the
renderer. The changelog's 30 Hz areas still need a separate scene check.

## Profiling (sampling, Release + PDB)

Method: `output/xbox_profile_run.py` with the three-Start timeline, 10 s of
2 ms RIP samples after the courtyard settles; symbolized with
`output/symbolize_xbox.py` against `xbox/build/Release/*.pdb`.

- Total process CPU during the 10 s sample: **7.36 cores**.
- One raster worker: **0.98 core** (busiest); twelve pool workers around
  **0.5 core** each; main/guest thread **0.56 core**; window thread ~0.03.
- The sampled raster worker's hottest symbols:

| symbol | share |
|---|---:|
| `xf_rows` (software pixel loop) | 25.3% |
| `ntdll` wait/lock paths | 33.1% |
| `nv2a_rc_eval_plan` | 5.5% |
| `nv2a_vsh_run` | 4.6% |
| `nv2a_pb_exec_method` | 3.9% |
| `raster_xf_triangle` | 3.7% |
| `prepare_p8_draw_textures` | 3.5% |
| `fast_present_rows` | 2.9% |

The main thread is not over the 16.6 ms budget; the raster pool is. The
`ntdll` wait share also shows lock/queue contention in the current deferred
queue, not pure ALU work.

## Ceiling and recommendation

From the changelog and this run:

- `RECOMP_SKIP_STENCIL=1` reaches ~47-53 fps but drops the main stencil-enabled
  passes, so it is a measurement only.
- `RECOMP_PACE_OFF=1` leaves 3D at 19-33 fps; pacing is not the limit.
- CPU stencil is 12-18 fps; the software rasteriser cannot absorb the shadow
  pass.
- The software path is limited by pixel work + VSH + P8, with the raster pool
  using ~6 cores. A GPU path that keeps surfaces resident and uses GPU stencil
  is the highest-leverage direction.

Recommended track: **Track A**, in this order:

1. Display-buffer-aware D3D11/D3D12 presentation with surface ownership and
   readback only on guest reads, so the per-queue readback and double-buffer
   ordering failures disappear.
2. GPU D24S8 stencil with two-sided ops and the traced plan-7/HUD states.
3. Persistent batched submission; the current D3D11 experiment still submits
   through the CPU executor and has no readback win.
4. GPU VSH translation; the profile still shows `nv2a_vsh_run` and the
   executor on the CPU.

Track B (D3D8 API HLE) should be chosen if step 1 still leaves the executor too
expensive. The existing D3D8GL scaffold still falls back on
`SetStreamSource` and draw `0x003BA740`, so it cannot yet remove the NV2A path.

This is an architecture change (surface ownership / present path) and requires
the user's approval before implementation.

## Test status

`ctest --test-dir xbox/build-tests -C Release`: **19/19 pass** in the current
tree (18 prior tests plus `bench-harness`). The harness test covers parsing and
scene-window invariants.
