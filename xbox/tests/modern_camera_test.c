#include "../src/controls/input_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
/* Host boundary fixture: no SDL, window, or real input devices required. */
static NiConfig config;
static float dx,dy;
static unsigned takes;
static float camera_seconds=1.0f/60.0f;
float noble_input_camera_seconds(void) { return camera_seconds; }
void noble_input_get_config(NiConfig *out) { *out=config; }
void noble_input_take_mouse(float *x,float *y) { *x=dx; *y=dy; dx=dy=0; ++takes; }
/* API declarations kept here so the baseline can demonstrate missing behavior. */
double noble_camera_adjust(unsigned axis,double original_delta);
static void near_value(double actual,double expected) { assert(fabs(actual-expected)<1e-5); }
int main(void) {
 memset(&config,0,sizeof config);
 config.mouse_sensitivity_x=.0025f; config.mouse_sensitivity_y=.005f;
 config.look_sensitivity_x=config.look_sensitivity_y=2.0943950862f;
 dx=40;dy=20;
 assert(signbit(noble_camera_adjust(0,-0.0))); assert(takes==0);
 config.mouse_camera=1;
 /* Guest subtracts pitch operand; mouse down increases stored pitch. */
 near_value(noble_camera_adjust(0,.2),.1); assert(takes==1);
 near_value(noble_camera_adjust(1,.3),.4); assert(takes==1);
 near_value(noble_camera_adjust(1,.3),.3); /* cached yaw is one shot */
 near_value(noble_camera_adjust(0,.2),.2); near_value(noble_camera_adjust(1,.3),.3);
 config.mouse_invert_x=config.mouse_invert_y=1; dx=40;dy=20;
 near_value(noble_camera_adjust(0,.2),.3); near_value(noble_camera_adjust(1,.3),.2);
 config.mouse_invert_x=config.mouse_invert_y=0; dx=NAN;dy=INFINITY;
 near_value(noble_camera_adjust(0,.2),.2); near_value(noble_camera_adjust(1,.3),.3);
 /* Count bounds match pure core; no dt factor or stick saturation. */
 dx=1e9f;dy=-1e9f;
 near_value(noble_camera_adjust(0,0),50); near_value(noble_camera_adjust(1,0),25);
 config.mouse_sensitivity_x=NAN; config.mouse_sensitivity_y=INFINITY; dx=4;dy=5;
 near_value(noble_camera_adjust(0,.2),.2); near_value(noble_camera_adjust(1,.3),.3);
 config.mouse_sensitivity_x=.0025f;config.mouse_sensitivity_y=.005f;
 dx=40;dy=20; (void)noble_camera_adjust(0,0);
 config.mouse_camera=0; near_value(noble_camera_adjust(1,.3),.3);
 config.mouse_camera=1; near_value(noble_camera_adjust(1,.3),.3);
 assert(isnan(noble_camera_adjust(2,NAN))); /* invalid axis preserves guest */
 camera_seconds=1.0f/30.0f; dx=40;dy=20;
 near_value(noble_camera_adjust(0,0),-.1); near_value(noble_camera_adjust(1,0),.1);
 camera_seconds=1.0f/120.0f; dx=40;dy=20;
 near_value(noble_camera_adjust(0,0),-.1); near_value(noble_camera_adjust(1,0),.1);
 /* Controller getter's filtered 2-degree operand becomes angular speed*dt.
  * At 30 and 120 updates the same second produces the same two radians. */
 config.mouse_camera=0; config.look_sensitivity_x=config.look_sensitivity_y=2;
 camera_seconds=1.0f/30.0f;
 near_value(noble_camera_adjust(0,.03490658477)*30,2);
 near_value(noble_camera_adjust(1,.03490658477)*30,2);
 camera_seconds=1.0f/120.0f;
 near_value(noble_camera_adjust(0,.03490658477)*120,2);
 near_value(noble_camera_adjust(1,.03490658477)*120,2);
 camera_seconds=10;
 near_value(noble_camera_adjust(0,.03490658477),.2);
 near_value(noble_camera_adjust(1,.03490658477),.2);
 camera_seconds=-1; config.mouse_camera=1; dx=100;dy=100;
 { unsigned previous=takes; assert(signbit(noble_camera_adjust(0,-0.0)));
   assert(noble_camera_adjust(1,.3)==.3); assert(takes==previous); }
 puts("modern camera tests passed"); return 0;
}
