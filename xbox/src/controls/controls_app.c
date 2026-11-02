/* Standalone configuration entry point: no extracted game assets required. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "input_host.h"
#include "controls_panel.h"
#include "input_win32.h"

static int quit_requested;
static void pump_ui(void)
{
    noble_win32_input_pump();
    controls_panel_pump();
}
static void dispatch(const void *opaque)
{
    const SDL_Event *event = (const SDL_Event *)opaque;
    controls_panel_event(opaque);
    if (event->type == SDL_EVENT_QUIT) quit_requested = 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    (void)instance; (void)previous; (void)command; (void)show;
    /* These are process-local: setup must work even in a benchmark shell. */
    _putenv_s("RECOMP_MODERN_INPUT", "1");
    _putenv_s("RECOMP_PAD_SCRIPT", "");
    _putenv_s("RECOMP_PAD_LIVE", "");
    if (!noble_input_init()) {
        MessageBoxA(NULL, "Unable to initialize SDL controller input.",
                    "Noble Archimedes Controls", MB_OK | MB_ICONERROR);
        return 1;
    }
    noble_input_set_open_controls(controls_panel_open);
    noble_win32_input_init();
    noble_input_set_ui_pump(pump_ui);
    noble_input_set_dispatch(dispatch);
    controls_panel_open();
    while (!quit_requested && controls_panel_is_open()) {
        noble_input_pump();
        SDL_Delay(8);
    }
    controls_panel_shutdown();
    noble_win32_input_shutdown();
    noble_input_shutdown();
    return 0;
}
