# Build and play

The **primary Noble Archimedes build is the Xbox D3D12 application**. The separate PS2 workflow appears below and remains experimental. Do not mix generated code, dependencies or game inputs between them.

## Primary Xbox prerequisites

Use Windows x64 with Python 3, Git, CMake 3.25+, Ninja, LLVM Clang/LLD, Visual Studio C++ Build Tools and a Windows SDK. Run from a developer environment that can find the C++ toolchain. The imported backend was built locally with Clang 22.1.8 and the VS 2022 Build Tools environment. Playing requires a Direct3D 12 capable graphics adapter and current driver. Dependencies are fetched during preparation/configuration; no proprietary Xbox SDK is required or provided.

Clone the repository and provide an extracted copy of your own **USA original Xbox** disc in `extracted/`: `default.xbe`, `data/`, `movie/`, `sound/`. PS2 discs cannot supply this input. The USA XBE accepted by the helper has SHA-1 `423bc19b7ced263570dce5e7cd5d81954dfbe13c`. The helper currently checks an unmodified executable; the upstream runtime's broader patched-header/ISO handling does not imply the build wrapper accepts every input layout.

```powershell
git clone https://github.com/sergiomanzur/noble-archimedes.git
cd noble-archimedes
& ".\Build Noble Archimedes.cmd" --full-game
& ".\Launch Noble Archimedes.cmd"
```

`Build Noble Archimedes.cmd` compiles the locally translated game DLL as well as the runtime/launcher; it can take time. To defer game-DLL compilation to first launch, use `Build Xbox D3D12.cmd` without `--full-game` (or the Python helper with `--build` alone). The wrapper prepares `output/jmafort-port/`, verifies the pristine source inventory under `xbox/third_party/castlevania-cod-pc/`, and uses SDK commit `91146ff6e2a218b19c1109dc665d06020472ab57` with the donor's MIT SDK patch and the [local frame copy patch](xbox/noble/sdk-patches/frame-copy.patch). Do not edit generated working-copy source as your maintained source; preserve any existing changes before preparing another version.

For a different extracted game directory:

```powershell
python xbox/tools/jmafort_backend.py --assets "D:/MyGames/CoD-USA" --build --full-game --jobs 4
```

The main output directory is `output/jmafort-port/out/build/release/`. It contains `NobleArchimedes.exe` and `NobleArchimedesLauncher.exe`; the launcher offers display, game, language and texture-pack settings. Keep compiler/runtime support and locally staged assets in their generated layout. Primary CMake targets and locally generated game DLLs use `NobleArchimedes*` names so first-launch compilation and imports agree. Some internal window classes, logs and settings retain upstream compatibility names. Loading PCSX2-format HD packs is supported by the imported backend; packs and game files must be obtained separately. Use the upstream [HD texture instructions](xbox/third_party/castlevania-cod-pc/README.md#hd-textures) for pack layout.

The root CMake default delegates to this Xbox build through the Python helper. It does not compile the experimental PS2 host unless explicitly configured with `-DNOBLE_EXPERIMENTAL_PS2=ON`.

For HD movie replacements, put a compatible pack's `movies.toml`, signatures and video files in `movies/` beside the game executable. On Windows the decoder uses Media Foundation. The game's disc still supplies the timing and audio. Use **HD movies** and **Movie framing** in the launcher or in-game settings. See the [upstream pack format](https://github.com/jmafort/CastlevaniaCOD-PC#hd-movies).

## European Xbox release

Retain USA assets for the main build and add a separate extracted European Xbox copy:

```powershell
python xbox/tools/jmafort_backend.py --assets "D:/MyGames/CoD-USA" --eu-assets "D:/MyGames/CoD-EU" --build --full-game
```

The EU XBE SHA-1 expected by the helper is `b1910aaa4897cc9ca9627190289bf18d6a0943bd`. The additional application is `NobleArchimedesEU.exe`. The helper creates an `eu-play/` layout beside the main build so EU discovery uses the EU game files. EU source/configuration support is imported from upstream; **no EU game image was available to qualify EU compilation, gameplay or language selection locally**. Treat it as unqualified until tested with your own copy.

## Xbox verification and distribution

```powershell
python xbox/tools/jmafort_backend.py --verify
python -m unittest discover -s xbox/tests -p test_jmafort_backend.py
python -m unittest discover -s xbox/tests -p test_imported_game_state.py
```

After preparing the Xbox SDK, run `xbox\tests\run_frame_copy_shader_test.cmd` to check the frame copy shaders without opening a game window.

These checks verify source/preparation helpers and diagnostics; they do not certify a complete playthrough. The branded USA build passed 7/7 new-game/movement/menu steps, and its no-DLL first-run test passed 8/8 startup/gameplay checks. To run the main test groups and reproduce first-run compilation:

```powershell
cmake -S . -B output/noble-root-configure -G Ninja
ctest --test-dir output/noble-root-configure --output-on-failure
python xbox/tools/check_noble_first_run.py
```

The first-run check starts a separate test folder and saves, supplies the local game files, and intentionally omits the generated game DLL. It limits PATH to Windows components, but installed runtime DLLs are still available: this is not clean-Windows package qualification. Local reports are under ignored `output/jmafort-port/out/test/`. Build success, a launcher's presence and a high frame counter alone are not whole-game validation.

Keep game files, locally generated C++/DLLs, logs containing game-derived data, screenshots/captures and saves out of source and release packages. A distributable host package must also include the applicable source/license/notice material and satisfy each dependency's terms; see [licensing](docs/LICENSING.md). Do not publish the whole local build directory.

---

# Experimental PS2 developer workflow

Choose a configuration in [build variants](docs/BUILD_VARIANTS.md). The common setup and generation steps below apply to each public configuration. To work on a fork and submit changes, see [CONTRIBUTING.md](CONTRIBUTING.md).

## Scope

Established target: Windows x64, MinGW-w64 GCC/G++ 15.2, Python 3.12, CMake 3.24+, Ninja and a Vulkan driver. Incremental builds were used during development. **Clean public generation/build/play has not been qualified end-to-end.** Private optimized VU DLLs and game-specific GPU translations are excluded; these instructions do not reproduce the private benchmark binary.

Only USA PS2 **SLUS-21168** is targeted. Supply your own lawfully obtained game. Keep every game input, generated body, capture and binary local and ignored.

## Fetch and restore the runtime

```powershell
git clone --recurse-submodules https://github.com/sergiomanzur/noble-archimedes.git
cd noble-archimedes
git -C tools/PS2Recomp apply --check ../../patches/ps2recomp-progress.patch
git -C tools/PS2Recomp apply ../../patches/ps2recomp-progress.patch
```

Apply once to the pinned clean submodule. Inspect a failed check; do not force it over another runtime version.

## Local game input

Place the disc image at the root as `Castlevania - Curse of Darkness (USA).iso`. Extract the boot executable locally:

```powershell
@'
from pathlib import Path
from tools.ps2_iso_reader import PS2IsoReader
iso = PS2IsoReader("Castlevania - Curse of Darkness (USA).iso")
try:
    target = Path("extracted/SLUS_211.68")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(iso.read_file("SLUS_211.68"))
finally:
    iso.close()
'@ | python -
```

Keep the ISO for streamed assets. An ELF alone is insufficient. The executable changes its working directory to its own directory, so the asset/dependency layout must be beside it.

## Configure dependencies and compiler

Run where `g++`, `cmake`, `ninja`, `python` and `git` are available. This is the expected setup rather than a newly certified clean-build recipe:

```powershell
python tools/PS2Recomp/ps2xRuntime/scripts/fetch_parallel_gs.py
cmake -S tools/PS2Recomp -B tools/PS2Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_ENABLE_PGS=ON
cmake --build tools/PS2Recomp/build --target ps2_recomp ps2_runtime ps2_iop --parallel 2
```

CMake fetches window/UI libraries and Windows FFmpeg; network and upstream toolchain requirements apply. The manual linker expects these paths relative to `tools/PS2Recomp/build`:

- `ps2xRuntime/libps2_runtime.a`, `librlImGui.a`, `libimgui.a`;
- `ps2xIOP/libps2_iop.a`;
- `_deps/raylib-build/raylib/libraylib.a`;
- `ThirdParty/ffmpeg-prefix/src/ffmpeg_external/bin/{avcodec,avformat,avutil,swresample,swscale}.lib`;
- optional GPU archive list `ps2xRuntime/pgs_link_inputs.txt`.

A portable dependency/install manifest remains missing. The root CMake default builds the primary Xbox application. The experimental PS2 CMake path is selected with `-DNOBLE_EXPERIMENTAL_PS2=ON`; the manual generated-chunk workflow below remains the established PS2 build entry point.

## Generate guest C++ locally

Locate the built compiler and invoke it from the root:

```powershell
$recompiler = Get-ChildItem tools/PS2Recomp/build -Filter ps2_recomp.exe -Recurse | Select-Object -First 1
if (-not $recompiler) { throw "Compiler not built" }
& $recompiler.FullName cvcod_ps2recomp_config.toml
if ($LASTEXITCODE -ne 0) { throw "Guest generation failed" }
python tools/generate_stub_registry.py
python tools/optimize_register_functions.py
python tools/generate_entry_chunk.py
```

The TOML supplies discovery/binding metadata; the ELF supplies instructions. Generated bodies, entry adapters, registration and chunks stay local. Stripped-game discovery and locally maintained guest compatibility changes still need reproducibility work. Completing generation does not prove complete gameplay.

## Build and launch

```powershell
python tools/build_recomp_chunks.py --jobs 2
python tools/link_noble_archimedes.py
$env:PS2_GS_BACKEND = "pgs"
Remove-Item Env:PS2_VU_BACKEND -ErrorAction SilentlyContinue
.\NobleArchimedes.exe
```

The scripts compile 32 chunks plus the entries chunk and host objects. Full chunk builds previously took about 20 minutes on the development host. Linking rejects missing/stale objects and dependency files.

Acquire required FFmpeg/window/compiler-runtime DLLs from your dependency build or authorized packages; keep them beside the executable as required by its imports. No DLL bundle is included. Keep the ISO and `extracted/SLUS_211.68` beside the executable. An alternate ELF path can be passed as argument one.

Use arrows/game buttons to select New Game, enter a name and follow prompts. See README for controls. Sound, movies, saves and whole-game behavior remain unfinished. `PS2_GS_BACKEND=cpu` selects diagnostic CPU rendering. Overlay renderer settings apply on restart; the environment override wins.

## Optional PCSX2 GS-only hybrid

Requirements: MSVC **14.40.33807**, Windows SDK **10.0.22621.0**, CMake/Ninja and 7-Zip. `tools/pcsx2_gs/build.py` uses standard BuildTools/SDK paths from the development machine; adapt its constants for different installations.

```powershell
git clone --no-checkout https://github.com/PCSX2/pcsx2.git output/reference_sources/pcsx2
git -C output/reference_sources/pcsx2 checkout --detach 646df006a4688063e56c0826e7ea4b8bb0c112c4
git -C output/reference_sources/pcsx2 submodule update --init --recursive
python tools/pcsx2_gs/build.py toolchain
python tools/pcsx2_gs/bootstrap.py
python tools/pcsx2_gs/build.py consumer
cmake -S tools/PS2Recomp -B tools/PS2Recomp/build -DPS2X_ENABLE_PCSX2_GS=ON
cmake --build tools/PS2Recomp/build --target ps2_runtime --parallel 2
```

Bootstrap verifies the official release asset's SHA256 but resolves the asset at run time instead of pinning an immutable dependency digest in this snapshot. Toolchain/dependency reproducibility remains incomplete.

The manual host builder does not inherit CMake's `NOBLE_HAVE_PCSX2_GS=1`. Compile host objects with that definition, then link:

```powershell
@'
from tools import build_recomp_chunks as build
build.HOST_OPT_FLAGS.append("-DNOBLE_HAVE_PCSX2_GS=1")
results = build.build_host_objects()
for name, ok, seconds, message in results:
    print(name, ok, message)
if not all(row[1] for row in results):
    raise SystemExit(1)
'@ | python -
python tools/link_noble_archimedes.py
$env:PS2_PCSX2_GS_ADAPTER = (Resolve-Path output/fps60/pcsx2_gs_consumer_v1/bin/noble_pcsx2_gs.dll).Path
$env:PS2_PCSX2_GS_RESOURCES = (Resolve-Path output/reference_sources/pcsx2/bin/resources).Path
$env:PS2_GS_BACKEND = "pcsx2"
.\NobleArchimedes.exe
```

Keep package-provided transitive DLLs beside the module. Missing module/resources/ABI support is an initialization error. The adapter does not start a PCSX2 VM: this project's EE/VU/gameplay remain active. No 60 FPS gameplay claim is made. See [adapter details](tools/pcsx2_gs/README.md).

## Validation and diagnostics

```powershell
python -m unittest discover -s tests -p test_build_helpers.py
python -m unittest discover -s tests -p test_guest_entries_generation.py
```

These qualify helpers, not gameplay. Benchmark with screenshots, GS history and detailed tracing disabled. Measure actual guest updates and fresh images with ordinary movement, known module identities and normal shutdown; window FPS or VBlank alone is insufficient. See [status](docs/PROJECT_STATUS.md) and [historical notes](docs/DEVELOPMENT_BUILD_NOTES.md).
