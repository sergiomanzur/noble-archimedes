#define _CRT_SECURE_NO_WARNINGS
/* SDL owns devices on the main thread. USB/API workers only copy snapshots. */
#include "input_host.h"
#include "input/xinput_xbox.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define NI_HOST_DEVICES 32
typedef struct {
 SDL_JoystickID id; SDL_Gamepad *pad; SDL_Joystick *joy;
 int port, supports_rumble; char name[128], guid[40], serial[128];
} HostDevice;
static HostDevice devices[NI_HOST_DEVICES];
typedef struct { int reserved; char guid[40],serial[128]; } PortPreference;
static PortPreference preferences[4];
static SDL_SpinLock snapshot_lock;
static XBOX_INPUT_STATE snapshots[4];
static XBOX_INPUT_CAPABILITIES published_caps[4];
static int connections[4], rumble_pending[4];
static XBOX_VIBRATION rumble_requests[4];
static int accepting_rumble;
static NiRawState raw[4]; static NiOutput outputs[4];
static NiConfig config; static NiContext context;
static int initialized, focused=1, panel_active, binding_ready, discard_motion=1;
static NiBinding last_binding; static NiDevice binding_device;
static uint64_t activity, family_tick, camera_last_take, camera_update_ns;
static int camera_armed, native_source;
static SDL_WindowID captured_window;
static NiFamily family=NI_FAMILY_KEYBOARD;
static char config_path[1024];
static char preferences_path[1024], mappings_path[1024];
static float camera_dx, camera_dy;
static void (*open_callback)(void), (*ui_callback)(void);
static void (*dispatch_callback)(const void *);
static void (*guest_axis_adapter)(NiOutput *);
static void text_copy(char *dst,size_t size,const char *src) { SDL_strlcpy(dst,src?src:"",size); }
static void lock(void) { SDL_LockSpinlock(&snapshot_lock); }
static void unlock(void) { SDL_UnlockSpinlock(&snapshot_lock); }
static int script_active(void) {
 const char *a=getenv("RECOMP_PAD_SCRIPT"),*b=getenv("RECOMP_PAD_LIVE");
 return (a&&*a)||(b&&*b);
}
static HostDevice *find(SDL_JoystickID id) { int i;for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].id==id)return &devices[i];return NULL; }
static void save_preferences(void) {
 FILE *f;char temp[1050];int p;if(!preferences_path[0])return;
 snprintf(temp,sizeof temp,"%s.tmp",preferences_path);f=fopen(temp,"wb");if(!f)return;
 for(p=0;p<4;p++)if(preferences[p].reserved)fprintf(f,"%d %s %s\n",p,preferences[p].guid,preferences[p].serial[0]?preferences[p].serial:"-");
 if(fclose(f)){remove(temp);return;}
#ifdef _WIN32
 if(!MoveFileExA(temp,preferences_path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))remove(temp);
#else
 if(rename(temp,preferences_path))remove(temp);
#endif
}
static void reserve_port(const HostDevice *d,int port) {
 preferences[port].reserved=1;text_copy(preferences[port].guid,sizeof preferences[port].guid,d->guid);
 text_copy(preferences[port].serial,sizeof preferences[port].serial,d->serial);
 save_preferences();
}
static NiFamily device_family(SDL_Gamepad *pad) {
 SDL_GamepadType t=pad?SDL_GetGamepadType(pad):SDL_GAMEPAD_TYPE_UNKNOWN;
 switch(t){
 case SDL_GAMEPAD_TYPE_PS3:case SDL_GAMEPAD_TYPE_PS4:case SDL_GAMEPAD_TYPE_PS5:return NI_FAMILY_PLAYSTATION;
 case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
 case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:return NI_FAMILY_NINTENDO;
 case SDL_GAMEPAD_TYPE_XBOX360:case SDL_GAMEPAD_TYPE_XBOXONE:return NI_FAMILY_XBOX;
 default:return NI_FAMILY_GENERIC;
 }
}
static void intentional(NiFamily next) {
 uint64_t now=SDL_GetTicks();
 if(next==family||now-family_tick>=250){family=next;family_tick=now;}
}
static void capture(NiBindingKind kind,int code,int sign,NiDevice device) {
 last_binding.kind=kind;last_binding.code=code;last_binding.sign=sign;
 binding_device=device;binding_ready=1;
}
/* A button used for an unrelated action must not change analog ownership. */
static void keyboard_analog_activity(NiBindingKind kind,int code,int sign) {
 int action,slot;int groups[2]={0,0};
 for(action=NI_MOVE_UP;action<NI_ACTION_COUNT;action++)for(slot=0;slot<NI_BINDING_SLOTS;slot++){
  NiBinding b=config.bindings[context][NI_KEYBOARD][action][slot];
  if(b.kind==kind&&b.code==code&&(kind!=NI_MOUSE_WHEEL||b.sign==sign))groups[action>=NI_LOOK_UP]=1;
 }
 if(groups[0])raw[0].keyboard_activity[0]=++activity;
 if(groups[1])raw[0].keyboard_activity[1]=++activity;
}
static void stop_rumble(void) {
 int i;lock();memset(rumble_requests,0,sizeof rumble_requests);memset(rumble_pending,0,sizeof rumble_pending);unlock();
 for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].joy)SDL_RumbleJoystick(devices[i].joy,0,0,0);
}
static void publish(void) {
 int p,i; XBOX_INPUT_STATE next[4];XBOX_INPUT_CAPABILITIES caps[4]; int connected[4]={1,0,0,0};
 int adapt=guest_axis_adapter!=NULL;
 memset(next,0,sizeof next);memset(caps,0,sizeof caps);
 for(p=0;p<4;p++){
  caps[p].Type=1;caps[p].SubType=1;caps[p].Gamepad.wButtons=255;memset(caps[p].Gamepad.bAnalogButtons,255,8);
  caps[p].Gamepad.sThumbLX=caps[p].Gamepad.sThumbLY=caps[p].Gamepad.sThumbRX=caps[p].Gamepad.sThumbRY=32767;
 }
 for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].pad&&devices[i].port>=0){
  p=devices[i].port;connected[p]=1;
  if(devices[i].supports_rumble){caps[p].Flags=1;caps[p].Vibration.wLeftMotorSpeed=caps[p].Vibration.wRightMotorSpeed=65535;}
 }
 for(p=0;p<4;p++){
  NiRawState eval=raw[p];eval.focused=focused&&!panel_active;
  ni_evaluate(&config,&eval,context,0.0f,&outputs[p]);
  /* Preserve normalized UI previews; only the provider's guest sticks adapt. */
  NiOutput guest=outputs[p];
  if(adapt&&eval.focused)guest_axis_adapter(&guest);
  next[p].Gamepad.wButtons=outputs[p].buttons;
  memcpy(next[p].Gamepad.bAnalogButtons,outputs[p].analog,8);
  next[p].Gamepad.sThumbLX=guest.lx;next[p].Gamepad.sThumbLY=guest.ly;
  next[p].Gamepad.sThumbRX=guest.rx;next[p].Gamepad.sThumbRY=guest.ry;
 }
 lock();accepting_rumble=focused&&!panel_active;
 for(p=0;p<4;p++){
  DWORD packet=snapshots[p].dwPacketNumber;
  if(memcmp(&next[p].Gamepad,&snapshots[p].Gamepad,sizeof next[p].Gamepad)||connections[p]!=connected[p])packet++;
  snapshots[p]=next[p];snapshots[p].dwPacketNumber=packet;connections[p]=connected[p];published_caps[p]=caps[p];
 }
 unlock();
}
static void clear_transient(void) {
 int p;for(p=0;p<4;p++){memset(raw[p].keys,0,sizeof raw[p].keys);raw[p].mouse_buttons=0;raw[p].mouse_wheel=0;raw[p].mouse_dx=raw[p].mouse_dy=0;}
 camera_dx=camera_dy=0;camera_armed=0;camera_update_ns=0;discard_motion=1;binding_ready=0;
}
static void release_capture(void) {
 SDL_Window *window=captured_window?SDL_GetWindowFromID(captured_window):SDL_GetKeyboardFocus();
 if(window)SDL_SetWindowRelativeMouseMode(window,false);
 captured_window=0;
}
static void add_device(SDL_JoystickID id) {
 int i,p;HostDevice *d;if(find(id))return;
 for(i=0;i<NI_HOST_DEVICES;i++)if(!devices[i].id)break;
 if(i==NI_HOST_DEVICES)return;
 d=&devices[i];memset(d,0,sizeof *d);d->port=-1;
 d->pad=SDL_IsGamepad(id)?SDL_OpenGamepad(id):NULL;
 d->joy=d->pad?SDL_GetGamepadJoystick(d->pad):SDL_OpenJoystick(id);
 if(!d->joy)return;
 d->id=id;
 d->supports_rumble=SDL_GetBooleanProperty(SDL_GetJoystickProperties(d->joy),SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN,false);
 text_copy(d->name,sizeof d->name,SDL_GetJoystickName(d->joy));
 SDL_GUIDToString(SDL_GetJoystickGUID(d->joy),d->guid,sizeof d->guid);
 text_copy(d->serial,sizeof d->serial,SDL_GetJoystickSerial(d->joy));
 if(d->pad)for(p=0;p<4;p++){
  int occupied=0;for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].id&&devices[i].port==p)occupied=1;
  /* A GUID identifies a model, not an individual controller. Without serial
   * identity an occupied/reserved port needs deliberate panel reassignment. */
  if(!occupied&&(!preferences[p].reserved||(d->serial[0]&&!strcmp(d->guid,preferences[p].guid)&&!strcmp(d->serial,preferences[p].serial)))){d->port=p;reserve_port(d,p);break;}
 }
}
static void remove_device(SDL_JoystickID id) {
 HostDevice *d=find(id);if(!d)return;
 SDL_RumbleJoystick(d->joy,0,0,0);
 if(d->port>=0){lock();rumble_pending[d->port]=0;unlock();memset(&raw[d->port],0,sizeof raw[d->port]);}
 if(d->pad)SDL_CloseGamepad(d->pad);else SDL_CloseJoystick(d->joy);
 memset(d,0,sizeof *d);d->port=-1;publish();
}
void noble_input_set_focus(int on) {
 focused=!!on;if(!focused){clear_transient();stop_rumble();
  release_capture();
 }else discard_motion=1;
 if(initialized)publish();
}
void noble_input_set_panel_active(int active) {
 panel_active=!!active;clear_transient();if(active){stop_rumble();release_capture();}
 if(initialized)publish();
}
void noble_input_key(unsigned code,int down) {
 if(code>=512)return;
 if(down&&!raw[0].keys[code]){
  if(code==SDL_SCANCODE_F2&&initialized)noble_input_open_controls();
  else {capture(NI_KEY,(int)code,1,NI_KEYBOARD);intentional(NI_FAMILY_KEYBOARD);keyboard_analog_activity(NI_KEY,(int)code,1);}
 }
 raw[0].keys[code]=(uint8_t)!!down;
}
void noble_input_event(const void *event) {
 const SDL_Event *e=event;HostDevice *d;if(!initialized||!e)return;
 switch(e->type){
 case SDL_EVENT_JOYSTICK_ADDED:add_device(e->jdevice.which);break;
 case SDL_EVENT_GAMEPAD_ADDED: /* mapping can become available after joystick-added */
  d=find(e->gdevice.which);if(d&&!d->pad){remove_device(d->id);}add_device(e->gdevice.which);break;
 case SDL_EVENT_JOYSTICK_REMOVED:case SDL_EVENT_GAMEPAD_REMOVED:remove_device(e->jdevice.which);break;
 case SDL_EVENT_WINDOW_FOCUS_LOST:noble_input_set_focus(0);break;
 case SDL_EVENT_WINDOW_FOCUS_GAINED:noble_input_set_focus(1);break;
 case SDL_EVENT_KEY_DOWN:case SDL_EVENT_KEY_UP:noble_input_key((unsigned)e->key.scancode,e->key.down);break;
 case SDL_EVENT_MOUSE_BUTTON_DOWN:case SDL_EVENT_MOUSE_BUTTON_UP:
  if(e->button.button>0&&e->button.button<=32){uint32_t mask=1u<<(e->button.button-1);
   if(e->button.down){if(!(raw[0].mouse_buttons&mask))keyboard_analog_activity(NI_MOUSE_BUTTON,e->button.button,1);raw[0].mouse_buttons|=mask;capture(NI_MOUSE_BUTTON,e->button.button,1,NI_KEYBOARD);intentional(NI_FAMILY_KEYBOARD);}else raw[0].mouse_buttons&=~mask;}break;
 case SDL_EVENT_MOUSE_WHEEL:
  raw[0].mouse_wheel+=(int)e->wheel.y;if(e->wheel.y){capture(NI_MOUSE_WHEEL,0,e->wheel.y>0?1:-1,NI_KEYBOARD);keyboard_analog_activity(NI_MOUSE_WHEEL,0,e->wheel.y>0?1:-1);}break;
 case SDL_EVENT_MOUSE_MOTION:
  if(discard_motion){discard_motion=0;break;}
  if(focused&&!panel_active){raw[0].mouse_dx+=e->motion.xrel;raw[0].mouse_dy+=e->motion.yrel;
   if(fabsf(e->motion.xrel)+fabsf(e->motion.yrel)>0){raw[0].keyboard_activity[1]=++activity;intentional(NI_FAMILY_KEYBOARD);}}break;
 case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  capture(NI_PAD_BUTTON,e->gbutton.button,1,NI_GAMEPAD);d=find(e->gbutton.which);intentional(device_family(d?d->pad:NULL));break;
 case SDL_EVENT_GAMEPAD_AXIS_MOTION:
  if(abs(e->gaxis.value)>18000)capture(NI_PAD_AXIS,e->gaxis.axis,e->gaxis.value>0?1:-1,NI_GAMEPAD);
  break;
 default:break;
 }
 if(dispatch_callback)dispatch_callback(event);
}
static void poll_keyboard_fallback(void) {
#ifdef _WIN32
 if(native_source)return; /* Raw Win32 bridge owns full keyboard and focus. */
 /* GDI presentation owns no SDL window. Poll only while our process has focus. */
 if(!SDL_GetKeyboardFocus()){
  static HWND owned_window;
  HWND window=GetForegroundWindow();DWORD pid=0;int s;
  if(GetActiveWindow())owned_window=GetActiveWindow();
  if(!owned_window||!IsWindow(owned_window))return;
  GetWindowThreadProcessId(window,&pid);
  if(pid!=GetCurrentProcessId()){if(focused)noble_input_set_focus(0);return;}
  if(!focused)noble_input_set_focus(1);
  for(s=4;s<232;s++){
   SDL_Keycode key=SDL_GetKeyFromScancode((SDL_Scancode)s,SDL_KMOD_NONE,false);
   int vk=0;if(key>='a'&&key<='z')vk='A'+(int)(key-'a');
   else if(key>='0'&&key<='9')vk=(int)key;
   else switch(s){case SDL_SCANCODE_RETURN:vk=VK_RETURN;break;case SDL_SCANCODE_ESCAPE:vk=VK_ESCAPE;break;case SDL_SCANCODE_SPACE:vk=VK_SPACE;break;case SDL_SCANCODE_TAB:vk=VK_TAB;break;
    case SDL_SCANCODE_BACKSPACE:vk=VK_BACK;break;case SDL_SCANCODE_UP:vk=VK_UP;break;case SDL_SCANCODE_DOWN:vk=VK_DOWN;break;case SDL_SCANCODE_LEFT:vk=VK_LEFT;break;case SDL_SCANCODE_RIGHT:vk=VK_RIGHT;break;
    case SDL_SCANCODE_F2:vk=VK_F2;break;case SDL_SCANCODE_LSHIFT:vk=VK_LSHIFT;break;case SDL_SCANCODE_RSHIFT:vk=VK_RSHIFT;break;case SDL_SCANCODE_LCTRL:vk=VK_LCONTROL;break;case SDL_SCANCODE_RCTRL:vk=VK_RCONTROL;break;
    case SDL_SCANCODE_LALT:vk=VK_LMENU;break;case SDL_SCANCODE_RALT:vk=VK_RMENU;break;default:break;}
   if(vk)noble_input_key((unsigned)s,(GetAsyncKeyState(vk)&0x8000)!=0);
  }
 }
#endif
}
void noble_input_pump(void) {
 SDL_Event e;int i,p,a;NiRawState previous[4];if(!initialized)return;
 while(SDL_PollEvent(&e))noble_input_event(&e);
 if(ui_callback)ui_callback();
 poll_keyboard_fallback();
 SDL_UpdateGamepads();memcpy(previous,raw,sizeof previous);
 for(p=0;p<4;p++){raw[p].pad_buttons=0;memset(raw[p].axes,0,sizeof raw[p].axes);raw[p].focused=focused;}
 for(i=0;i<NI_HOST_DEVICES;i++){
  HostDevice *d=&devices[i];if(!d->pad||d->port<0)continue;p=d->port;
  for(a=0;a<SDL_GAMEPAD_BUTTON_COUNT&&a<32;a++)if(SDL_GetGamepadButton(d->pad,(SDL_GamepadButton)a))raw[p].pad_buttons|=1u<<a;
  for(a=0;a<6;a++){
   Sint16 v=SDL_GetGamepadAxis(d->pad,(SDL_GamepadAxis)a);raw[p].axes[a]=v<0?v/32768.0f:v/32767.0f;
   if(fabsf(raw[p].axes[a])>0.3f&&fabsf(raw[p].axes[a]-previous[p].axes[a])>0.03f){if(a<4)raw[p].pad_activity[a/2]=++activity;intentional(device_family(d->pad));}
  }
  if(raw[p].pad_buttons&~previous[p].pad_buttons)intentional(device_family(d->pad));
 }
 {SDL_Window *window=SDL_GetKeyboardFocus();int eligible=noble_input_mouse_capture_active();
  if(eligible){camera_dx+=raw[0].mouse_dx;camera_dy+=raw[0].mouse_dy;}
  else {camera_dx=camera_dy=0;discard_motion=1;}
  if(window&&SDL_GetWindowRelativeMouseMode(window)!=(bool)eligible)SDL_SetWindowRelativeMouseMode(window,(bool)eligible);
  if(window&&eligible)captured_window=SDL_GetWindowID(window);
  if(!eligible&&captured_window)release_capture();
 }
 publish();
 for(p=0;p<4;p++){raw[p].mouse_dx=raw[p].mouse_dy=0;raw[p].mouse_wheel=0;}
 for(p=0;p<4;p++){
  XBOX_VIBRATION v;int pending;lock();pending=rumble_pending[p];v=rumble_requests[p];rumble_pending[p]=0;unlock();
  if(pending&&focused&&!panel_active)for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].joy&&devices[i].port==p)SDL_RumbleJoystick(devices[i].joy,v.wLeftMotorSpeed,v.wRightMotorSpeed,1000);
 }
}
unsigned long noble_input_read(unsigned long p,void *state) {
 if(p>=4||!state)return ERROR_DEVICE_NOT_CONNECTED;
 lock();memcpy(state,&snapshots[p],sizeof snapshots[p]);{int c=connections[p];unlock();return c?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;}
}
int noble_input_connected(unsigned long p) {int c;if(p>=4)return 0;lock();c=connections[p];unlock();return c;}
unsigned long noble_input_capabilities(unsigned long p,unsigned long flags,void *caps) {
 int connected;(void)flags;if(!caps||p>=4)return ERROR_DEVICE_NOT_CONNECTED;
 lock();connected=connections[p];memcpy(caps,&published_caps[p],sizeof published_caps[p]);unlock();
 return connected?ERROR_SUCCESS:ERROR_DEVICE_NOT_CONNECTED;
}
unsigned long noble_input_rumble(unsigned long p,const void *vibration) {
 if(!vibration||p>=4)return ERROR_DEVICE_NOT_CONNECTED;
 lock();if(!connections[p]){unlock();return ERROR_DEVICE_NOT_CONNECTED;}
 if(!accepting_rumble){unlock();return ERROR_SUCCESS;}
 rumble_requests[p]=*(const XBOX_VIBRATION*)vibration;rumble_pending[p]=1;unlock();return ERROR_SUCCESS;
}
static DWORD provider_read(DWORD p,XBOX_INPUT_STATE *s){return (DWORD)noble_input_read(p,s);}
static BOOL provider_connected(DWORD p){return noble_input_connected(p);}
static DWORD provider_caps(DWORD p,DWORD f,XBOX_INPUT_CAPABILITIES *c){return (DWORD)noble_input_capabilities(p,f,c);}
static DWORD provider_rumble(DWORD p,const XBOX_VIBRATION *v){return (DWORD)noble_input_rumble(p,v);}
int noble_input_init(void) {
 const char *enabled=getenv("RECOMP_MODERN_INPUT");int count,i;SDL_JoystickID *ids;
 static const XBOX_INPUT_PROVIDER provider={provider_read,provider_connected,provider_caps,provider_rumble};
 if(initialized)return 1;
 if((enabled&&!strcmp(enabled,"0"))||script_active())return 0;
 /* A foreground native Controls panel must continue receiving raw gamepad
  * events. Gameplay focus/panel gating remains explicit in publish(). */
 SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
 if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS))return 0;
 ni_config_pc_defaults(&config);context=NI_GAMEPLAY;focused=1;panel_active=0;family_tick=SDL_GetTicks()-250;
 config_path[0]=preferences_path[0]=mappings_path[0]=0;memset(preferences,0,sizeof preferences);
#ifdef _WIN32
 {const char *base=getenv("APPDATA");if(base&&*base){char dir[1024];FILE *f;snprintf(dir,sizeof dir,"%s/NobleArchimedes",base);CreateDirectoryA(dir,NULL);snprintf(config_path,sizeof config_path,"%s/controls.ini",dir);if(GetFileAttributesA(config_path)!=INVALID_FILE_ATTRIBUTES)ni_config_load(&config,config_path);
  snprintf(preferences_path,sizeof preferences_path,"%s/controllers.ini",dir);snprintf(mappings_path,sizeof mappings_path,"%s/gamecontrollerdb.txt",dir);
  f=fopen(preferences_path,"rb");if(f){int port;char guid[40],serial[128];while(fscanf(f,"%d %39s %127s",&port,guid,serial)==3)if(port>=0&&port<4&&strlen(guid)==32){preferences[port].reserved=1;text_copy(preferences[port].guid,40,guid);if(strcmp(serial,"-"))text_copy(preferences[port].serial,128,serial);}fclose(f);}
  if(GetFileAttributesA(mappings_path)!=INVALID_FILE_ATTRIBUTES)SDL_AddGamepadMappingsFromFile(mappings_path);
 }}
#endif
 initialized=1;ids=SDL_GetJoysticks(&count);for(i=0;ids&&i<count;i++)add_device(ids[i]);SDL_free(ids);
 publish();xbox_InputSetProvider(&provider);return 1;
}
void noble_input_shutdown(void) {
 int i;if(!initialized)return;stop_rumble();release_capture();
 for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].id){if(devices[i].pad)SDL_CloseGamepad(devices[i].pad);else SDL_CloseJoystick(devices[i].joy);}
 memset(devices,0,sizeof devices);clear_transient();lock();memset(connections,0,sizeof connections);memset(snapshots,0,sizeof snapshots);memset(published_caps,0,sizeof published_caps);unlock();
 initialized=0;xbox_InputSetProvider(NULL);SDL_QuitSubSystem(SDL_INIT_GAMEPAD|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS);
}
void noble_input_get_config(NiConfig *c){if(c)*c=config;}
int noble_input_apply_config(const NiConfig *c){if(!c||!ni_config_valid(c))return 0;if(config_path[0]&&!ni_config_save(c,config_path))return 0;config=*c;clear_transient();publish();return 1;}
void noble_input_preview(unsigned p,NiRawState *r,NiOutput *o){if(p>=4)return;if(r)*r=raw[p];if(o)*o=outputs[p];}
void noble_input_set_guest_axis_adapter(void (*adapter)(NiOutput *)){guest_axis_adapter=adapter;if(initialized)publish();}
static int16_t inverse_axis_round(double value){
 if(value>32767.0)return 32767;
 if(value< -32767.0)return -32767;
 return (int16_t)lround(value);
}
static void inverse_guest_pair(int16_t *x,int16_t *y,double scale,double bias){
 double nx=*x/32767.0,ny=*y/32767.0,radius=sqrt(nx*nx+ny*ny),desired,mapped;
 if(radius==0){*x=*y=0;return;}
 desired=radius>1?1:radius;mapped=(desired-bias)/scale;
 if(mapped<0)mapped=0;
 if(mapped>1)mapped=1;
 *x=inverse_axis_round(nx/radius*mapped*32767.0);
 *y=inverse_axis_round(ny/radius*mapped*32767.0);
}
int noble_input_inverse_guest_axes(NiOutput *output,float coefficient,float bias){
 double scale=(32767.0/256.0)*(double)coefficient;
 if(!output||!isfinite(coefficient)||!isfinite(bias)||coefficient<=0||coefficient>1||bias< -1||bias>0||scale+bias<=0)return 0;
 inverse_guest_pair(&output->lx,&output->ly,scale,bias);
 inverse_guest_pair(&output->rx,&output->ry,scale,bias);
 return 1;
}
void noble_input_set_context(NiContext c){if(c>=NI_GAMEPLAY&&c<NI_CONTEXT_COUNT&&context!=c){context=c;clear_transient();publish();}}
void noble_input_set_open_controls(void(*cb)(void)){open_callback=cb;}
void noble_input_set_dispatch(void(*cb)(const void*)){dispatch_callback=cb;}
void noble_input_set_ui_pump(void(*cb)(void)){ui_callback=cb;}
void noble_input_open_controls(void){if(initialized&&open_callback)open_callback();}
NiFamily noble_input_family(void){return config.prompt_family==NI_FAMILY_AUTO?family:config.prompt_family;}
int noble_input_take_binding(NiBinding *b,NiDevice *d){if(!binding_ready)return 0;if(b)*b=last_binding;if(d)*d=binding_device;binding_ready=0;return 1;}
int noble_input_devices(NobleInputDevice *out,int capacity){int i,n=0;for(i=0;i<NI_HOST_DEVICES;i++)if(devices[i].id){if(out&&n<capacity){out[n].id=devices[i].id;out[n].port=devices[i].port;out[n].mapped=devices[i].pad!=NULL;text_copy(out[n].name,sizeof out[n].name,devices[i].name);text_copy(out[n].guid,sizeof out[n].guid,devices[i].guid);text_copy(out[n].serial,sizeof out[n].serial,devices[i].serial);}n++;}return n;}
int noble_input_assign(unsigned id,int port){HostDevice *d=find(id);int i,old;if(!d||port < -1||port>3||(!d->pad&&port>=0))return 0;for(i=0;i<NI_HOST_DEVICES;i++)if(&devices[i]!=d&&devices[i].id&&port>=0&&devices[i].port==port)return 0;old=d->port;if(old>=0){memset(&raw[old],0,sizeof raw[old]);memset(&preferences[old],0,sizeof preferences[old]);}d->port=port;if(port>=0)reserve_port(d,port);else save_preferences();publish();return 1;}
int noble_input_add_mapping(const char *mapping){int r;FILE *f;if(!mapping||strchr(mapping,'\n')||strchr(mapping,'\r'))return 0;r=SDL_AddGamepadMapping(mapping);if(r<0)return 0;if(mappings_path[0]){f=fopen(mappings_path,"ab");if(!f)return 0;if(fprintf(f,"%s\n",mapping)<0){fclose(f);return 0;}if(fclose(f))return 0;}return 1;}
int noble_input_mouse_capture_active(void){return initialized&&config.mouse_camera&&focused&&!panel_active&&context==NI_GAMEPLAY&&camera_armed&&SDL_GetTicks()-camera_last_take<=100;}
float noble_input_camera_seconds(void){uint64_t now,ticks;float seconds;if(!initialized)return -1;now=SDL_GetTicksNS();ticks=SDL_GetTicks();seconds=camera_update_ns?(float)((now-camera_update_ns)/1000000000.0):(1.0f/60.0f);camera_update_ns=now;
 /* The first eligible title update after a menu/stall uses one normal tick,
  * rather than replaying a capped tenth of a second of controller motion. */
 if(seconds>0.1f)seconds=1.0f/60.0f;
 if(!camera_armed||ticks-camera_last_take>100){camera_dx=camera_dy=0;discard_motion=1;}
 camera_last_take=ticks;camera_armed=focused&&!panel_active;return seconds;}
void noble_input_set_native_source(int active){native_source=!!active;}
void noble_input_take_mouse(float *dx,float *dy){uint64_t now=SDL_GetTicks();int fresh=camera_armed&&now-camera_last_take<=100&&focused&&!panel_active;
 if(dx)*dx=fresh?camera_dx:0;
 if(dy)*dy=fresh?camera_dy:0;
 camera_dx=camera_dy=0;
 camera_last_take=now;camera_armed=focused&&!panel_active&&config.mouse_camera;
}
