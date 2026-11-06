# Modern PC controls (primary Xbox D3D12 build)

`NobleArchimedes.exe` now has native mouse look, a Resident Evil 2 Remake-style
over-the-shoulder camera, a first-person camera, and a small set of PC mouse
bindings. The work is delivered through the product overlay, so the pinned
donor snapshot in `xbox/third_party/castlevania-cod-pc` stays unchanged.

## Camera modes

Open the in-game settings with **F10** and use the **Camera** tab, or press
**F5** during play to cycle modes.

| Mode | Behaviour |
|---|---|
| Classic | The original follow camera, unchanged. Free mouse look is not applied in this mode. |
| Over-the-shoulder (default) | Orbits the camera around Hector at the current distance, then offsets it to the right. This is the RE2 Remake-style view. |
| First person | Places the camera at Hector's eye line. No extra collision probe is needed because the player's own collision keeps the eye point out of walls. |

The camera only changes during ordinary gameplay. Title screens, movies, story
scenes, room transitions, the pause/inventory menu, message boxes, the map
screen and debug views keep the original camera.

## Keyboard and mouse

| Input | Action |
|---|---|
| `W A S D` | Move, relative to the camera |
| Mouse | Look; captured automatically during gameplay |
| Left mouse / `J` | X (attack) |
| Right mouse (hold) | Aim: slower walk and a closer camera |
| Middle mouse / `I` | Y |
| Mouse 4 / Mouse 5 | Black / white |
| `K` or `Space` | A (jump) |
| `L` | B |
| `Q` | Left trigger |
| `E` | Right trigger |
| `Z` | White |
| `C` | Black |
| `Enter` | Start |
| `Backspace` | Back |
| Arrow keys | Camera / right stick |
| Numpad `8 2 4 6` | D-pad |
| `F5` | Cycle Classic → over-the-shoulder → first person |
| `F6` | Release or recapture the mouse |
| `F10` | Settings menu (Camera tab); `Escape` closes it |

A connected controller still works alongside the keyboard and mouse. The
mouse buttons are additions, not replacements: the existing keyboard and pad
bindings remain available.

The title's controller camera is inverted by default. Two Camera-tab options,
**Invert camera stick X** and **Invert camera stick Y**, are enabled by default
to correct it. They affect both Classic and direct-look modes; uncheck either
one to use the title's original inverted stick on that axis.

## Mouse capture

The cursor is captured only while the game is in normal gameplay and the game
window has focus. Opening the F10 menu, Alt-Tabbing, losing focus, entering a
movie or cutscene, and pressing **F6** all release it. A small centered dot appears during capture. Inside the game window, the system arrow is hidden, including when gameplay capture is off. The normal pointer remains available outside the game client.

Mouse travel changes the camera angle once and directly. Stopping the mouse
stops the camera immediately; there is no residual stick value or continued
turning. The camera owns an absolute heading, while the title still supplies
the player target, a collision-informed distance and scripted camera changes.

Movement in this title is relative to its own follow camera. The host steers
that camera toward the same heading with the right-stick channel, so `W` goes
where the mouse is pointing instead of continuing along the old character
path. While moving, the game turns the character toward the movement direction;
while idle, the host turns the character to the camera heading.

## Settings

The Camera tab writes the following keys to `castlevania.toml` beside the
executable. The launcher preserves unknown keys, so the settings survive a
launcher save.

| Key | Default | Meaning |
|---|---:|---|
| `modern_camera_mode` | `1` | `0` Classic, `1` over-the-shoulder, `2` first person |
| `modern_mouse_sensitivity` | `0.15` | Degrees of camera travel per mouse pixel |
| `modern_invert_x` | `false` | Flip horizontal mouse look |
| `modern_invert_y` | `false` | Flip vertical mouse look |
| `modern_invert_stick_x` | `true` | Correct the title's inverted controller camera X |
| `modern_invert_stick_y` | `true` | Correct the title's inverted controller camera Y |
| `modern_shoulder_offset` | `120` | Guest units to the right of the safe camera line |
| `modern_camera_distance` | `620` | Guest units from Hector in over-the-shoulder mode |
| `modern_first_person_offset` | `0` | Extra forward distance beyond the default head clearance |
| `modern_mouse_capture` | `true` | Capture the mouse during gameplay |

First person follows Hector’s current position with a fixed eye height and forward clearance. Looking up or down rotates the view without moving the eye point. Over-the-shoulder offset decreases when the original camera is close to a wall.

## Scripted and recorded input

`CV_PAD_SCRIPT` and `CV_PAD_LIVE` runs keep deterministic priority: physical
mouse input and cursor capture are disabled for those runs. The camera mode
still renders normally, so a test that needs the original camera should set
`modern_camera_mode = 0` in the scenario settings.

## Validation

- `xbox/tests/modern_camera_math_test.cpp` checks the title's column-major view
  matrix, the over-the-shoulder pull and shoulder clamp, and first-person eye
  placement.
- A fresh `newgame.json` cvtest run passes all seven steps with the new camera
  active.
- Manual checks: F10 Camera tab, F5 with the window focused, F6 release and
  recapture, and Alt-Tab release.

In-engine movies, menus and the title screen still use the original camera.

## Known limits

- The title's follow camera has its own maximum turn rate. A very fast mouse
  swing can leave movement briefly aligned with the previous heading while
  that camera catches up; normal turns and gradual flicks align immediately.
- Over-the-shoulder uses the title camera's current distance as the orbit
  radius. The base distance and shoulder offset were collision-safe for the
  original heading, but a large free-look rotation near a wall can still clip.
  Lower `modern_camera_distance` or `modern_shoulder_offset` in tight rooms.
- Classic mode deliberately keeps the original camera and does not apply the
  free-look offset. Switch to over-the-shoulder or first person for direct
  mouse look.
- There is one sensitivity for both axes; per-axis tuning and a separate
  vertical scale are not exposed yet.
- The in-game button prompts remain the original artwork. The F10 Controls tab
  lists the host bindings, but the game's tutorial and HUD glyphs are unchanged.
- First person has no visible hands/weapon and does not hide Hector's model in
  a cutscene; it is a gameplay camera only.
