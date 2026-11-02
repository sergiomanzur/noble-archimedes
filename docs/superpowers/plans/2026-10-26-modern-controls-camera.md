# Modern Controls and Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver the approved Windows Xbox modern-controls feature with configurable actions, SDL3 gamepads, a usable Controls panel, mouse-camera integration, and binding-aware prompts.

**Architecture:** A pure C action/configuration core feeds a thread-safe host provider that both Xbox input read paths use. SDL3 input is host-owned and shares one event dispatcher with the renderer. Game-specific camera and prompt hooks require verified integration; compatibility features remain labeled as such.

**Tech Stack:** C11, Win32 UI, SDL3 3.4.18 bundled SDK, existing CMake/MSVC Xbox build.

**Spec:** `docs/superpowers/specs/2026-10-26-modern-controls-camera-design.md`

## Global Constraints

- Work in the existing non-main `xbox-60fps` checkout: uncommitted correctness repairs, generated sources and local game inputs are required. Do not revert, stage, or commit unrelated work.
- User approved the design and explicitly requested execution. Continue without another plan-approval stop.
- Preserve renderer defaults, DSP settle, all render passes, existing deterministic scripted-pad behavior, and guest ABI.
- Input core is independent of SDL and renderer. Host events are pumped on the SDL-owning main thread; API/USB workers read published state only.
- No per-frame config/file IO or rescanning connected devices. New diagnostics default off.
- Implement and test meaningful behavior before success claims; native mouse camera and original game prompts cannot be claimed without verified hooks.

## Review Focus

- A held button during disconnect/Alt-Tab must release; neutral devices must not steal prompt ownership.
- Both USB and direct API readers must see one consistent packet and not consume mouse movement twice.
- Rebinding confirm/back must not strand the Controls panel; F2 recovery and cancel/defaults remain available.
- Unknown/duplicate controllers must have explicit mappings/assignment, never duplicate XInput+SDL input.
- Scripted benchmark replay must remain independent of physical input, UI, and settings changes.

## Task 1 — portable action/configuration core

Files: `xbox/src/controls/input_core.h`, `input_core.c`, `input_config.c`; `xbox/tests/modern_input_core_test.c`.

Interface: `NiConfig`, `NiBinding`, `NiRawState`, `NiOutput`; `ni_config_defaults`, `ni_config_load`, `ni_config_save`, `ni_evaluate`, `ni_deadzone`, `ni_binding_label`, `ni_action_name`. Keyboard uses SDL scancode numeric values, gamepad uses SDL standardized button/axis numeric values but header has no SDL dependency. Two bindings per device family/action. Guest output is a plain button/analog/stick snapshot, not platform typedefs.

- [ ] Write and run failing behavior tests for bindings/contexts, radial zones, analog triggers, diagonal/opposite keys, inversion, config roundtrip/corruption and prompt labels.
- [ ] Implement pure mapping and versioned atomic configuration save with bounds validation.
- [ ] Run tests; review core interface and consumption semantics.

## Task 2 — SDL3 host provider and existing runtime bridge

Files: `xbox/src/controls/input_host.h`, `input_host.c`; runtime `tools/xboxrecomp/src/input/xinput_device.c`, `xinput_xbox.h`; renderer `xbox/src/d3d8gl/stubs.c` integration coordinated by parent.

Interface: `noble_input_init`, `noble_input_pump`, `noble_input_event(const void*)`, `noble_input_shutdown`, `noble_input_read(port,state)`, provider-connected/capabilities/rumble callbacks, `noble_input_open_controls`. Parent registers provider before `xbox_InputInit`, calls pump once on host frame boundary and forwards SDL events from renderer through a registered callback. `RECOMP_MODERN_INPUT=0` is legacy fallback; scripts bypass physical state.

- [ ] Test published packet reuse, release/focus/disconnect, virtual gamepad hotplug and four port slots.
- [ ] Implement SDL gamepad/joystick lifecycle, raw mouse/key routing, immutable snapshots, preference/assignment, rumble routing and shutdown.
- [ ] Add optional provider callbacks to runtime without breaking legacy/non-Windows behavior; missing callback preserves original path.
- [ ] Verify SDL initialization/event ownership before/after GL window creation and in software mode.

## Task 3 — Controls panel and device mapping

Files: `xbox/src/controls/controls_panel.c`, host/core interfaces as necessary; `xbox/CMakeLists.txt`, `xbox/tests/CMakeLists.txt`; documentation `xbox/README.md`.

- [ ] Build keyboard/controller-accessible Win32 Controls panel with action list, two bindings, listen/clear, Apply/Cancel/Defaults, conflict feedback, sensitivity/inversion, live axes, dead zones, family preference, port assignment and unknown joystick mapping.
- [ ] F2 always opens recovery. Panel suppresses gameplay and releases mouse; close/apply clears transient held input.
- [ ] Persist config under `%APPDATA%/NobleArchimedes/controls.ini`; document paths, defaults and compatible devices.
- [ ] Test UI model state and config mutation; manually verify visible panel, cancellation, focus and keyboard navigation.

## Task 4 — title camera and in-game prompt integration

Files depend on audited title hooks; tracked host adapter under `xbox/src/controls/`, reproducible `xbox/tools/prepare_generated.py` hooks only if verified.

- [ ] Trace right-stick and camera update to identify angular input and script/menu/lock-on state. Verify guest address/signature/side effects before patching.
- [ ] Implement native relative mouse camera preserving collision/lock-on/scripted ownership, one consumption per logical update, FPS-independent mouse counts, stick dt semantics and transitions; test camera math and guest ABI.
- [ ] Trace original prompt text/glyph path. Implement binding-aware prompt labels through verified hook, active-device hysteresis and manual family override; do not claim host-panel labels replace original game prompts.
- [ ] If hook discovery is blocked, record exact evidence and preserve explicit compatibility mode without false completion.

## Task 5 — combined verification and review

- [ ] Configure/build standalone tests and host; run CTest, real-GPU/ABI tests and virtual-device controls tests.
- [ ] Run scripted HLE capture smoke test and verify normal gamepad/keyboard actions, no new always-on logging or per-frame file IO.
- [ ] Manually inspect panel and test disconnect/focus/reconnect using virtual pads; record available real hardware separately.
- [ ] Independent final review; repair actionable findings, update ledger and report delivered features and any verified title-hook limitations.

## Execution ledger

Plan progress lives in `output/modern_controls/progress.md`. Parent owns build/CMake/event integration and panel; workers own only explicitly assigned controls modules or read-only hook investigation. No worker stages broad dirty-tree changes.
