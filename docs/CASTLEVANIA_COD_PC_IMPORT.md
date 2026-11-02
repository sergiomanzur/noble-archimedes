# CastlevaniaCOD-PC source import

The imported Xbox implementation is now the primary **Noble Archimedes** build.
Its maintained branding overlay is `xbox/noble/overlay/`; original art, icon
and music are in `assets/noble/`. Build/launch with `Build Noble Archimedes.cmd`
and `Launch Noble Archimedes.cmd`. Current executable/DLL names are
`NobleArchimedes*`. The initial import audit and measurements below describe
the pre-branding import; current instructions are in [BUILDING.md](../BUILDING.md).

Imported on 2026-11-02 from [jmafort/CastlevaniaCOD-PC](https://github.com/jmafort/CastlevaniaCOD-PC), commit `1d9edf6b2bdbb683cb95dff5b317916aec31db84` (version 1.5.1). A separate download is at `../CastlevaniaCOD-PC`. All 60 tracked files are preserved under `xbox/third_party/castlevania-cod-pc`, including both release configs, the full SDK patch, launcher, runtime extensions, scenarios and tools. `IMPORT.json` records every file's SHA-256, repository and SDK pin. Hashes normalize CRLF to LF so Git's platform line-ending conversion does not invalidate the inventory; source content is otherwise unchanged.

The donor credits Pazzi7054's port, TGP482's Xbox XBE Recomp, Neguchads' game research and PCSX2 texture naming. Keep its README credits and GPL-2.0 LICENSE with this source. The adapted Python decoder and release metadata carry the same attribution and terms. The optional backend is a separate executable, with no linking into the GPL-3.0 PS2 host.

## How the import fits

Our repository contains both PS2 and Xbox recompilation paths. The donor is Xbox only: its x86 addresses, D3D8 state and XBE configs cannot be used as PS2 MIPS addresses or VU replacements. Even our Xbox path uses a different runtime: global/TLS registers and generated C versus the donor's `X86Context`, generated C++, game DLL and `gpu::Extension`. Applying the SDK patch to `tools/xboxrecomp` would be incorrect.

The complete donor is therefore available as an optional D3D12 backend, prepared and built by our own helpers. It can retain all the donor's coupled renderer and gameplay features without overwriting current input, renderer, performance or PS2 work. Existing launchers continue to select their current hosts. A separate read-only diagnostic adapter also makes the donor's state layout usable against our existing Xbox process.

## Build and launch

Requirements: Windows x64, Visual Studio C++ Build Tools/Windows SDK, LLVM `clang`/`clang++`/`lld-link`, CMake 3.25+, Ninja, Git and Python on PATH. Preparation downloads the pinned SDK and CMake dependencies; it never invokes a toolchain installer.

From the repository root:

```powershell
& '.\Build Xbox D3D12.cmd' --full-game
& '.\Launch Xbox D3D12.cmd'
```

`--full-game` also builds `Castlevania.dll` from your local XBE. Without it, the runtime/launcher/recompiler are built and the first launch compiles the DLL. Generated game code and binaries stay under ignored `output/`; first compilation can take considerable time. Use `--jobs 2` to reduce build memory pressure.

The default game source is `extracted/default.xbe` with `data`, `movie`, `sound`. The USA SHA-1 must be `423bc19b7ced263570dce5e7cd5d81954dfbe13c`. To select another extracted copy, add `--assets C:\path\to\usa`. To build EU too, add `--eu-assets C:\path\to\eu` (EU SHA-1 `b1910aaa4897cc9ca9627190289bf18d6a0943bd`), then launch with `Launch Xbox D3D12.cmd eu`. This uses `eu-play` with its own EU asset root, saves and settings; the default launch still selects USA. No EU image is supplied. The helper deliberately accepts exact hashes; the donor runtime itself has broader patched-certificate recognition.

Preparation copies our pristine snapshot to `output/jmafort-port`, copies the XBE and makes directory junctions to the three game-data folders. It does not copy gigabytes of assets, alter those files, or share saves. SDK `91146ff6e2a218b19c1109dc665d06020472ab57` is checked out under `output/jmafort-sdk` and the exact donor patch is checked and applied there once. Preparation is repeatable and rejects edited working source or a different SDK revision rather than overwriting them.

Runtime outputs are `output/jmafort-port/out/build/release`. The imported launcher manages its own `castlevania.toml`; saves are in its own `hdd/E/UDATA/4b4e002d`. Existing host saves/settings are not migrated automatically. Put optional PCSX2 HD replacements under `output/jmafort-port/textures`; no texture pack is downloaded or bundled. Gamepad/keyboard defaults and the F10 menu belong to this backend, independently of our modern Controls panel.

Source verification and preparation without building:

```powershell
python xbox/tools/jmafort_backend.py --verify
python xbox/tools/jmafort_backend.py
```

## Reuse audit

"Available in imported backend" means the implementation is preserved and included in that backend's build. It does not mean that feature has been validated here in every scene or ported to our existing renderer.

| Donor component | Useful work | Integration and remaining work |
| --- | --- | --- |
| `src/common/widescreen.cpp` | Hor+ projection, shader classification, 4:3 HUD/movie composition, backdrop edge expansion, frame-pass overscan repair | Available in imported backend. Our D3D8GL renderer has its own projection/classification; translating the extension requires equivalent vertex, render-target and constant-state inputs. EU frame-pass and CPU-written movie targets need distinct handling. |
| `sdk-patches/xboxxbe-recomp.patch` | 4X stencil shadow draw grid, no-texture fixed-function cascade, indexed UP draws, shader disk cache, anisotropy without mipmaps, render-extension hooks, DSound methods, region dispatch, console language and multichannel mixing | Applied only to pinned donor SDK. Shadow grid and no-texture cascade are concrete leads for our current renderer; they need synthetic rendering fixtures and scene comparisons before porting. This patch does not replace our scheduler or NV2A pushbuffer work. |
| `src/common/texpack.cpp` | PCSX2 XXH3 palette/GS block hashing, P8-to-P4 detection, eight Xbox/PS2 aliases, alpha conversion, asynchronous PNG/DDS replacements with mipmaps, PS2 icon sheet | Available in imported backend. Existing Xbox renderer needs its own cache invalidation, upload and replacement interfaces before reuse. Changed HARD DISK/NEW!/D-pad artwork must not be blindly aliased. |
| `src/game/hooks.cpp` | Wider guest culling matrices, optional Lament-save Moai bonus, Crazy/Trevor name-code unlocks | Available in imported backend, with region macros. Our existing culling hook is already present; do not apply its scale a second time. Bonus/unlock hooks remain donor-only and optional. |
| `src/common/release.h`, `src/game_eu/*`, both configs | USA/EU function and data addresses; shared hooks with correct per-region selection | Retained in backend; USA/EU data profiles extracted to `xbox/tools/imported_game_profiles.json`. Existing generated Xbox C remains USA only. Presence of EU metadata does not confer EU support on that host. |
| `src/common/telemetry.cpp` | Player activity, pose, HP/MP/EXP/gold, camera, scene/loading/menu/modal state, rooms and enemy JSON | Read-only player/camera/scene subset adapted into `xbox/tools/imported_game_state.py` for our host. Full telemetry remains available in donor backend. External samples are best-effort, not frame-atomic. |
| `src/game/enemies.cpp`, `warp.cpp`, `src/common/bot.cpp` | Enemy discovery, game-thread room transitions, recorded route movement/combat | Retained in imported backend. Existing host would need safe guest-call wrappers and frame-thread request dispatch before enabling writes or automated warps. No arbitrary thread warp is installed. |
| `src/common/keyboard.cpp` | Scripted/live controller commands, frame stepping, dump/warp/heal/bot protocol and telemetry | Available in imported backend. Its CV_PAD protocol differs from our RECOMP_PAD scripts; scripts must not be interchanged. Existing modern input bindings remain independent. |
| `src/common/menu.cpp`, `setup.cpp`, `game_info.cpp` | F10 UI, display/graphics settings, asset setup, PS2 layout/bonus toggles and EU language callbacks | Available in imported backend. These rely on donor `app::GameInfo`; copying into our host directly would bypass our input/renderer settings. |
| `src/launcher/*` | D3D12 GPU checks, settings, pack detection and seven UI languages | Built as imported `CastlevaniaLauncher.exe`, reached through `Launch Xbox D3D12.cmd`. Does not control our existing PS2 or Xbox executables. |
| `tools/cvtest/*`, routes and scenarios | Deterministic new-game/movement/menu assertions, save/load, warp/sweep, route bot, timings, release-install checks | All preserved, runnable against the imported build. They depend on the donor's CV_PAD and telemetry interfaces. Optional screenshot judging needs separate model/service configuration and is not invoked by our build. |
| `tools/release_map.py` | Cross-region function/data matching from local XBE bytes and generated mappings | Preserved for future EU hook work. Its heuristic matches require play validation; no game bytes or generated mapping is included. |
| `tools/texpack/upscale.py` | Generate missing replacements and calibrate models against a supplied pack | Preserved as an optional offline tool; requires separately installed PyTorch/spandrel/Pillow and a model. No model, pack or generated game art was imported. |
| `tools/extract_iso.ps1`, `xbe_icon.cpp`, screenshot/test scripts | Xbox disc extraction, game-owned icon extraction, captures | Preserved. Our default helper reuses extracted data and builds with `CV_GAME_ICON=OFF`; no game icon enters source. |
| CMake/presets, build/release scripts | Separate game DLL, first-run recompilation and package checks | Used through our wrapper with an exact SDK pin. Original `build.bat`/`release.bat` preserved for reference; they are not automatically executed, do not install dependencies or publish a release. |

## State diagnostics for our existing Xbox host

```powershell
python xbox/tools/imported_game_state.py --pid 12345 --samples 10 --interval 1
```

Use the PID of `curse_of_darkness_xbox_recomp.exe`. Guest addresses must belong to the USA Xbox image. If the runtime maps guest memory with a nonzero `g_xbox_mem_offset`, supply `--memory-offset 0x...` from that process's diagnostic/debugger value. The helper defaults to identity mapping; missing fields can mean a wrong offset or unmapped memory, not a game-state assertion. `--region eu` only selects an EU layout for an actual EU process; it does not convert our executable.

The reader opens the process with `PROCESS_VM_READ` only, bounds-checks address arithmetic, rejects short reads and nonfinite vectors, and omits unreadable fields. It does not pause the game or write any memory. JSON goes to stdout; save private samples under ignored `output/` if needed.

## Verification

```powershell
python -m unittest discover -s xbox/tests -p test_imported_game_state.py
python -m unittest discover -s xbox/tests -p test_jmafort_backend.py
python xbox/tools/jmafort_backend.py --verify
```

Local verification on 2026-11-02:

- Full `Build Xbox D3D12.cmd --full-game` completed with exit 0 using Clang 22.1.8 and the VS 2022 Build Tools environment. Runtime, launcher, recompiler and all 128 generated game partitions linked successfully. The local XBE SHA-1 matched the USA release. SDL deprecation and Dear ImGui warnings were nonfatal.
- Thirteen focused Python checks passed (seven state-reader checks, six backend/import checks), also registered as two passing CTest tests. This includes a real Windows process-memory read, corrupt/short/unmapped reads, nonfinite camera values, region metadata, inventory/newline conversion and repeatable EU staging. Preparation was repeated successfully against the patched SDK.
- The donor's unmodified `newgame.json` scenario passed **7/7 steps**: title, skip opening movies, enter gameplay, stand, walk, open pause, close pause. Courtyard standing measured 59.8 FPS and movement 59.5 FPS on the local RTX 3080; Hector moved about 649 game units along +z, and HP/max HP were 100. These are short scenario samples, not a whole-game benchmark or a controlled comparison against our other hosts.
- The timed `after-movies` screenshot was black during a transition, reported as a warning by the scenario. The subsequent gameplay, movement and menu steps passed. No screenshot model/judge service was used.
- A fresh review found and resolved the EU asset-discovery issue. EU staging is covered synthetically, but an EU game image was unavailable, so EU compilation/gameplay and language selection were not qualified here. HD packs, later rooms, combat, shadows across scenes and release-install checks remain unqualified.

The local gameplay report, JSON states and screenshots are under `output/jmafort-port/out/test/newgame-20261102-105330`; build output is in `output/jmafort-build.log`. These generated/captured game materials remain ignored. Reproduce the gameplay test with:

```powershell
cd output/jmafort-port
python -I tools/cvtest/cvtest.py tools/cvtest/scenarios/newgame.json --review=none --keep
```

Upstream README texture coverage and other performance numbers are upstream claims, not measurements of our build. Existing PS2 and Xbox renderer paths were not changed by this import.
