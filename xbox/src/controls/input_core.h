#ifndef NOBLE_INPUT_CORE_H
#define NOBLE_INPUT_CORE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NI_CONFIG_VERSION 1
#define NI_BINDING_SLOTS 2
typedef enum { NI_GAMEPLAY, NI_MENU, NI_CONTEXT_COUNT } NiContext;
typedef enum { NI_KEYBOARD, NI_GAMEPAD, NI_DEVICE_COUNT } NiDevice;
typedef enum { NI_FAMILY_AUTO, NI_FAMILY_KEYBOARD, NI_FAMILY_XBOX, NI_FAMILY_PLAYSTATION, NI_FAMILY_NINTENDO, NI_FAMILY_GENERIC } NiFamily;
typedef enum { NI_NONE, NI_KEY, NI_MOUSE_BUTTON, NI_MOUSE_WHEEL, NI_PAD_BUTTON, NI_PAD_AXIS } NiBindingKind;
typedef enum {
 NI_DPAD_UP, NI_DPAD_DOWN, NI_DPAD_LEFT, NI_DPAD_RIGHT, NI_START, NI_BACK,
 NI_LEFT_CLICK, NI_RIGHT_CLICK, NI_A, NI_B, NI_X, NI_Y, NI_BLACK, NI_WHITE,
 NI_LEFT_TRIGGER, NI_RIGHT_TRIGGER, NI_MOVE_UP, NI_MOVE_DOWN, NI_MOVE_LEFT,
 NI_MOVE_RIGHT, NI_LOOK_UP, NI_LOOK_DOWN, NI_LOOK_LEFT, NI_LOOK_RIGHT,
 NI_ACTION_COUNT
} NiAction;
typedef struct { int kind, code, sign; } NiBinding;
typedef struct {
 unsigned version;
 NiBinding bindings[NI_CONTEXT_COUNT][NI_DEVICE_COUNT][NI_ACTION_COUNT][NI_BINDING_SLOTS];
 float left_inner, left_outer, right_inner, right_outer, trigger_threshold;
 float mouse_sensitivity_x, mouse_sensitivity_y, look_sensitivity_x, look_sensitivity_y;
 int mouse_invert_x, mouse_invert_y, pad_invert_x, pad_invert_y;
 int mouse_camera; /* Emit relative angular intent; host decides safe camera consumption. */
 NiFamily prompt_family;
} NiConfig;
typedef struct {
 uint8_t keys[512]; uint32_t mouse_buttons; int mouse_wheel;
 uint32_t pad_buttons; float axes[6]; /* SDL axes LX,LY,RX,RY,LT,RT; Y positive down. */
 float mouse_dx, mouse_dy; int focused;
 /* Monotonic intentional-use counters, independently for movement/look. */
 uint64_t keyboard_activity[2], pad_activity[2];
} NiRawState;
typedef struct {
 uint16_t buttons; /* Xbox masks: dpad 1/2/4/8, Start16, Back32, L/R click64/128. */
 uint8_t analog[8]; /* Runtime ABI: A,B,X,Y,BLACK,WHITE,LT,RT. */
 int16_t lx,ly,rx,ry; /* Guest Y is positive up. */
 float camera_yaw, camera_pitch; /* mouse counts*sensitivity + stick speed*seconds */
} NiOutput;
void ni_config_defaults(NiConfig *config);
void ni_config_pc_defaults(NiConfig *config);
int ni_config_valid(const NiConfig *config);
/* Load always initializes output to defaults on failure; never modifies the source file. */
int ni_config_load(NiConfig *config, const char *path);
int ni_config_save(const NiConfig *config, const char *path);
void ni_deadzone(float x,float y,float inner,float outer,float *out_x,float *out_y);
/* Pure evaluation; caller owns snapshots and consumes mouse deltas once. */
void ni_evaluate(const NiConfig *config,const NiRawState *raw,NiContext context,float seconds,NiOutput *out);
const char *ni_action_name(NiAction action);
void ni_binding_label(NiBinding binding,NiFamily family,char *label,size_t capacity);
int ni_binding_conflict(const NiConfig *config,NiContext context,NiDevice device,NiAction action,int slot,NiBinding binding);
#ifdef __cplusplus
}
#endif
#endif
