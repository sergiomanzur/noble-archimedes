# Xbox gameplay flicker and performance follow-up (2026-10-21)

The user reported black regions blinking after movement, with frame drops near
40 FPS. The exact executable/backend of that interactive run is unknown.

## Confirmed findings

- HEAD on entry was `46001e0`. The root Xbox executable predates the benchmark
  executable by two days. `Launch Native.cmd` starts the separate PS2 host.
- `frame_pacing.c` prints the pacing target as `fps=60`; the preceding
  `flips/s` field measures presentation calls. A line containing `fps=60`
  alone does not establish measured 60 FPS.
- The real-GL integration test failed on its first pixel after async readback
  was introduced: black was returned instead of a green triangle. The 21
  existing host tests did not cover that GPU readback contract.
- The two-slot readback ring returns pixels from two swaps earlier in steady
  state, rather than the one-frame delay stated in the previous checkpoint.
- Startup reported successful readback without writing pixels. Size/pitch
  changes interpreted pending allocations using the new dimensions.

## Repair

Explicit `noble_d3d8_readback` reads current GPU pixels synchronously. The new
`noble_d3d8_readback_swap` preserves async overlap for presentation. Startup,
size/pitch transitions, and failed maps recover through a synchronous copy.
Saved read-framebuffer and pixel-pack state is restored on the common exit
path, and the renderer state cache is invalidated. Full GL state preservation
has not been established: framebuffer creation and multisample resolve can
also change draw bindings. GPU regression cases cover
current explicit reads, async frame order, and grow/shrink transitions.

This repair does **not** establish the cause of localized flicker during
movement. Consecutive gameplay captures are required to assess that symptom.

## Performance experiment

Vertex/index mirror caching remains disabled because guest stores are not
observable through the original write-watch hooks. Re-enabling it could render
stale geometry. Existing GL state caching already covers many common setters.

`buffer_upload` splits small uploads into 4 KiB pieces for Mesa, while the saved
host profile uses NVIDIA. Test this hypothesis without changing the default:

- `RECOMP_D3D8GL_UPLOAD_WHOLE=1` issues one GL call per logical upload.
- `RECOMP_D3D8GL_UPLOAD_PERF=1` reports logical uploads, bytes, actual GL calls,
  and CPU time per frame in 120-frame windows.

Use the same HLE-calibrated frame input for both modes. Preserve the 10 ms DSP
settle, render passes, readback, and unique-frame checks. Dense captures around
movement impose extra I/O overhead, so these diagnostic runs are not clean
final performance qualification.

## Verification and remaining work

The rebuilt host passes all 21 CTest entries. The expanded real-GPU integration
test passes in both upload modes. An extracted-C upload check verifies boundary
sizes, offsets, copied bytes, and same-address mutations between uploads.

Local diagnostic outputs are under `output/orchestrator_validation/`; the
consecutive-capture driver is `output/orchestrator_probe.py`. These remain local
with the generated game assets and binaries.

Two 112-second HLE replays reused the same HLE calibration and controller
script, including 120 consecutive movement captures and seven scene midpoints
per run. Diagnostic gameplay medians (flips/s):

| Scene | Repaired default | Whole-upload experiment |
| --- | ---: | ---: |
| Courtyard light | 50 | 49 |
| Pause | 55 | 53 |
| Courtyard heavy | 47 | 47 |
| Combat window | 50 | 50 |

Unique/present ratios were 99.1–100% in these gameplay windows. There is no
demonstrated performance gain, so whole-upload remains opt-in. These are one
run per mode with capture I/O, not a statistically qualified comparison.
The default replay used the repaired DLL before upload instrumentation was
built; the experiment replay used that same repair plus the upload switch.
DLL-side upload diagnostics were not captured into the host's redirected log,
so these runs do not establish upload CPU cost or actual driver call counts.

The contact sheet sampled every fifth captured movement frame; it showed no
obvious large black flash. Across all 120 consecutive frames, the largest
newly-near-black region relative to the preceding frame was 0.822% of pixels.
This heuristic cannot distinguish legitimate motion/shadows from a small
rendering defect, and does not close the user's localized flicker report.

The next performance profile must include immediate-mode `End`: the existing
HLE submit timer includes clears/setters but returns before accumulating End's
backend draw time. Its label is not a complete measure of GL draw submission.

Next acceptance gates, in order:

1. Reproduce the localized blinking in an identified binary/backend and retain
   adjacent good/bad frames, including the actual window/presentation path.
2. Compare that moving scene with a reference; correlate the bad frame with
   render-target, depth/stencil, shader, and texture state. Investigate GL
   state preservation with a test rather than assuming it causes the symptom.
3. Complete internal draw-stage timing, including immediate End, then choose
   an optimization based on the measured cost.
4. Qualify accepted changes with multiple runs without dense capture overhead.
   Do not enable HLE by default or claim stable 60 FPS from a pacing target or
   seven isolated screenshots.
