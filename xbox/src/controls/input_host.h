#ifndef NOBLE_INPUT_HOST_H
#define NOBLE_INPUT_HOST_H
#include "input_core.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Main thread lifecycle/event/config APIs. Readers and rumble are worker safe. */
int noble_input_init(void);
void noble_input_pump(void);
void noble_input_event(const void *sdl_event);
void noble_input_shutdown(void);
unsigned long noble_input_read(unsigned long port, void *xbox_state);
int noble_input_connected(unsigned long port);
unsigned long noble_input_capabilities(unsigned long port, unsigned long flags, void *caps);
unsigned long noble_input_rumble(unsigned long port, const void *vibration);
void noble_input_get_config(NiConfig *config);
int noble_input_apply_config(const NiConfig *config);
void noble_input_preview(unsigned port, NiRawState *raw, NiOutput *output);
/* Main-thread title adapter: preview remains normalized, only guest sticks adapt.
 * Passing NULL preserves the pure core's sticks. Scripts/legacy bypass adapters. */
void noble_input_set_guest_axis_adapter(void (*adapter)(NiOutput *output));
/* Invert clamp(length(stick/256)*scale+bias,0,1) for both normalized sticks.
 * Invalid coefficients leave all output fields unchanged and return zero. */
int noble_input_inverse_guest_axes(NiOutput *output, float scale, float bias);
void noble_input_set_panel_active(int active);
void noble_input_set_focus(int focused);
void noble_input_set_context(NiContext context);
void noble_input_set_open_controls(void (*callback)(void));
void noble_input_set_dispatch(void (*callback)(const void *));
void noble_input_set_ui_pump(void (*callback)(void));
void noble_input_open_controls(void);
void noble_input_key(unsigned scancode, int down);
NiFamily noble_input_family(void);
/* Last deliberate binding event is consumed by the panel listen workflow. */
int noble_input_take_binding(NiBinding *binding, NiDevice *device);
typedef struct { unsigned id; int port, mapped; char name[128], guid[40], serial[128]; } NobleInputDevice;
int noble_input_devices(NobleInputDevice *devices, int capacity);
int noble_input_assign(unsigned id, int port); /* -1 unassigns */
int noble_input_add_mapping(const char *sdl_mapping);
/* Native camera adapter may consume once; input reads never consume motion. */
void noble_input_take_mouse(float *dx, float *dy);
int noble_input_mouse_capture_active(void);
float noble_input_camera_seconds(void);
void noble_input_set_native_source(int active);
#ifdef __cplusplus
}
#endif
#endif
