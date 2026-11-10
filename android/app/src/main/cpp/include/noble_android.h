#pragma once
#include <cstdint>

namespace input {
struct Pad;
}

namespace noble_android {

// Gameplay pad state produced by the Android touch overlay. Physical controllers
// and the touch pad are merged: buttons OR together, sticks take the larger
// deflection, so either can drive the game at any time.
void MergeTouchPad(int port, input::Pad& pad);

// The game's first usable frame is up; the Java splash may fade away.
void SetBooted();
bool IsBooted();

// The Java overlay's settings menu bridge.
void ToggleMenu();
bool MenuOpen();

}  // namespace noble_android
