#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "../src/controls/controls_panel.h"
#include "../src/controls/input_host.h"
static NiConfig saved;
static int active, applies;
static int pump_changes;
static NiRawState preview;
static char imported[2048];
static int queued_east;
void noble_input_get_config(NiConfig *c){*c=saved;}
int noble_input_apply_config(const NiConfig *c){saved=*c;applies++;return 1;}
void noble_input_set_panel_active(int a){active=a;}
void noble_input_set_ui_pump(void (*f)(void)){(void)f;pump_changes++;}
void noble_input_preview(unsigned p,NiRawState *r,NiOutput *o){(void)p;*r=preview;memset(o,0,sizeof(*o));}
NiFamily noble_input_family(void){return NI_FAMILY_XBOX;}
int noble_input_take_binding(NiBinding *b,NiDevice *d){if(!queued_east)return 0;queued_east=0;b->kind=NI_PAD_BUTTON;b->code=SDL_GAMEPAD_BUTTON_EAST;b->sign=1;*d=NI_GAMEPAD;return 1;}
int noble_input_devices(NobleInputDevice *d,int n){if(n<1)return 0;memset(d,0,sizeof(*d));d->id=42;d->port=-1;strcpy(d->guid,"03000000123400005678000000000000");strcpy(d->name,"Unknown joystick");return 1;}
int noble_input_assign(unsigned id,int p){(void)id;(void)p;return 1;}
int noble_input_add_mapping(const char *m){snprintf(imported,sizeof(imported),"%s",m);return 1;}
#ifdef PANEL_RED
void controls_panel_open(void){}
void controls_panel_pump(void){}
void controls_panel_shutdown(void){}
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL: %s at %d\n",#x,__LINE__);return 1;}}while(0)
static void foreground(HWND w){
 DWORD own=GetCurrentThreadId(),other=GetWindowThreadProcessId(GetForegroundWindow(),NULL);
 if(other!=own)AttachThreadInput(own,other,TRUE);
 SetForegroundWindow(w);SetActiveWindow(w);
 if(other!=own)AttachThreadInput(own,other,FALSE);
}
static BOOL CALLBACK find_panel(HWND w,LPARAM result){
 char name[64];GetClassNameA(w,name,sizeof(name));
 if(!strcmp(name,"NobleControlsPanel")){*(HWND *)result=w;return FALSE;}return TRUE;
}
static HWND own_panel(void){HWND w=NULL;EnumThreadWindows(GetCurrentThreadId(),find_panel,(LPARAM)&w);return w;}
int main(int argc,char **argv){
 HWND w; NiConfig original;
 ni_config_defaults(&saved); original=saved;
 controls_panel_open(); controls_panel_pump();
 w=own_panel(); CHECK(w != NULL);CHECK(active);CHECK(pump_changes==0);{char text[512];GetWindowTextA(GetDlgItem(w,107),text,sizeof(text));CHECK(strstr(text,"Slot 1:")!=NULL);}
 /* Until a title menu-context hook exists, do not expose inert menu settings. */
 CHECK(SendMessage(GetDlgItem(w,100),CB_GETCOUNT,0,0)==1);
 if(argc>1 && !strcmp(argv[1],"--show")){DWORD until=GetTickCount()+60000;while(IsWindow(w)&&GetTickCount()<until){controls_panel_pump();Sleep(10);}controls_panel_shutdown();return 0;}
 /* A preset is staged: Cancel must discard it and release capture suppression. */
 SendMessage(w,WM_COMMAND,120,0); CHECK(!memcmp(&saved,&original,sizeof(saved)));
 SendMessage(w,WM_COMMAND,IDCANCEL,0);CHECK(!IsWindow(w));CHECK(!active);CHECK(!applies);
 controls_panel_open();w=own_panel();CHECK(w);
 SendMessage(w,WM_COMMAND,121,0);CHECK(!applies);
 SetWindowTextA(GetDlgItem(w,130),"invalid");
 SendMessage(w,WM_COMMAND,IDOK,0);CHECK(!applies);CHECK(IsWindow(w));
 SetWindowTextA(GetDlgItem(w,130),"0.12");
 SendMessage(w,WM_COMMAND,IDOK,0);CHECK(applies==1);CHECK(!active);CHECK(!IsWindow(w));
 controls_panel_open();w=own_panel();CHECK(w);
 SendMessage(w,WM_COMMAND,104,0);Sleep(270);
 PostMessage(w,WM_KEYDOWN,'G',0);controls_panel_pump();
 SendMessage(w,WM_COMMAND,IDOK,0);CHECK(applies==2);CHECK(saved.bindings[0][0][0][0].code==10);
 /* Sharing a binding requires an explicit confirmation and remains staged. */
 controls_panel_open();w=own_panel();CHECK(w);
 SendMessage(GetDlgItem(w,102),LB_SETCURSEL,1,0);
 SendMessage(w,WM_COMMAND,104,0);Sleep(270);
 PostMessage(w,WM_KEYDOWN,'G',0);controls_panel_pump();
 CHECK(IsWindowEnabled(GetDlgItem(w,122)));CHECK(saved.bindings[0][0][1][0].code!=10);
 SendMessage(w,WM_COMMAND,122,0);CHECK(!IsWindowEnabled(GetDlgItem(w,122)));
 SendMessage(w,WM_COMMAND,IDOK,0);CHECK(applies==3);CHECK(saved.bindings[0][0][1][0].code==10);
 controls_panel_open();w=own_panel();CHECK(w);
 /* The wizard consumes only the selected device, and an axis must return to neutral. */
 foreground(w);controls_panel_pump();CHECK(GetForegroundWindow()==w);
 SendMessage(w,WM_COMMAND,115,0);
 {SDL_Event e;memset(&e,0,sizeof(e));e.type=SDL_EVENT_JOYSTICK_BUTTON_DOWN;e.jbutton.which=99;e.jbutton.button=7;controls_panel_event(&e);
  e.jbutton.which=42;e.jbutton.button=2;controls_panel_event(&e);
  e.type=SDL_EVENT_JOYSTICK_AXIS_MOTION;e.jaxis.which=42;e.jaxis.axis=0;e.jaxis.value=30000;controls_panel_event(&e);controls_panel_event(&e);
 }
 SendMessage(w,WM_COMMAND,117,0);CHECK(strstr(imported,"a:b2,b:+a0,")!=NULL);CHECK(strstr(imported,"x:")==NULL);
 controls_panel_shutdown();
 controls_panel_open();w=own_panel();CHECK(w);
 /* Pad South activates the focused Cancel button using local control messages. */
 foreground(w);controls_panel_pump();CHECK(GetForegroundWindow()==w);
 SetWindowTextA(GetDlgItem(w,130),"0.12");SetFocus(GetDlgItem(w,130));Sleep(310);
 foreground(w);SetFocus(GetDlgItem(w,130));preview.pad_buttons=1u<<SDL_GAMEPAD_BUTTON_DPAD_UP;controls_panel_pump();
 {char value[32];GetWindowTextA(GetDlgItem(w,130),value,sizeof(value));CHECK(strtof(value,NULL)>.125f);}
 preview.pad_buttons=0;controls_panel_pump();
 SetFocus(GetDlgItem(w,IDCANCEL));Sleep(170);preview.pad_buttons=1u<<SDL_GAMEPAD_BUTTON_SOUTH;
 controls_panel_pump();CHECK(!IsWindow(w));CHECK(!active);preview.pad_buttons=0;
 controls_panel_open();w=own_panel();CHECK(w);
 foreground(w);controls_panel_pump();
 SendMessage(GetDlgItem(w,101),CB_SETCURSEL,1,0);
 SendMessage(w,WM_COMMAND,104,0);Sleep(270);queued_east=1;preview.pad_buttons=1u<<SDL_GAMEPAD_BUTTON_EAST;
 controls_panel_pump();Sleep(610);controls_panel_pump();
 {char text[256];GetWindowTextA(GetDlgItem(w,108),text,sizeof(text));CHECK(strstr(text,"Changes are staged")!=NULL);}
 preview.pad_buttons=0;controls_panel_pump();
 SendMessage(w,WM_COMMAND,104,0);Sleep(270);queued_east=1;preview.pad_buttons=1u<<SDL_GAMEPAD_BUTTON_EAST;
 controls_panel_pump();preview.pad_buttons=0;controls_panel_pump();CHECK(IsWindowEnabled(GetDlgItem(w,122)));
 controls_panel_shutdown();CHECK(!IsWindow(w));CHECK(!active);
 puts("PASS: panel preset staging, Apply, Cancel, and shutdown");return 0;
}
