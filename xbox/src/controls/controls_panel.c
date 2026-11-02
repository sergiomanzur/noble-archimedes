#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "controls_panel.h"
#include "input_host.h"
enum { C_CONTEXT=100,C_DEVICE,C_ACTION,C_SLOT,C_LISTEN,C_STOP,C_CLEAR,C_BINDING,C_STATUS,
 C_FAMILY,C_DEVICES,C_PORT,C_ASSIGN,C_REFRESH,C_MAPPING,C_MAPSTART,C_MAPSKIP,C_MAPADD,
 C_PREVIEW,C_CLASSIC=120,C_PC,C_SHARE,C_NUMBER=130,C_INVERT=150,C_CAMERA=160 };
static HWND panel;
static HFONT fitted_font;
static float panel_scale;
static NiConfig draft;
static NiBinding pending_binding;
static int pending_conflict;
static int pending_east;
static DWORD east_since;
static int listening, listen_context,listen_device,listen_action,listen_slot, wizard=-1;
static DWORD listen_after,nav_after,preview_after;
static unsigned nav_previous;
static NobleInputDevice devices[32];
static int device_count;
static unsigned wizard_id;
static int wizard_axes[256];
static unsigned char wizard_hats[256];
static char mapping[2048];
static const char *map_fields[]={"a","b","x","y","back","guide","start","leftstick","rightstick","leftshoulder","rightshoulder","dpup","dpdown","dpleft","dpright","leftx","lefty","rightx","righty","lefttrigger","righttrigger"};
static const char *number_names[]={"Left inner zone","Left outer zone","Right inner zone","Right outer zone","Trigger threshold","Mouse X sensitivity","Mouse Y sensitivity","Pad look X sensitivity","Pad look Y sensitivity"};
static HWND item(int id){return GetDlgItem(panel,id);}
static int selection(int id){return (int)SendMessage(item(id),CB_GETCURSEL,0,0);}
static void status(const char *s){SetWindowTextA(item(C_STATUS),s);}
static HWND control(const char *cls,const char *text,DWORD style,int id,int x,int y,int w,int h){
 HWND c=CreateWindowExA(!strcmp(cls,"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,panel,(HMENU)(INT_PTR)id,GetModuleHandle(NULL),NULL);
 SendMessage(c,WM_SETFONT,(WPARAM)GetStockObject(DEFAULT_GUI_FONT),TRUE);return c;
}
static void label(const char *s,int x,int y,int w){control("STATIC",s,0,0,x,y,w,20);}
static void button(const char *s,int id,int x,int y,int w){control("BUTTON",s,WS_TABSTOP,id,x,y,w,26);}
static void combo(int id,int x,int y,int w,const char *const *names,int count){int i;control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,id,x,y,w,220);for(i=0;i<count;i++)SendMessageA(item(id),CB_ADDSTRING,0,(LPARAM)names[i]);SendMessage(item(id),CB_SETCURSEL,0,0);}
static void values_out(void){
 float v[]={draft.left_inner,draft.left_outer,draft.right_inner,draft.right_outer,draft.trigger_threshold,draft.mouse_sensitivity_x,draft.mouse_sensitivity_y,draft.look_sensitivity_x,draft.look_sensitivity_y};
 int b[]={draft.mouse_invert_x,draft.mouse_invert_y,draft.pad_invert_x,draft.pad_invert_y};int i;char text[40];
 for(i=0;i<9;i++){snprintf(text,sizeof(text),"%.4g",v[i]);SetWindowTextA(item(C_NUMBER+i),text);}
 for(i=0;i<4;i++)SendMessage(item(C_INVERT+i),BM_SETCHECK,b[i]?BST_CHECKED:BST_UNCHECKED,0);
 SendMessage(item(C_CAMERA),BM_SETCHECK,draft.mouse_camera?BST_CHECKED:BST_UNCHECKED,0);
 SendMessage(item(C_FAMILY),CB_SETCURSEL,draft.prompt_family,0);
}
static int values_in(NiConfig *c){
 float *v[]={&c->left_inner,&c->left_outer,&c->right_inner,&c->right_outer,&c->trigger_threshold,&c->mouse_sensitivity_x,&c->mouse_sensitivity_y,&c->look_sensitivity_x,&c->look_sensitivity_y};
 int *b[]={&c->mouse_invert_x,&c->mouse_invert_y,&c->pad_invert_x,&c->pad_invert_y};int i;char text[64],*end;
 for(i=0;i<9;i++){GetWindowTextA(item(C_NUMBER+i),text,sizeof(text));*v[i]=strtof(text,&end);while(*end==' ')end++;if(end==text||*end||!isfinite(*v[i]))return 0;}
 for(i=0;i<4;i++)*b[i]=SendMessage(item(C_INVERT+i),BM_GETCHECK,0,0)==BST_CHECKED;
 c->mouse_camera=SendMessage(item(C_CAMERA),BM_GETCHECK,0,0)==BST_CHECKED;c->prompt_family=(NiFamily)selection(C_FAMILY);
 return ni_config_valid(c);
}
static void binding_labels(void){
 int a=(int)SendMessage(item(C_ACTION),LB_GETCURSEL,0,0),d=selection(C_DEVICE),c=selection(C_CONTEXT),s;char text[512],b[128];NiFamily f=(NiFamily)selection(C_FAMILY);
 if(a<0||d<0||c<0)return;
 if(f==NI_FAMILY_AUTO)f=noble_input_family();
 text[0]=0;
 for(s=0;s<2;s++){ni_binding_label(draft.bindings[c][d][a][s],d==NI_KEYBOARD?NI_FAMILY_KEYBOARD:f,b,sizeof(b));snprintf(text+strlen(text),sizeof(text)-strlen(text),"Slot %d: %s%s",s+1,b,s?"":"    |    ");}
 SetWindowTextA(item(C_BINDING),text);
}
static void stop_listen(void){listening=0;pending_east=0;pending_conflict=0;EnableWindow(item(C_SHARE),FALSE);status("Changes are staged until Apply. Tab / arrows navigate; Enter activates; Esc cancels.");}
static void bind_value(NiBinding b,NiDevice d){
 if(!listening||GetTickCount()<listen_after||(int)d!=listen_device)return;
 if(ni_binding_conflict(&draft,(NiContext)listen_context,d,(NiAction)listen_action,listen_slot,b)){
  listening=0;pending_binding=b;pending_conflict=1;EnableWindow(item(C_SHARE),TRUE);
  status("Conflict: input already assigned in this context. Share explicitly allows both actions; Cancel listen rejects it.");
  SetFocus(item(C_SHARE));return;
 }
 draft.bindings[listen_context][listen_device][listen_action][listen_slot]=b;stop_listen();binding_labels();
}
static void refresh_devices(void){
 int i;char text[240];unsigned old=0;int selected=selection(C_DEVICES);
 if(selected>=0&&selected<device_count)old=devices[selected].id;
 device_count=noble_input_devices(devices,32);if(device_count>32)device_count=32;
 SendMessage(item(C_DEVICES),CB_RESETCONTENT,0,0);
 for(i=0;i<device_count;i++){snprintf(text,sizeof(text),"%s  [%s, port %d]",devices[i].name,devices[i].mapped?"mapped":"needs mapping",devices[i].port+1);SendMessageA(item(C_DEVICES),CB_ADDSTRING,0,(LPARAM)text);}
 selected=0;for(i=0;i<device_count;i++)if(devices[i].id==old)selected=i;
 SendMessage(item(C_DEVICES),CB_SETCURSEL,selected,0);
}
static void wizard_prompt(void){char text[200];if(wizard<0)return;snprintf(text,sizeof(text),"Mapping: move / press %s on selected device; Skip omits it. Import mapping when finished.",map_fields[wizard]);status(text);}
static void wizard_next(const char *physical){
 char part[80];if(wizard<0)return;
 if(physical){snprintf(part,sizeof(part),"%s:%s,",map_fields[wizard],physical);if(strlen(mapping)+strlen(part)<sizeof(mapping))strcat(mapping,part);}
 SetWindowTextA(item(C_MAPPING),mapping);wizard++;
 if(wizard>20){wizard=-1;status("Mapping captured. Review then choose Import mapping (device change applies immediately).");}else wizard_prompt();
}
static void close_panel(void){if(panel)DestroyWindow(panel);}
static void command(int id,int notification){
 int a=(int)SendMessage(item(C_ACTION),LB_GETCURSEL,0,0),d=selection(C_DEVICE),c=selection(C_CONTEXT),s=selection(C_SLOT),dev=selection(C_DEVICES);
 if(id==IDCANCEL){if(listening){stop_listen();return;}if(wizard>=0){wizard=-1;stop_listen();return;}close_panel();return;}
 if(id==IDOK){NiConfig candidate=draft;stop_listen();if(!values_in(&candidate)){status("Invalid values. Zones / threshold must be 0..1 with inner < outer; threshold < 1; sensitivities must be nonnegative.");return;}if(!noble_input_apply_config(&candidate)){status("Could not save settings. Changes have not been applied; check the configuration directory.");return;}close_panel();return;}
 if(id==C_CLASSIC||id==C_PC){stop_listen();if(id==C_PC)ni_config_pc_defaults(&draft);else ni_config_defaults(&draft);values_out();binding_labels();return;}
 if(id==C_LISTEN&&a>=0&&s>=0){NiBinding b;NiDevice bd;while(noble_input_take_binding(&b,&bd)){}stop_listen();listening=1;wizard=-1;listen_context=c;listen_device=d;listen_action=a;listen_slot=s;listen_after=GetTickCount()+250;status("Listening: tap desired input. Hold pad East for 0.6s to cancel; tap East to bind. Esc also cancels.");SetFocus(panel);return;}
 if(id==C_STOP){wizard=-1;stop_listen();return;}
 if(id==C_SHARE&&pending_conflict){draft.bindings[listen_context][listen_device][listen_action][listen_slot]=pending_binding;stop_listen();binding_labels();return;}
 if(id==C_CLEAR&&a>=0&&s>=0){stop_listen();memset(&draft.bindings[c][d][a][s],0,sizeof(NiBinding));binding_labels();return;}
 if(id==C_REFRESH){refresh_devices();return;}
 if(id==C_ASSIGN&&dev>=0&&dev<device_count){if(!noble_input_assign(devices[dev].id,selection(C_PORT)-1))status("Port is occupied or device disconnected. Unassign its current device first.");else{status("Device assignment updated immediately.");refresh_devices();}return;}
 if(id==C_MAPSTART&&dev>=0&&dev<device_count){char name[128];int i;strcpy(name,devices[dev].name);for(i=0;name[i];i++)if(name[i]==',')name[i]=' ';snprintf(mapping,sizeof(mapping),"%s,%s,",devices[dev].guid,name);wizard=0;memset(wizard_axes,0,sizeof(wizard_axes));memset(wizard_hats,0,sizeof(wizard_hats));wizard_id=devices[dev].id;listening=0;wizard_prompt();return;}
 if(id==C_MAPSKIP){wizard_next(NULL);return;}
 if(id==C_MAPADD){GetWindowTextA(item(C_MAPPING),mapping,sizeof(mapping));if(noble_input_add_mapping(mapping)){wizard=-1;status("SDL mapping imported and saved immediately.");refresh_devices();}else status("Mapping rejected. Use GUID,name,a:b0,... or capture a mapping first.");return;}
 if((id==C_CONTEXT||id==C_DEVICE||id==C_SLOT)&&notification==CBN_SELCHANGE)stop_listen();
 if(id==C_ACTION&&notification==LBN_SELCHANGE)stop_listen();
 if(id==C_CONTEXT||id==C_DEVICE||id==C_ACTION||id==C_SLOT||id==C_FAMILY)binding_labels();
}
static int key_scancode(WPARAM vk,LPARAM lp){
 if(vk>='A'&&vk<='Z')return SDL_SCANCODE_A+(int)(vk-'A');
 if(vk>='1'&&vk<='9')return SDL_SCANCODE_1+(int)(vk-'1');
 if(vk>=VK_F1&&vk<=VK_F12)return SDL_SCANCODE_F1+(int)(vk-VK_F1);
 switch(vk){case '0':return SDL_SCANCODE_0;case VK_SPACE:return SDL_SCANCODE_SPACE;case VK_RETURN:return SDL_SCANCODE_RETURN;case VK_TAB:return SDL_SCANCODE_TAB;case VK_BACK:return SDL_SCANCODE_BACKSPACE;case VK_UP:return SDL_SCANCODE_UP;case VK_DOWN:return SDL_SCANCODE_DOWN;case VK_LEFT:return SDL_SCANCODE_LEFT;case VK_RIGHT:return SDL_SCANCODE_RIGHT;case VK_SHIFT:return ((lp>>16)&255)==0x36?SDL_SCANCODE_RSHIFT:SDL_SCANCODE_LSHIFT;case VK_CONTROL:return (lp&(1<<24))?SDL_SCANCODE_RCTRL:SDL_SCANCODE_LCTRL;case VK_MENU:return (lp&(1<<24))?SDL_SCANCODE_RALT:SDL_SCANCODE_LALT;case VK_DELETE:return SDL_SCANCODE_DELETE;case VK_INSERT:return SDL_SCANCODE_INSERT;case VK_HOME:return SDL_SCANCODE_HOME;case VK_END:return SDL_SCANCODE_END;case VK_PRIOR:return SDL_SCANCODE_PAGEUP;case VK_NEXT:return SDL_SCANCODE_PAGEDOWN;default:break;}
 /* Translate punctuation using its printable Unicode key; SDL maps keyboard-layout keys. */
 {BYTE keys[256]={0};WCHAR ch[4];if(ToUnicode((UINT)vk,(UINT)((lp>>16)&255),keys,ch,4,0)>0)return SDL_GetScancodeFromKey((SDL_Keycode)ch[0],NULL);}
 return 0;
}
static LRESULT CALLBACK panel_proc(HWND w,UINT message,WPARAM wp,LPARAM lp){
 switch(message){case WM_COMMAND:command(LOWORD(wp),HIWORD(wp));return 0;
 case WM_CLOSE:close_panel();return 0;
 case WM_ACTIVATE:if(LOWORD(wp)==WA_INACTIVE){listening=0;wizard=-1;}return 0;
 case WM_DESTROY:if(fitted_font){DeleteObject(fitted_font);fitted_font=NULL;}panel=NULL;listening=0;wizard=-1;noble_input_set_panel_active(0);return 0;}
 return DefWindowProcA(w,message,wp,lp);
}
static BOOL CALLBACK fit_child(HWND child,LPARAM unused){
 RECT r;POINT p;(void)unused;GetWindowRect(child,&r);p.x=r.left;p.y=r.top;ScreenToClient(panel,&p);
 MoveWindow(child,(int)(p.x*panel_scale),(int)(p.y*panel_scale),(int)((r.right-r.left)*panel_scale),(int)((r.bottom-r.top)*panel_scale),TRUE);
 if(fitted_font)SendMessage(child,WM_SETFONT,(WPARAM)fitted_font,TRUE);
 return TRUE;
}
static void fit_to_work_area(const RECT *area){
 RECT r={0,0,832,728};LOGFONTA font;int frame_x,frame_y,width,height;
 AdjustWindowRectEx(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,WS_EX_CONTROLPARENT);
 frame_x=r.right-r.left-832;frame_y=r.bottom-r.top-728;
 panel_scale=fminf(1,fminf((area->right-area->left-frame_x-20)/832.0f,(area->bottom-area->top-frame_y-20)/728.0f));
 if(panel_scale<1){GetObjectA(GetStockObject(DEFAULT_GUI_FONT),sizeof(font),&font);font.lfHeight=(LONG)(font.lfHeight*panel_scale);fitted_font=CreateFontIndirectA(&font);EnumChildWindows(panel,fit_child,0);}
 width=(int)(832*panel_scale)+frame_x;height=(int)(728*panel_scale)+frame_y;
 SetWindowPos(panel,NULL,area->left+(area->right-area->left-width)/2,area->top+(area->bottom-area->top-height)/2,width,height,SWP_NOZORDER);
}
void controls_panel_open(void){
 static const char *contexts[]={"Gameplay and menus"},*types[]={"Keyboard / mouse","Gamepad"},*slots[]={"Slot 1","Slot 2"},*families[]={"Auto","Keyboard","Xbox","PlayStation","Nintendo","Generic"},*ports[]={"Unassigned","Port 1","Port 2","Port 3","Port 4"};
 static const char *inverts[]={"Invert mouse X","Invert mouse Y","Invert pad X","Invert pad Y"};
 WNDCLASSA wc={0};RECT area,rect={0,0,832,728};int i,x,y;NiRawState raw;NiOutput out;
 if(panel){ShowWindow(panel,SW_RESTORE);SetForegroundWindow(panel);return;}
 wc.lpfnWndProc=panel_proc;wc.hInstance=GetModuleHandle(NULL);wc.lpszClassName="NobleControlsPanel";wc.hCursor=LoadCursor(NULL,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);RegisterClassA(&wc);
 noble_input_get_config(&draft);SystemParametersInfo(SPI_GETWORKAREA,0,&area,0);AdjustWindowRectEx(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE,WS_EX_CONTROLPARENT);
 panel=CreateWindowExA(WS_EX_CONTROLPARENT,wc.lpszClassName,"Controls - Noble Archimedes",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,area.left+20,area.top+10,rect.right-rect.left,rect.bottom-rect.top,NULL,NULL,wc.hInstance,NULL);
 if(!panel)return;
 label("Controls: remap buttons, movement, and camera",16,12,780);
 label("Applies to",16,40,75);combo(C_CONTEXT,90,36,180,contexts,1);EnableWindow(item(C_CONTEXT),FALSE);label("Device",290,40,60);combo(C_DEVICE,345,36,210,types,2);combo(C_SLOT,575,36,110,slots,2);
 control("LISTBOX","",WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY,C_ACTION,16,75,255,247);
 for(i=0;i<NI_ACTION_COUNT;i++)SendMessageA(item(C_ACTION),LB_ADDSTRING,0,(LPARAM)ni_action_name((NiAction)i));
 SendMessage(item(C_ACTION),LB_SETCURSEL,0,0);
 label("Selected action bindings",290,78,480);control("STATIC","",0,C_BINDING,290,105,520,42);
 button("Listen",C_LISTEN,290,152,90);button("Cancel listen",C_STOP,390,152,115);button("Clear slot",C_CLEAR,515,152,100);
 button("Share",C_SHARE,625,152,90);EnableWindow(item(C_SHARE),FALSE);
 label("Prompt labels",290,201,110);combo(C_FAMILY,420,197,195,families,6);
 label("On-screen game prompts currently use the original layout.",290,236,515);
 label("Pad: D-pad navigates; South activates; East cancels; R shoulder = Tab.",290,267,520);
 label("Camera and dead zones",16,334,780);
 for(i=0;i<9;i++){x=i<5?16:425;y=362+(i<5?i:i-5)*29;label(number_names[i],x,y+3,185);control("EDIT","",WS_TABSTOP|ES_AUTOHSCROLL,C_NUMBER+i,x+190,y,100,24);}
 for(i=0;i<4;i++)control("BUTTON",inverts[i],WS_TABSTOP|BS_AUTOCHECKBOX,C_INVERT+i,425+(i%2)*185,485+(i/2)*25,180,24);
 control("BUTTON","Enable mouse camera (experimental)",WS_TABSTOP|BS_AUTOCHECKBOX,C_CAMERA,16,541,790,24);
 label("Devices / mappings (assignment and mapping changes apply immediately)",16,575,790);
 combo(C_DEVICES,16,598,450,NULL,0);combo(C_PORT,478,598,115,ports,5);button("Assign",C_ASSIGN,602,597,85);button("Refresh",C_REFRESH,697,597,115);
 control("EDIT","",WS_TABSTOP|ES_AUTOHSCROLL,C_MAPPING,16,632,450,24);SendMessage(item(C_MAPPING),EM_SETLIMITTEXT,2047,0);
 button("Capture mapping",C_MAPSTART,478,631,135);button("Skip",C_MAPSKIP,620,631,60);button("Import mapping",C_MAPADD,687,631,125);
 control("STATIC","",0,C_PREVIEW,16,664,796,20);
 button("Classic defaults",C_CLASSIC,16,690,130);button("PC defaults",C_PC,156,690,120);button("Apply",IDOK,602,690,95);button("Cancel",IDCANCEL,707,690,105);
 /* Status lives between bind controls and tuning values; a fixed rectangle avoids bottom clipping. */
 control("STATIC","",0,C_STATUS,290,292,522,42);
 fit_to_work_area(&area);values_out();binding_labels();refresh_devices();stop_listen();noble_input_set_panel_active(1);
 noble_input_preview(0,&raw,&out);nav_previous=raw.pad_buttons;nav_after=GetTickCount()+300;preview_after=0;
 ShowWindow(panel,SW_SHOW);SetForegroundWindow(panel);SetFocus(item(C_ACTION));
}
static void controller_key(UINT key){HWND focus=GetFocus();char cls[32];if(!focus||!(focus==panel||IsChild(panel,focus)))return;
 GetClassNameA(focus,cls,sizeof(cls));
 if(key==VK_TAB){HWND next=GetNextDlgTabItem(panel,focus,FALSE);if(next)SetFocus(next);return;}
 if(key==VK_RETURN){if(!strcmp(cls,"Button")){SendMessage(focus,BM_CLICK,0,0);return;}SetFocus(GetNextDlgTabItem(panel,focus,FALSE));return;}
 if(!strcmp(cls,"Edit")&&(key==VK_UP||key==VK_DOWN)){
  int id=GetDlgCtrlID(focus);char text[64];float value,step,limit;
  if(id>=C_NUMBER&&id<C_NUMBER+9){
   GetWindowTextA(focus,text,sizeof(text));value=strtof(text,NULL);
   step=id<C_NUMBER+5?.01f:id<C_NUMBER+7?.0005f:.1f;
   limit=id<C_NUMBER+5?1:id<C_NUMBER+7?10:100;
   value=fmaxf(0,fminf(limit,value+(key==VK_UP?step:-step)));
   snprintf(text,sizeof(text),"%.4g",value);SetWindowTextA(focus,text);return;
  }
 }
 if(!strcmp(cls,"Button")||focus==panel){HWND next=GetNextDlgTabItem(panel,focus,key==VK_UP||key==VK_LEFT);if(next)SetFocus(next);return;}
 SendMessage(focus,WM_KEYDOWN,key,0);SendMessage(focus,WM_KEYUP,key,0);
}
void controls_panel_pump(void){
 MSG msg;NiRawState raw;NiOutput out;NiBinding b;NiDevice d;unsigned nav,edge;DWORD now;char text[256];int dev,port=0;
 if(!panel)return;
 /* Filter to the panel and its children: SDL remains owner of guest-window messages. */
 while(panel&&PeekMessage(&msg,panel,0,0,PM_REMOVE)){
  if(msg.message==WM_KEYDOWN||msg.message==WM_SYSKEYDOWN){
   if(msg.wParam==VK_ESCAPE){command(IDCANCEL,0);continue;}
   if(listening){int code=key_scancode(msg.wParam,msg.lParam);if(code&&msg.wParam!=VK_F2){NiBinding key={NI_KEY,code,1};bind_value(key,NI_KEYBOARD);}continue;}
   if(msg.wParam==VK_RETURN){controller_key(VK_RETURN);continue;}
  }
  if(listening&&(msg.message==WM_LBUTTONDOWN||msg.message==WM_RBUTTONDOWN||msg.message==WM_MBUTTONDOWN||msg.message==WM_XBUTTONDOWN||msg.message==WM_MOUSEWHEEL)){
   NiBinding mouse={NI_MOUSE_BUTTON,msg.message==WM_LBUTTONDOWN?1:msg.message==WM_MBUTTONDOWN?2:msg.message==WM_RBUTTONDOWN?3:GET_XBUTTON_WPARAM(msg.wParam)==XBUTTON1?4:5,1};
   if(msg.message==WM_MOUSEWHEEL){mouse.kind=NI_MOUSE_WHEEL;mouse.code=0;mouse.sign=GET_WHEEL_DELTA_WPARAM(msg.wParam)>0?1:-1;}bind_value(mouse,NI_KEYBOARD);continue;
  }
  if(!panel||!IsDialogMessage(panel,&msg)){TranslateMessage(&msg);DispatchMessage(&msg);}
 }
 if(!panel)return;
 while(noble_input_take_binding(&b,&d))if(listening){
  if(d==NI_GAMEPAD&&listen_device==NI_GAMEPAD&&b.kind==NI_PAD_BUTTON&&b.code==SDL_GAMEPAD_BUTTON_EAST&&GetTickCount()>=listen_after){pending_east=1;east_since=GetTickCount();}
  else bind_value(b,d);
 }
 dev=selection(C_DEVICES);if(dev>=0&&dev<device_count&&devices[dev].port>=0)port=devices[dev].port;
 noble_input_preview((unsigned)port,&raw,&out);now=GetTickCount();nav=raw.pad_buttons;
 if(raw.axes[0]<-.65f)nav|=1u<<SDL_GAMEPAD_BUTTON_DPAD_LEFT;
 if(raw.axes[0]>.65f)nav|=1u<<SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
 if(raw.axes[1]<-.65f)nav|=1u<<SDL_GAMEPAD_BUTTON_DPAD_UP;
 if(raw.axes[1]>.65f)nav|=1u<<SDL_GAMEPAD_BUTTON_DPAD_DOWN;
 edge=nav&~nav_previous;nav_previous=nav;
 if(listening&&GetForegroundWindow()==panel){
  if(listen_device==NI_KEYBOARD&&(edge&(1u<<SDL_GAMEPAD_BUTTON_EAST)))stop_listen();
  else if(pending_east){
   if(!(raw.pad_buttons&(1u<<SDL_GAMEPAD_BUTTON_EAST))){NiBinding east={NI_PAD_BUTTON,SDL_GAMEPAD_BUTTON_EAST,1};pending_east=0;bind_value(east,NI_GAMEPAD);}
   else if(now-east_since>=600)stop_listen();
  }
 }
 if(GetForegroundWindow()==panel&&!listening&&wizard<0&&now>=nav_after&&edge){
  nav_after=now+160;
  if(edge&(1u<<SDL_GAMEPAD_BUTTON_EAST))command(IDCANCEL,0);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_SOUTH))controller_key(VK_RETURN);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_DPAD_UP))controller_key(VK_UP);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_DPAD_DOWN))controller_key(VK_DOWN);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_DPAD_LEFT))controller_key(VK_LEFT);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_DPAD_RIGHT))controller_key(VK_RIGHT);
  else if(edge&(1u<<SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))controller_key(VK_TAB);
 }
 if(panel&&now>=preview_after){
  static const char *names[]={"Auto","Keyboard","Xbox","PlayStation","Nintendo","Generic"};
  int family=selection(C_FAMILY);if(family==NI_FAMILY_AUTO)family=noble_input_family();if(family<0||family>5)family=5;
  preview_after=now+100;snprintf(text,sizeof(text),"Port %d: LX %+.2f LY %+.2f RX %+.2f RY %+.2f LT %.2f RT %.2f | Buttons %08X | %s",port+1,raw.axes[0],raw.axes[1],raw.axes[2],raw.axes[3],raw.axes[4],raw.axes[5],(unsigned)raw.pad_buttons,names[family]);SetWindowTextA(item(C_PREVIEW),text);binding_labels();
 }
}
void controls_panel_event(const void *event){
 const SDL_Event *e=(const SDL_Event *)event;char physical[40];if(!panel||!e)return;
 if(e->type==SDL_EVENT_JOYSTICK_ADDED||e->type==SDL_EVENT_JOYSTICK_REMOVED||e->type==SDL_EVENT_GAMEPAD_ADDED||e->type==SDL_EVENT_GAMEPAD_REMOVED){refresh_devices();return;}
 if(wizard<0||GetForegroundWindow()!=panel)return;
 if(e->type==SDL_EVENT_JOYSTICK_BUTTON_DOWN&&e->jbutton.which==wizard_id){snprintf(physical,sizeof(physical),"b%d",e->jbutton.button);wizard_next(physical);}
 else if(e->type==SDL_EVENT_JOYSTICK_AXIS_MOTION&&e->jaxis.which==wizard_id){int axis=e->jaxis.axis,was=wizard_axes[axis];wizard_axes[axis]=abs(e->jaxis.value)>24000;if(!was&&wizard_axes[axis]){snprintf(physical,sizeof(physical),"%sa%d",wizard<15?(e->jaxis.value<0?"-":"+"):"",axis);wizard_next(physical);}}
 else if(e->type==SDL_EVENT_JOYSTICK_HAT_MOTION&&e->jhat.which==wizard_id){unsigned char was=wizard_hats[e->jhat.hat];wizard_hats[e->jhat.hat]=e->jhat.value;if(!was&&e->jhat.value){snprintf(physical,sizeof(physical),"h%d.%d",e->jhat.hat,e->jhat.value);wizard_next(physical);}}
}
int controls_panel_is_open(void){return panel && IsWindow(panel);}
void controls_panel_shutdown(void){close_panel();}
