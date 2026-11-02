#include "camera_input.h"
#include "input_host.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#if defined(_MSC_VER)
#define CAMERA_TLS __declspec(thread)
#else
#define CAMERA_TLS _Thread_local
#endif
/* Only the guest camera thread owns the pending pair and counters. */
static CAMERA_TLS double pending_yaw;
static CAMERA_TLS int yaw_ready;
static CAMERA_TLS double yaw_scale=1;
static CAMERA_TLS int input_active;
static CAMERA_TLS uint64_t pitch_calls,yaw_calls,moved_pairs;
static int diagnostics;
static double angular(float counts,float sensitivity,int inverted) {
 double c=counts;
 if(!isfinite(c)||!isfinite(sensitivity)||sensitivity<0||sensitivity>10) return 0;
 if(c>10000) c=10000;
 if(c< -10000) c= -10000;
 return c*(double)sensitivity*(inverted?-1:1);
}
static double stick_scale(float speed,float seconds) {
 if(!isfinite(speed)||speed<0||speed>100) return 1;
 if(!isfinite(seconds)||seconds<0) return 1;
 if(seconds>.1f) seconds=.1f;
 /* Audited 0x432C08: original full filtered stick advances 2 degrees/update.
  * Preserve that guest filter and replace its fixed update rate with rad/s. */
 return (double)speed*(double)seconds/.03490658477;
}
double noble_camera_adjust(unsigned axis,double original_delta) {
 NiConfig config;
 double addition=0,scale=1;
 if(axis>1) return original_delta;
 if(axis==0) {
  float seconds=noble_input_camera_seconds();
  input_active=isfinite(seconds)&&seconds>=0;
  if(!input_active) { pending_yaw=0; yaw_ready=0; return original_delta; }
  noble_input_get_config(&config);
  scale=stick_scale(config.look_sensitivity_y,seconds);
  yaw_scale=stick_scale(config.look_sensitivity_x,seconds);
  if(diagnostics) ++pitch_calls;
 } else {
  if(!input_active) return original_delta;
  noble_input_get_config(&config);
  scale=yaw_scale;
  if(diagnostics) ++yaw_calls;
 }
 if(!config.mouse_camera) { pending_yaw=0; yaw_ready=0; }
 else if(axis==0) {
  float dx=0,dy=0;
  noble_input_take_mouse(&dx,&dy);
  pending_yaw=angular(dx,config.mouse_sensitivity_x,config.mouse_invert_x);
  yaw_ready=1;
  /* Guest fsubr negates this operand. Downward mouse motion therefore
   * increases the stored polar pitch, before the original 30..100 clamp. */
  addition= -angular(dy,config.mouse_sensitivity_y,config.mouse_invert_y);
  if(diagnostics&&(addition!=0||pending_yaw!=0)) ++moved_pairs;
 } else {
  addition=yaw_ready?pending_yaw:0;
  pending_yaw=0; yaw_ready=0;
 }
 if(scale!=1&&original_delta!=0) original_delta*=scale;
 /* Avoid even adding +0 to preserve signed zero and NaN payloads on idle. */
 return addition!=0?original_delta+addition:original_delta;
}
void noble_camera_set_diagnostics(int enabled) { diagnostics=enabled!=0; }
void noble_camera_report(void) {
 if(diagnostics) fprintf(stderr,"[CAMERA] pitch-hooks=%llu yaw-hooks=%llu mouse-pairs=%llu\n",
  (unsigned long long)pitch_calls,(unsigned long long)yaw_calls,(unsigned long long)moved_pairs);
}
