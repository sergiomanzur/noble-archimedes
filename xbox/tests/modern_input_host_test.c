#include "controls/input_host.h"
#include "input/xinput_xbox.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int provider_calls;
static DWORD fake_read(DWORD p,XBOX_INPUT_STATE *s){(void)p;memset(s,0,sizeof *s);provider_calls++;return 73;}
static BOOL fake_connected(DWORD p){(void)p;provider_calls++;return TRUE;}
static DWORD fake_caps(DWORD p,DWORD f,XBOX_INPUT_CAPABILITIES *c){(void)p;(void)f;memset(c,0,sizeof *c);provider_calls++;return 74;}
static DWORD fake_rumble(DWORD p,const XBOX_VIBRATION *v){(void)p;(void)v;provider_calls++;return 75;}
static void provider_precedence_test(void){
 static const XBOX_INPUT_PROVIDER provider={fake_read,fake_connected,fake_caps,fake_rumble};
 XBOX_INPUT_STATE state;XBOX_INPUT_CAPABILITIES caps;XBOX_VIBRATION vib={0};
 xbox_InputSetProvider(&provider);CHECK(xbox_InputGetState(0,&state)==73);CHECK(xbox_InputIsConnected(0));
 CHECK(xbox_InputGetCapabilities(0,0,&caps)==74);CHECK(xbox_InputSetState(0,&vib)==75);CHECK(provider_calls==4);
 _putenv_s("RECOMP_PAD_SCRIPT","deterministic-test-script");
 xbox_InputSetProvider(&provider);
 xbox_InputGetState(0,&state);xbox_InputIsConnected(0);xbox_InputGetCapabilities(0,0,&caps);xbox_InputSetState(0,&vib);CHECK(provider_calls==4);
 _putenv_s("RECOMP_PAD_SCRIPT","");_putenv_s("RECOMP_PAD_LIVE","deterministic-test-live");
 xbox_InputSetProvider(&provider);
 xbox_InputGetState(0,&state);xbox_InputIsConnected(0);xbox_InputGetCapabilities(0,0,&caps);xbox_InputSetState(0,&vib);CHECK(provider_calls==4);
 _putenv_s("RECOMP_PAD_LIVE","");_putenv_s("RECOMP_MODERN_INPUT","0");
 xbox_InputSetProvider(&provider);
 xbox_InputGetState(0,&state);CHECK(provider_calls==4);_putenv_s("RECOMP_MODERN_INPUT","");xbox_InputSetProvider(NULL);
}
static int toggle_provider(void *unused){
 static const XBOX_INPUT_PROVIDER provider={fake_read,fake_connected,fake_caps,fake_rumble};int i;(void)unused;
 for(i=0;i<100000;i++){xbox_InputSetProvider(&provider);xbox_InputSetProvider(NULL);}return 0;
}
static void provider_concurrency_test(void){SDL_Thread *thread=SDL_CreateThread(toggle_provider,"provider-toggle",NULL);XBOX_INPUT_STATE state;int i;CHECK(thread);for(i=0;i<20000;i++)xbox_InputGetState(0,&state);SDL_WaitThread(thread,NULL);xbox_InputSetProvider(NULL);}
static void analog_ownership_test(SDL_Joystick *stick){
 NiConfig saved,c;XBOX_INPUT_STATE state;SDL_Event mouse;NiRawState before,after;
 noble_input_get_config(&saved);c=saved;
 c.bindings[NI_GAMEPLAY][NI_KEYBOARD][NI_MOVE_RIGHT][1]=(NiBinding){NI_MOUSE_BUTTON,1,1};
 c.bindings[NI_GAMEPLAY][NI_KEYBOARD][NI_LOOK_RIGHT][1]=(NiBinding){NI_MOUSE_WHEEL,0,1};
 CHECK(noble_input_apply_config(&c));
 noble_input_key(SDL_SCANCODE_W,1);noble_input_pump();noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLY>30000);
 CHECK(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTX,24000));noble_input_pump();noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLX>20000&&state.Gamepad.sThumbLY==0);
 noble_input_key(SDL_SCANCODE_Z,1);noble_input_pump();noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLX>20000&&state.Gamepad.sThumbLY==0); /* Attack must not steal held movement. */
 noble_input_preview(0,&before,NULL);memset(&mouse,0,sizeof mouse);mouse.type=SDL_EVENT_MOUSE_BUTTON_DOWN;mouse.button.button=1;mouse.button.down=true;noble_input_event(&mouse);
 noble_input_preview(0,&after,NULL);CHECK(after.keyboard_activity[0]>before.keyboard_activity[0]);CHECK(after.keyboard_activity[1]==before.keyboard_activity[1]);
 noble_input_pump();noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLY>10000); /* Mouse movement binding takes movement ownership. */
 noble_input_preview(0,&before,NULL);mouse.type=SDL_EVENT_MOUSE_WHEEL;mouse.wheel.y=1;noble_input_event(&mouse);
 noble_input_preview(0,&after,NULL);CHECK(after.keyboard_activity[0]==before.keyboard_activity[0]);CHECK(after.keyboard_activity[1]>before.keyboard_activity[1]);
 CHECK(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTX,0));CHECK(noble_input_apply_config(&saved));noble_input_pump();
}
static float guest_filtered_radius(int16_t x,int16_t y){float radius=hypotf(x/256.0f,y/256.0f)/79.0f-16.0f/79.0f;return radius<0?0:radius>1?1:radius;}
static void audited_guest_adapter(NiOutput *output){CHECK(noble_input_inverse_guest_axes(output,1.0f/79.0f,-16.0f/79.0f));}
static void guest_axis_adapter_test(void){
 static const int16_t axes[][2]={{0,0},{16384,0},{0,-16384},{32767,0},{0,-32768},{9830,13107},{23170,-23170},{32767,32767}};
 NiOutput output,before;unsigned i;XBOX_INPUT_STATE state;NiOutput preview;
 for(i=0;i<sizeof axes/sizeof axes[0];i++){
  float desired=fminf(hypotf(axes[i][0]/32767.0f,axes[i][1]/32767.0f),1.0f);
  memset(&output,0,sizeof output);output.lx=axes[i][0];output.ly=axes[i][1];output.rx=axes[i][1];output.ry=axes[i][0];output.buttons=0x55;output.analog[2]=123;output.camera_yaw=.25f;
  CHECK(noble_input_inverse_guest_axes(&output,1.0f/79.0f,-16.0f/79.0f));
  CHECK(fabsf(guest_filtered_radius(output.lx,output.ly)-desired)<.0001f);
  CHECK(fabsf(guest_filtered_radius(output.rx,output.ry)-desired)<.0001f);
  CHECK(output.buttons==0x55&&output.analog[2]==123&&output.camera_yaw==.25f);
  if(!desired)CHECK(output.lx==0&&output.ly==0&&output.rx==0&&output.ry==0);
  else CHECK(fabs((double)output.lx*axes[i][1]-(double)output.ly*axes[i][0])<40000); /* Direction preserved within one int16 count. */
 }
 {NiConfig c;NiRawState input;float desired;ni_config_defaults(&c);memset(&input,0,sizeof input);input.focused=1;
  input.axes[0]=c.left_inner+(c.left_outer-c.left_inner)*.5f;ni_evaluate(&c,&input,NI_GAMEPLAY,0,&output);desired=output.lx/32767.0f;
  CHECK(fabsf(desired-.5f)<.0001f);CHECK(noble_input_inverse_guest_axes(&output,1.0f/79.0f,-16.0f/79.0f));
  CHECK(fabsf(guest_filtered_radius(output.lx,output.ly)-desired)<.0001f); /* Host deadzone is applied exactly once. */
 }
 memset(&before,0,sizeof before);before.lx=12345;before.ly=-6789;before.rx=-32768;before.ry=32767;output=before;
 CHECK(!noble_input_inverse_guest_axes(&output,NAN,-.2f));CHECK(!memcmp(&before,&output,sizeof output));
 CHECK(!noble_input_inverse_guest_axes(&output,.01f,INFINITY));CHECK(!noble_input_inverse_guest_axes(&output,0,-.2f));
 CHECK(!noble_input_inverse_guest_axes(&output,-.01f,-.2f));CHECK(!noble_input_inverse_guest_axes(&output,2,-.2f));CHECK(!noble_input_inverse_guest_axes(&output,.01f,-2));CHECK(!noble_input_inverse_guest_axes(&output,.01f,.2f));CHECK(!memcmp(&before,&output,sizeof output));
 noble_input_key(SDL_SCANCODE_W,1);noble_input_pump();noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLY==32767); /* No adapter means unchanged. */
 noble_input_set_guest_axis_adapter(audited_guest_adapter);noble_input_read(0,&state);noble_input_preview(0,NULL,&preview);
 CHECK(state.Gamepad.sThumbLY<27000&&preview.ly==32767);CHECK(fabsf(guest_filtered_radius(state.Gamepad.sThumbLX,state.Gamepad.sThumbLY)-1)<.0001f);
 noble_input_set_guest_axis_adapter(NULL);noble_input_read(0,&state);CHECK(state.Gamepad.sThumbLY==32767);noble_input_key(SDL_SCANCODE_W,0);noble_input_pump();
}
static void camera_rearm_test(void){float seconds;noble_input_set_focus(0);noble_input_set_focus(1);seconds=noble_input_camera_seconds();CHECK(fabsf(seconds-1.0f/60.0f)<.00001f);SDL_Delay(120);seconds=noble_input_camera_seconds();CHECK(fabsf(seconds-1.0f/60.0f)<.00001f);}
int main(void) {
 SDL_VirtualJoystickDesc desc; SDL_JoystickID ids[4]; SDL_Joystick *sticks[4];
 XBOX_INPUT_STATE a,b; SDL_Event focus; int i;
 _putenv_s("APPDATA",""); /* Isolate this deterministic test from user preferences. */
 CHECK(noble_input_init());
 CHECK(noble_input_read(0,&a)==0); CHECK(noble_input_read(0,&b)==0);
 CHECK(a.dwPacketNumber==b.dwPacketNumber);
 guest_axis_adapter_test();camera_rearm_test();
 memset(&desc,0,sizeof desc); desc.version=sizeof(desc);
 desc.type=SDL_JOYSTICK_TYPE_GAMEPAD; desc.naxes=6; desc.nbuttons=26;
 desc.axis_mask=0x3f; desc.button_mask=0x3ffffff; desc.name="Noble virtual gamepad";
 for(i=0;i<4;i++){ids[i]=SDL_AttachVirtualJoystick(&desc); CHECK(ids[i]); sticks[i]=SDL_OpenJoystick(ids[i]); CHECK(sticks[i]);}
 noble_input_pump();
 for(i=0;i<4;i++)CHECK(noble_input_connected(i));
 analog_ownership_test(sticks[0]);
 {XBOX_INPUT_CAPABILITIES caps;CHECK(noble_input_capabilities(0,0,&caps)==0);CHECK(caps.Vibration.wLeftMotorSpeed==0&&caps.Vibration.wRightMotorSpeed==0);}
 CHECK(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_SOUTH,true));
 noble_input_pump(); CHECK(noble_input_read(0,&a)==0); CHECK(a.Gamepad.bAnalogButtons[0]==255);
 noble_input_pump(); CHECK(noble_input_read(0,&b)==0); CHECK(a.dwPacketNumber==b.dwPacketNumber);
 memset(&focus,0,sizeof focus);focus.type=SDL_EVENT_WINDOW_FOCUS_LOST;noble_input_event(&focus);
 CHECK(noble_input_read(0,&b)==0);CHECK(b.Gamepad.bAnalogButtons[0]==0); CHECK(b.dwPacketNumber!=a.dwPacketNumber);
 noble_input_set_focus(1); noble_input_pump(); noble_input_read(0,&a); CHECK(a.Gamepad.bAnalogButtons[0]==255);
 noble_input_set_panel_active(1);noble_input_read(0,&b);CHECK(b.Gamepad.bAnalogButtons[0]==0);
 {NiRawState preview;CHECK(!strcmp(SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS),"1"));
  noble_input_set_focus(0);CHECK(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_EAST,true));noble_input_pump();
  noble_input_preview(0,&preview,NULL);CHECK(preview.pad_buttons&(1u<<SDL_GAMEPAD_BUTTON_EAST));
  noble_input_read(0,&b);CHECK(b.Gamepad.bAnalogButtons[0]==0&&b.Gamepad.bAnalogButtons[1]==0);
  CHECK(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_EAST,false));noble_input_set_focus(1);}
 noble_input_set_panel_active(0);
 CHECK(SDL_DetachVirtualJoystick(ids[0]));noble_input_pump();noble_input_read(0,&a);CHECK(a.Gamepad.bAnalogButtons[0]==0);
 {SDL_JoystickID extra=SDL_AttachVirtualJoystick(&desc);NobleInputDevice list[8];int n,j,port=-2;
  CHECK(extra);noble_input_pump();n=noble_input_devices(list,8);for(j=0;j<n;j++)if(list[j].id==extra)port=list[j].port;
  CHECK(port==-1); /* A newly connected identical device must not silently steal reserved port. */
  CHECK(noble_input_assign(extra,0));CHECK(SDL_DetachVirtualJoystick(extra));noble_input_pump();}
 CHECK(!noble_input_connected(4));
 {float dx,dy;SDL_Event motion;memset(&motion,0,sizeof motion);motion.type=SDL_EVENT_MOUSE_MOTION;
  noble_input_take_mouse(&dx,&dy);CHECK(dx==0&&dy==0); /* First eligible update arms relative input. */
  motion.motion.xrel=1;noble_input_event(&motion); /* First motion after capture is deliberately discarded. */
  motion.motion.xrel=5;motion.motion.yrel=-3;noble_input_event(&motion);noble_input_pump();
  noble_input_take_mouse(&dx,&dy);CHECK(dx==5&&dy==-3);noble_input_take_mouse(&dx,&dy);CHECK(dx==0&&dy==0);
  motion.motion.xrel=50;noble_input_event(&motion);noble_input_pump();SDL_Delay(120);
  noble_input_take_mouse(&dx,&dy);CHECK(dx==0&&dy==0); /* No saved motion leaks across a menu/stall gap. */
 }
 for(i=0;i<4;i++){SDL_CloseJoystick(sticks[i]);if(i)CHECK(SDL_DetachVirtualJoystick(ids[i]));}
 noble_input_shutdown();CHECK(!noble_input_connected(0));
 _putenv_s("RECOMP_PAD_SCRIPT","startup-script");CHECK(!noble_input_init());_putenv_s("RECOMP_PAD_SCRIPT","");
 _putenv_s("RECOMP_PAD_LIVE","startup-live");CHECK(!noble_input_init());_putenv_s("RECOMP_PAD_LIVE","");
 _putenv_s("RECOMP_MODERN_INPUT","0");CHECK(!noble_input_init());_putenv_s("RECOMP_MODERN_INPUT","");
 CHECK(noble_input_camera_seconds()<0);provider_precedence_test();provider_concurrency_test();
 puts("modern_input_host_test: PASS");return 0;
}
