# Xbox paths

The primary Noble Archimedes application uses the imported D3D12/XAudio2 host
from jmafort's CastlevaniaCOD-PC, with original launcher art, icon and music.
Run `Build Noble Archimedes.cmd` at the repository root, then
`Launch Noble Archimedes.cmd`. Its source, feature audit, separate saves/settings,
build options and diagnostics are documented in
[`docs/CASTLEVANIA_COD_PC_IMPORT.md`](../docs/CASTLEVANIA_COD_PC_IMPORT.md).
The primary executable uses its own SDK. The software/D3D8GL host described
below is retained as experimental renderer research; its graphics and input
work are independent of the primary executable.

## Legacy Xbox host

Prepare its separate MIT runtime with `python tools/fetch_legacy_xbox_runtime.py`.
The primary D3D12 release does not depend on this checkout.

From the repository root, build with Visual Studio 2022 Build Tools and CMake:

```powershell
cmake -S xbox -B xbox/build
cmake --build xbox/build --config Release --target curse_of_darkness_xbox_recomp
cmake --build xbox/build --config Release --target NobleControls
```

The build requires the locally generated C sources in `xbox/src/recomp/gen/`
and the Xbox game files in `extracted/`, including `default.xbe`. Generated
game code and extracted assets remain excluded from version control.
`tools/prepare_generated.py` reapplies host integration repairs before compiling.

Run from the repository root so asset paths resolve:

```powershell
& .\xbox\build\Release\curse_of_darkness_xbox_recomp.exe
```

## Controls

Run `Configure Controls.cmd` at the repository root, or open Controls with F2
while playing. The panel is also available directly as
`xbox/build/Release/NobleControls.exe`; it does not require the game files.
Changes to bindings and camera settings are staged until Apply; device/port
assignment and imported controller mappings take effect immediately. Select an
action, device, and slot, then choose Listen and press the desired input.
Conflicting bindings require the explicit Share action. F2 remains reserved as
a recovery shortcut. A gamepad can navigate the panel; East held while listening
cancels that binding operation.

The default PC preset retains Enter for Start, Backspace for Back, Z/X/C/V for
the original A/B/X/Y buttons, arrows for the D-pad, and WASD movement. It adds
Space for A, E for B, left mouse for X, right mouse for Y, Escape for Start,
and M for Back. I/J/K/L remain alternate stick inputs. These are original Xbox
button names; title-specific action labels are not yet verified. Bindings apply
to gameplay and menus together.

Mapped SDL3 controllers, including XInput and other supported USB gamepads,
can be connected or disconnected while the game is running. The panel can
capture a mapping for an unknown joystick and assign devices to guest ports.
Identical controllers without a reliable serial number may need reassignment.
The available rumble support depends on the controller. Physical XInput and
non-XInput hardware still need qualification; virtual devices are covered by
automated tests.

Mouse camera control is experimental: sensitivity and X/Y inversion are
configurable, and the original camera's angular wrap/clamp remains in place.
Gameplay camera direction, collision, lock-on and scripted transitions still
need interactive validation. F2 or focus loss releases mouse capture. The
in-game button prompts still use the original game's artwork; automatic
binding-aware prompts are not implemented. Prompt family and current binding
labels in the Controls panel do not change those original prompts.

Settings are saved to `%APPDATA%\NobleArchimedes\controls.ini`; device
assignments and mapping strings are in `controllers.ini` and
`gamecontrollerdb.txt` in the same folder. Missing/invalid settings fall back
to safe defaults. `Launch Xbox Modern.cmd` starts the current build with modern
input; `RECOMP_MODERN_INPUT=0` selects the legacy input path. Scripted benchmark
input retains its independent deterministic path.

For D3D8GL, rebuild its local DLL after input changes with
`cmd /c xbox\tools\build_d3d8gl.cmd` and set `RECOMP_D3D8GL=1` before launch.
The SDL3 SDK defaults to `output/sdl3/SDL3-3.4.18`; set `NOBLE_SDL3_ROOT` at
CMake configuration if installed elsewhere. The tracked input-provider patch is
applied to `tools/xboxrecomp` during configuration; an incompatible local edit
is reported and left untouched.

Closing the framebuffer window exits the host.

## Experimental widescreen

`Launch Xbox Widescreen.cmd` starts the HLE renderer with the wider target,
guest projection adjustment, and modern controls enabled. Edit
`Widescreen Settings.cmd` to select aspect ratio (for example `1.7777778` for
16:9 or `2.3333333` for 21:9), FOV multiplier, and initial window height. A
FOV multiplier of `1.0` keeps the original vertical FOV; values greater than
one show more. The window can also be resized after launch.

The captured title and prerecorded prologue retain their 4:3 composition;
3D scenes use the wider projection. The GPU-only renderer remains an opt-in
path, and the default software launcher remains unchanged. Coverage so far
includes title, movie, pause, and limited courtyard captures. Wider camera
angles, later scenes, in-engine cinematics, HUD placement and interactive
play still require validation; HUD elements are not independently movable
yet. Existing renderer artifacts and performance limitations remain open.

Movie playback uses the native SSE2 ARGB converter at `sub_004005EC` for
the player's `LIN_A8R8G8B8` surface. The converter uses the runtime guest
mapping offset and the original callee stack cleanup. Display field status
reports progressive scan with the live host vblank counter, so XMV frame
scheduling advances even when the guest D3D vblank counter is not updated.
`prepare_generated.py` reapplies both hooks after local regeneration.

## Performance (2026-10-19, post pre-work)
The software rasteriser now measures title 59 fps, menu 40 fps (static UI;
about 60% unique images), cutscene 57 fps, courtyard 31.5 fps, pause 53 fps
and post-movement courtyard 32 fps in the benchmark harness. The P8 UI path
now samples the palette per texel for quads; p8 fell from 6-14 ms/frame to
0.8-1.0 ms/frame. The default remains software; D3D11 is still opt-in and not
yet a 60 fps path. Frame-exact input/capture and the pause/movie presentation
path are the next prerequisites.

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
