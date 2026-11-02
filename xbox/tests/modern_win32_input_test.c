#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/controls/input_win32.h"
static int registered;
void noble_input_event(const void *event) { (void)event; }
void noble_input_set_focus(int active) { (void)active; }
void noble_input_set_native_source(int active) { registered=active; }
int noble_input_mouse_capture_active(void) { return 0; }
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1; } }while(0)
int main(void) {
    CHECK(noble_win32_scancode(0x1e,0)==SDL_SCANCODE_A);
    CHECK(noble_win32_scancode(0x11,0)==SDL_SCANCODE_W);
    CHECK(noble_win32_scancode(0x3c,0)==SDL_SCANCODE_F2);
    CHECK(noble_win32_scancode(0x1d,0)==SDL_SCANCODE_LCTRL);
    CHECK(noble_win32_scancode(0x1d,RI_KEY_E0)==SDL_SCANCODE_RCTRL);
    CHECK(noble_win32_scancode(0x48,0)==SDL_SCANCODE_KP_8);
    CHECK(noble_win32_scancode(0x48,RI_KEY_E0)==SDL_SCANCODE_UP);
    CHECK(noble_win32_scancode(0x1c,RI_KEY_E0)==SDL_SCANCODE_KP_ENTER);
    CHECK(noble_win32_scancode(0xff,0)==SDL_SCANCODE_UNKNOWN);
    CHECK(noble_win32_mouse_button(0)==SDL_BUTTON_LEFT);
    CHECK(noble_win32_mouse_button(1)==SDL_BUTTON_RIGHT);
    CHECK(noble_win32_mouse_button(2)==SDL_BUTTON_MIDDLE);
    CHECK(noble_win32_input_init()); CHECK(registered);
    noble_win32_input_pump(); noble_win32_input_shutdown(); CHECK(!registered);
    puts("Win32 input: physical keys and fallback lifecycle pass");return 0;
}
