// Android: the imported modern mouse controls are Windows-only (relative mouse,
// window client geometry). The game keeps its original camera and controller
// mapping; the touch overlay and SDL gamepads drive both sticks. These are the
// same entry points with no mouse work.
#include "common/modern_controls.h"
#include "core/input.h"

namespace modern {

void LoadSettings(const void*) {}
void SaveSettings(void*) {}
void Frame(bool, bool, bool, uint32_t) {}
void SetMenuOpen(bool) {}
bool MouseCaptured() { return false; }
void ApplyPad(input::Pad&) {}
void ApplyStickDeadzones(input::Pad&) {}
void AfterCameraUpdate(uint8_t*) {}
bool DrawSettings() { return false; }
void ResetDefaults() {}
void CycleCameraMode() {}
void ToggleMouseCapture() {}
void Shutdown() {}

int GetCameraMode() { return kClassic; }
void SetCameraMode(int) {}
float GetMouseSensitivity() { return 1.0f; }
void SetMouseSensitivity(float) {}
bool GetInvertX() { return false; }
void SetInvertX(bool) {}
bool GetInvertY() { return false; }
void SetInvertY(bool) {}
float GetShoulderOffset() { return 1.0f; }
void SetShoulderOffset(float) {}
float GetCameraDistance() { return 1.0f; }
void SetCameraDistance(float) {}
float GetFirstPersonOffset() { return 0.0f; }
void SetFirstPersonOffset(float) {}
bool GetMouseCapture() { return false; }
void SetMouseCapture(bool) {}

}  // namespace modern
