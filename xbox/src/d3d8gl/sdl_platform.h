#ifndef NOBLE_D3D8GL_SDL_PLATFORM_H
#define NOBLE_D3D8GL_SDL_PLATFORM_H

#include "platform.h"

/* The small subset of OpenCE's input structs the D3D8 device uses. */
struct platform_input_state
{
    unsigned char keys[256];
    unsigned char mouse_buttons[8];
    float mouse_dx, mouse_dy;
    float mouse_wheel;
    BOOL focused;
    BOOL mouse_released;
    BOOL ui_pointer;
    BOOL menus;
};

#endif
