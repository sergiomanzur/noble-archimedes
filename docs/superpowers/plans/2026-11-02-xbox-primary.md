# Noble Archimedes Xbox-primary integration

Goal: make the imported Xbox D3D12 recomp the primary bring-your-own-game product, keeping PS2 explicitly experimental. Retain recompiler/DLL generation, renderer/fixes, game options, USA/EU hooks and PS2 HD pack compatibility. Brand the launcher and executable as Noble Archimedes with original imagery, icon and music. Preserve upstream provenance and provide explicit fork-friendly licenses.

Architecture: keep the 60-file donor snapshot pristine. Maintain a reviewed patch/overlay for branded CMake/runtime/launcher source; deterministic preparation applies it to the isolated output workspace. Preserve original generated-code ABI names internally where needed, but use Noble Archimedes executable output names. Keep all generated game code/assets private. Build the Xbox path by default at the repository root; opt into PS2. Preserve existing work in place.

Visual direction (implementation choice): midnight teal, warm brass, celestial/architectural astrolabe mark, original observatory illustration; no game logos, extracted icon or game soundtrack. Original procedurally composed chamber/ambient loop, toggleable and stopped on Play/exit. Interface text names the game as required input, separate from the product name.

- [x] Audit mixed licenses and write explicit scoped licenses, attribution and source/build/fork instructions.
- [x] Create original illustration/icon and reproducible original loop; retain asset provenance and licenses.
- [x] Add branded launcher overlay preserving GPU, file, region, graphics, texture and game options; test save/mute lifecycle.
- [x] Integrate patch/overlay preparation, branded executable outputs, main root build and launch scripts, USA/EU staging and bring-your-own-game startup workflow.
- [x] Build and run focused tests plus automated new-game smoke scenario; inspect launcher visually; review final integration.

Constraints: preserve ongoing PS2/legacy Xbox edits; no game bytes/captures/DLLs in tracked or distributable source; retain upstream license terms; do not claim EU gameplay without an EU copy. No publication or push requested.

Verification: full branded build and incremental resource correction exit 0. Five main CTest groups / 23 checks passed. Branded newgame scenario passes 7/7 steps. Isolated no-DLL first-run test generated the game DLL from supplied XBE in about 85 seconds and passed 8/8 startup/gameplay steps; Windows-only PATH, but not a clean-Windows redistribution test. Launcher visually inspected; own icon/background visible and music-off persisted. Fresh reviewer found and resolved trademark substitution in generated metadata. Source inventory stays pristine. EU image and HD texture pack unavailable: those integrations remain unqualified locally.
