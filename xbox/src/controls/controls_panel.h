#ifndef NOBLE_CONTROLS_PANEL_H
#define NOBLE_CONTROLS_PANEL_H
/* Modeless main-thread Win32 panel. Host pump calls pump, no worker UI calls. */
void controls_panel_open(void);
void controls_panel_pump(void);
void controls_panel_event(const void *sdl_event);
void controls_panel_shutdown(void);
int controls_panel_is_open(void);
#endif
