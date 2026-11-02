# Modern controls and camera — proposed design

Date: 2026-10-26. Status: approved by the user ("please proceed").

## Intent and scope

Make the Windows Xbox recomp feel like a native PC game: configurable keyboard,
mouse, and controller actions; dependable device changes; and camera control
appropriate to Curse of Darkness. Preserve the existing rendering, audio/DSP,
save behavior, and benchmark work.

The user requested fully remappable keyboard/controller bindings, mouse camera,
sensitivity, inversion, dead zones, automatic button prompts, hot-plugging, and
XInput/USB controller support. The scope choices below are the orchestrator's
proposal, not previously approved implementation details.

First integration target: `xbox/build/Release/curse_of_darkness_xbox_recomp.exe`.
Input must operate with both software presentation and D3D8GL; input availability
must not depend on loading the renderer DLL. PS2 adoption is a later integration,
using portable action/configuration components where practical.

## Existing evidence

- `tools/xboxrecomp/src/input/xinput_device.c`: Windows XInput polling plus a
  hard-coded keyboard merge. Both guest API and emulated USB reads reach this
  layer; route through a shared provider rather than adding a competing path.
- `tools/xboxrecomp/src/input/keyboard.c`: fixed virtual Xbox button/axis map.
- `tools/xboxrecomp/src/input/mkb_input.c`: opt-in Win32 raw input, currently
  shooter-oriented bindings and mouse-to-stick conversion. It is not a verified
  native camera implementation for this title.
- `xbox/src/d3d8gl/stubs.c`: SDL3 video/context and event polling, currently
  discarding events other than quit.
- `src/input/` and `src/camera/`: separate PS2/raylib experiments; do not assume
  their camera implementation or memory addresses apply to Xbox.
- SDL3 headers/runtime already exist locally under `output/sdl3/`.

## User-visible behavior

### Devices and controller identity

Use SDL3 gamepad support as the common mapped-controller backend on Windows,
including available XInput and other supported controller drivers. Do not poll
the same physical device independently through both SDL and direct XInput.

Support mapped Xbox-style, PlayStation-style, Nintendo-style, and generic USB
gamepads; wired and wireless devices where the OS/SDL exposes them. USB describes
the connection, not a universal controller layout. Unrecognized joystick devices
get an axis/button/hat mapping workflow that persists an SDL mapping. Do not claim
every arbitrary USB HID device is supported.

Preserve up to four virtual guest ports. Port 1 is the gameplay default; the
Controls panel assigns physical devices to ports and remembers a preferred
identity when available. Disconnecting releases held inputs and stops rumble.
Reconnect preserves bindings and offers the device again without restarting.
Do not silently let another device take over an occupied assigned port. Where
identical devices lack stable unique identity, ask for reassignment in the panel
rather than claiming stable identification.

### Actions and bindings

Separate physical inputs, semantic actions, and guest-pad output. Audit the actual
Xbox title controls before naming gameplay actions; do not import PS2 or generic
shooter semantics without verification. Include movement/look axes, attack,
jump, interaction, guard/lock-on and Innocent Devil actions as verified, menu
navigation/confirm/back, pause, map, and all remaining original pad functions.

Allow two bindings per action per keyboard/mouse and controller profile. Support
keyboard keys, mouse buttons/wheel, controller buttons/hats, signed axis bindings,
and analog triggers. Preserve analog trigger values. Provide gameplay and menu
contexts so remapping a combat action does not accidentally break confirmation.

Rebinding is available through a Controls panel, with listen/cancel/clear,
conflict reporting, Apply, Cancel, and Restore Defaults. Conflicting bindings may
be explicitly allowed; do not silently replace them. Provide a permanent F2
recovery shortcut to open Controls even if navigation bindings are removed.
The first panel may use native Windows UI; polished in-game presentation can
follow. It must be usable with keyboard and controller, not require hand edits.

Keep a Classic preset matching the current gamepad layout and a PC preset with
WASD movement and mouse camera. Remaining default action keys follow the verified
game controls. All defaults are displayed before Apply; actual key choices are
left to the builder after the action audit. Ordinary interactive keyboard use
must not require environment variables.

Allow keyboard and controller use together. Merge action buttons; choose the
most recently intentionally used source for each analog action, with hysteresis,
rather than adding two full stick vectors. Normalize keyboard diagonals and
cancel opposite keys. UI consumption prevents the same input reaching gameplay.

### Camera

Retain a Classic mode and offer Mouse Camera. Provide separate mouse X/Y
sensitivity, controller look X/Y sensitivity, and per-device X/Y inversion.
Mouse uses relative motion with no OS pointer acceleration; sensitivity is
angular change per mouse count. Stick sensitivity is angular speed over elapsed
game time. Mouse counts must not be multiplied by delta time a second time.

Audit the Xbox camera update and right-stick handling before implementation.
The intended native mouse path feeds angular intent into the game's camera
update while retaining original collision, lock-on, recentering, and scripted
camera behavior. Do not bypass those systems by overwriting the view matrix.
No over-the-shoulder mode, custom collision system, or FOV work is included here.

If no safe angular hook exists, a mouse-to-stick adapter may be exposed as an
explicit interim compatibility mode. It is not the completed native mouse-camera
feature and must not be presented as one. Hook discovery is a real dependency.

Consume relative mouse deltas exactly once per logical camera update, irrespective
of repeated guest API/USB reads. Do not accumulate camera motion through menus,
cutscenes, pauses, lost focus, or reconnects. Clamp/validate elapsed intervals
after stalls to prevent stick-camera jumps.

### Dead zones and focus

Provide independent left/right radial inner and outer dead zones, trigger
thresholds, live axis visualization, sensitivity preview, and reset defaults.
Apply normalization/conditioning once. Avoid accidentally stacking host and
game dead zones; audit and document the game-side filter before compensating it.

Ignore gameplay input when unfocused. Release keys/buttons, clear pending mouse
motion, stop rumble, and release cursor capture on focus loss or panel entry.
Capture the mouse only during active mouse-camera gameplay. Restore it after a
deliberate return to gameplay; first motion after refocus must not jump.

### Automatic prompts

Switch prompt family after meaningful input, not idle stick noise or mere device
connection. Provide Auto and manual Keyboard/Xbox/PlayStation/Nintendo/Generic
choices. Resolve labels from the current binding, including remapped mouse/key
labels and controller layout. Generic devices use readable numbered labels when
their button family is unknown. Physical-position defaults avoid accidental
Nintendo/Xbox face-button swaps.

Host Controls UI and gameplay/tutorial prompts share the same action-to-label
resolver. Updating original in-game prompts requires a verified UI/text/texture
hook; device detection or a host status label alone does not complete this
requirement. Ship newly drawn glyph assets or text fallbacks. Do not distribute
extracted game UI assets.

## Technical boundaries

- Portable pure action mapping/configuration core, host device backend, immutable
  input snapshots, guest-pad adapter, and title-specific camera/prompt adapters.
- Host owns SDL3 input initialization, event dispatch and shutdown independently
  of renderer selection. Exactly one main-thread dispatcher pumps SDL events;
  the renderer must forward events rather than drain a competing queue.
- Input API and USB worker reads consume thread-safe published snapshots. They
  must not pump SDL, independently consume mouse deltas, or perform hot-plug
  enumeration. Explicitly preserve connection/capability/packet-number semantics.
- Rumble requests are routed to the assigned physical device through the provider
  with safe disconnect/shutdown handling. Preserve existing basic rumble where
  hardware supports it; advanced adaptive-trigger features are out of scope.
- Save a versioned, human-readable configuration under the user's application
  data directory. Use atomic replacement, validation, and safe fallback for
  malformed/unknown values. Separate controller-family/GUID mappings from an
  optional device-specific override; persist only supported identity information.
- Keep host integration in tracked sources/preparation hooks. Do not rely on
  edits to ignored generated game code. Preserve unrelated dirty-tree work.
- Scripted benchmark input has explicit deterministic priority. Physical input,
  new UI, and device activity must not change existing scheduled-pad replays.
- No per-frame file I/O, configuration parsing, device rescans, or default-on
  tracing. Measure overhead with input enabled, including with no pad connected.

SDL references: https://wiki.libsdl.org/SDL3/CategoryGamepad,
https://wiki.libsdl.org/SDL3/SDL_PumpEvents,
https://wiki.libsdl.org/SDL3/SDL_GetGamepadType.

## Delivery order and acceptance

1. Input foundation: audited action map, configuration model, SDL3 backend,
   provider bridge, preserved scripted input, hot-plug/focus behavior. Validate
   both guest input paths and renderer independence.
2. User controls: keyboard/controller-accessible panel, remapping, calibration,
   device assignment, persistence, conflict handling, basic rumble.
3. Camera integration: verified title hook, relative mouse, original camera
   safeguards, sensitivity/inversion, game-state transitions.
4. Prompts and complete qualification: native UI hook and binding-aware labels;
   test keyboard/controller switching, menus, gameplay, and tutorial prompts.

These are milestones of the requested feature, not permission to declare the
whole feature finished after the input foundation.

Automated tests cover mapping/contexts, trigger and axis boundaries, dead zones,
configuration recovery, packet changes, duplicate readers, mouse consumption,
focus/disconnect release, identity/port assignment, prompt hysteresis, scripted
replay precedence, and camera math across update rates.

Use virtual controllers for deterministic connect/disconnect and mutation tests.
Physical qualification includes an XInput pad and at least one non-XInput USB pad
when available; report which hardware was actually tested. No hardware access is
assumed. Preserve existing host/GPU/ABI tests and capture scene correctness.

Manually verify startup with no controller, connecting while in menu/gameplay,
disconnecting while an action/stick is held, reconnecting, two identical devices,
Alt-Tab, rebinding essential controls, cursor escape, lock-on, camera collision,
scripted camera transitions, and prompt labels after remapping.

Do not claim complete native mouse camera or in-game automatic prompts until
their title-specific hooks are verified in actual gameplay.

## Open implementation investigations

Camera update hook, camera-active gameplay/menu/cutscene state, original prompt
hook, exact title action meanings, and SDL event ownership during pre-window
boot need investigation. The builder chooses the smallest verified hooks and
records evidence. If a required hook cannot be found safely, report a specific
blocker and keep the partial milestone labeled accurately; do not substitute a
fake-complete overlay or guessed guest address.
