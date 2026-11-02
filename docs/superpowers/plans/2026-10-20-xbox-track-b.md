# Xbox Track B implementation plan

**Goal:** Reach correctly rendered 60 fps Xbox gameplay by replacing guest draw execution with D3D8 API HLE.

**Authority:** The user requested continuation of the Track B proposal and the remaining blockers. Continue in the existing `xbox-60fps` checkout, which contains the required local game translation and assets.

**Architecture:** Audit the actual translated XDK functions before redirecting them. Keep guest resource creation, references and state updates; suppress only operations whose complete rendering work is handled by the host. Initialize deterministically after successful guest device creation. Preserve software as the default until hardware captures and measurements pass.

**Constraints:** Do not commit generated game code, assets, DLLs, or benchmark outputs. Do not skip visual passes or change game timing to claim 60 fps. Use the same presented-frame input/capture schedule for comparisons. Existing 19 regressions must remain green.

## Tasks

- [ ] Verify API addresses, arguments, cleanup, state layouts and guest mapping; record evidence in `docs/xbox_d3d8_abi.md`.
- [ ] Add failing bridge tests for lifecycle, render-target vs Swap, stream/index/UP arguments, constants and preservation of guest shader handles. Repair `xbox/src/d3d8_hle.c` and replay hooks in `xbox/tools/prepare_generated.py`.
- [ ] Repair the GL boundary: guest offset, embedded resource data, TSS translation, initialization errors, UP draws, shader/FVF compatibility and once-per-frame capture. Add a repeatable tracked build script.
- [ ] Extend `bench.py` and `frame_bench.py` with explicit HLE selection and reusable software calibration; test the command/environment contract.
- [ ] Build, run all regressions, capture identical software/HLE flips and inspect images. Measure courtyard, pause, movement and movie independently. Diagnose every visual failure before accepting an FPS result.
- [ ] Review the combined patch, fix findings, record measured results and unresolved scene coverage. Enable no default switch without a correct and faster result.

## Review focus

Guest memory uses offset `0x10000` on this host. A guest shader handle is not an OpenCE object. Render-target changes are not presents. SetTile must remain a guest operation. Constants and guest state arrays must remain visible to later guest library calls. Runtime GPU/backend failures must not silently discard draws.

## Initial evidence

`ctest --test-dir xbox/build-tests -C Release --output-on-failure`: 19/19 pass. Existing software baseline capture launched with the saved frame script from `20261019_224739`; results belong under `xbox/bench/out/track_b_baseline`.
