# Xbox host

From the repository root, build with Visual Studio 2022 Build Tools and CMake:

```powershell
cmake -S xbox -B xbox/build
cmake --build xbox/build --config Release --target curse_of_darkness_xbox_recomp
```

The build requires the locally generated C sources in `xbox/src/recomp/gen/`
and the Xbox game files in `extracted/`, including `default.xbe`. Generated
game code and extracted assets remain excluded from version control.
`tools/prepare_generated.py` reapplies host integration repairs before compiling.

Run from the repository root so asset paths resolve:

```powershell
& .\xbox\build\Release\curse_of_darkness_xbox_recomp.exe
```

Closing the framebuffer window exits the host. Enter is Start, Z is A,
X is B, arrows are the D-pad, and WASD controls the left stick.

Movie playback uses the native SSE2 ARGB converter at `sub_004005EC` for
the player's `LIN_A8R8G8B8` surface. The converter uses the runtime guest
mapping offset and the original callee stack cleanup. Display field status
reports progressive scan with the live host vblank counter, so XMV frame
scheduling advances even when the guest D3D vblank counter is not updated.
`prepare_generated.py` reapplies both hooks after local regeneration.

## Performance (2026-10-19)
The software rasteriser runs the title screen and main menu at 58-61 fps and
the 2D options/pause menu at ~56 fps; the measured 3D courtyard is 21-36 fps
depending on the section. The D3D11 queue backend (`RECOMP_GPU_RASTER=1`) now
renders the whole title/menu correctly at 57-59 fps, but its one-draw-per-
triangle submission is slower than the software path in 3D, so the software
rasteriser remains the default. 60 fps 3D gameplay still needs batched GPU
submission or a GPU/VSH path; stencil shadows are also still missing.

## Benchmark

The deterministic per-scene harness is `xbox/tools/bench.py`:

```powershell
python xbox/tools/bench.py --runs 3
```

It records the software path's title, menu, cutscene, courtyard light/heavy and
pause intervals, writes captures under the gitignored `xbox/bench/`, and
reports both presented flips and unique framebuffer hashes. The current 3-run
software medians are title 54 fps, menu 34 fps (static UI), cutscene 50 fps,
and courtyard 26-27 fps. See `docs/xbox_60fps_analysis.md` for the profile and
the open pause/movie/combat capture caveats.
