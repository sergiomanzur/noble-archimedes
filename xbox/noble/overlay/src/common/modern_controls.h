// Modern PC controls for the imported Xbox build: relative mouse look, a
// Resident Evil 2 Remake style over-the-shoulder camera, and a close
// first-person camera. Settings live in castlevania.toml, so the launcher and
// the in-game menu share them and unknown keys survive either writer.
#pragma once

#include <cstdint>

namespace input {
struct Pad;
}

namespace modern {

enum CameraMode : int {
    kClassic = 0,       // the original game camera, without the free-look offset
    kOverShoulder = 1,  // RE2 Remake style close, offset orbit camera
    kFirstPerson = 2,   // camera at Hector's eye line with direct look
};

// Called by app::Load/app::Save with the parsed settings table.
void LoadSettings(const void* table);
void SaveSettings(void* table);

// Main-thread/game-thread per-frame work. `scripted` disables physical mouse
// input during CV_PAD_SCRIPT/CV_PAD_LIVE runs. `frame` is the game's swap
// counter so several pad reads in one frame consume the mouse only once.
void Frame(bool focused, bool menu_open, bool scripted, uint32_t frame);
// The settings menu can be opened without a pad read; release capture at once.
void SetMenuOpen(bool open);
bool MouseCaptured();

// Adds the modern mouse buttons and aim behaviour to the pad the game is
// about to read. Call after the keyboard mapping; scripted runs skip it.
void ApplyPad(input::Pad& pad);
// Filter controller input before adding keyboard, mouse or scripted input.
void ApplyStickDeadzones(input::Pad& pad);

// Applies the selected camera to the guest camera view after the original
// title camera update has run. Safe to call for every camera update; it does
// nothing outside normal gameplay or when the camera is Classic.
void AfterCameraUpdate(uint8_t* base);

// In-game Camera tab. Returns true when a setting changed and should be saved.
bool DrawSettings();

void ResetDefaults();
void CycleCameraMode();
void ToggleMouseCapture();
// Releases gameplay capture when the game closes.
void Shutdown();

int GetCameraMode();
void SetCameraMode(int mode);
float GetMouseSensitivity();
void SetMouseSensitivity(float value);
bool GetInvertX();
void SetInvertX(bool value);
bool GetInvertY();
void SetInvertY(bool value);
float GetShoulderOffset();
void SetShoulderOffset(float value);
float GetCameraDistance();
void SetCameraDistance(float value);
float GetFirstPersonOffset();
void SetFirstPersonOffset(float value);
bool GetMouseCapture();
void SetMouseCapture(bool value);

}  // namespace modern
