#ifndef NOBLE_INPUT_WIN32_H
#define NOBLE_INPUT_WIN32_H
/* Raw-input fallback for the Win32 software window, all main-thread owned. */
unsigned noble_win32_scancode(unsigned make_code, unsigned flags);
unsigned noble_win32_mouse_button(unsigned flag_pair);
int noble_win32_input_init(void);
void noble_win32_input_pump(void);
void noble_win32_input_shutdown(void);
#endif
