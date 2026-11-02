#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <SDL3/SDL.h>
#include <string.h>
#include "input_host.h"
#include "input_win32.h"

static HWND raw_window;
static int cursor_hidden, was_capture, had_focus;
unsigned noble_win32_mouse_button(unsigned pair)
{
    static const unsigned buttons[5]={SDL_BUTTON_LEFT,SDL_BUTTON_RIGHT,
        SDL_BUTTON_MIDDLE,SDL_BUTTON_X1,SDL_BUTTON_X2};
    return pair<5 ? buttons[pair] : 0;
}
/* Windows set-1 scan positions to USB/SDL scancodes; preserve physical keys
 * instead of deriving bindings from typed text or keyboard layout. */
unsigned noble_win32_scancode(unsigned make, unsigned flags)
{
    static const unsigned char normal[128] = {
      [1]=41,[2]=30,[3]=31,[4]=32,[5]=33,[6]=34,[7]=35,[8]=36,[9]=37,[10]=38,[11]=39,
      [12]=45,[13]=46,[14]=42,[15]=43,[16]=20,[17]=26,[18]=8,[19]=21,[20]=23,[21]=28,
      [22]=24,[23]=12,[24]=18,[25]=19,[26]=47,[27]=48,[28]=40,[29]=224,[30]=4,[31]=22,
      [32]=7,[33]=9,[34]=10,[35]=11,[36]=13,[37]=14,[38]=15,[39]=51,[40]=52,[41]=53,
      [42]=225,[43]=49,[44]=29,[45]=27,[46]=6,[47]=25,[48]=5,[49]=17,[50]=16,[51]=54,
      [52]=55,[53]=56,[54]=229,[55]=85,[56]=226,[57]=44,[58]=57,[59]=58,[60]=59,[61]=60,
      [62]=61,[63]=62,[64]=63,[65]=64,[66]=65,[67]=66,[68]=67,[69]=83,[70]=71,
      [71]=95,[72]=96,[73]=97,[74]=86,[75]=92,[76]=93,[77]=94,[78]=87,[79]=89,[80]=90,
      [81]=91,[82]=98,[83]=99,[86]=100,[87]=68,[88]=69
    };
    if (flags & RI_KEY_E1) return make==0x45 ? SDL_SCANCODE_PAUSE : 0;
    if (flags & RI_KEY_E0) {
        switch(make) {
        case 0x1c:return SDL_SCANCODE_KP_ENTER;case 0x1d:return SDL_SCANCODE_RCTRL;
        case 0x35:return SDL_SCANCODE_KP_DIVIDE;case 0x37:return SDL_SCANCODE_PRINTSCREEN;
        case 0x38:return SDL_SCANCODE_RALT;case 0x47:return SDL_SCANCODE_HOME;
        case 0x48:return SDL_SCANCODE_UP;case 0x49:return SDL_SCANCODE_PAGEUP;
        case 0x4b:return SDL_SCANCODE_LEFT;case 0x4d:return SDL_SCANCODE_RIGHT;
        case 0x4f:return SDL_SCANCODE_END;case 0x50:return SDL_SCANCODE_DOWN;
        case 0x51:return SDL_SCANCODE_PAGEDOWN;case 0x52:return SDL_SCANCODE_INSERT;
        case 0x53:return SDL_SCANCODE_DELETE;case 0x5b:return SDL_SCANCODE_LGUI;
        case 0x5c:return SDL_SCANCODE_RGUI;case 0x5d:return SDL_SCANCODE_APPLICATION;
        default:return 0;
        }
    }
    return make<128 ? normal[make] : 0;
}

static int process_foreground(HWND *foreground)
{
    DWORD pid=0; HWND h=GetForegroundWindow();
    if(foreground)*foreground=h;
    if(h)GetWindowThreadProcessId(h,&pid);
    return h && pid==GetCurrentProcessId() && h!=raw_window;
}

static void release_cursor(void)
{
    if(was_capture)ClipCursor(NULL);
    if(cursor_hidden)ShowCursor(TRUE);
    cursor_hidden=was_capture=0;
}

static LRESULT CALLBACK raw_proc(HWND h,UINT message,WPARAM w,LPARAM l)
{
    if(message==WM_INPUT) {
        union { RAWINPUT data; unsigned char space[1024]; } input;
        UINT size=sizeof(input); SDL_Event e;
        if(SDL_GetKeyboardFocus() || !process_foreground(NULL))
            return DefWindowProc(h,message,w,l);
        if(GetRawInputData((HRAWINPUT)l,RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER))==(UINT)-1)
            return DefWindowProc(h,message,w,l);
        memset(&e,0,sizeof(e));
        if(input.data.header.dwType==RIM_TYPEKEYBOARD) {
            RAWKEYBOARD *key=&input.data.data.keyboard;
            unsigned scan=noble_win32_scancode(key->MakeCode,key->Flags);
            if(scan) {
                e.type=(key->Flags&RI_KEY_BREAK)?SDL_EVENT_KEY_UP:SDL_EVENT_KEY_DOWN;
                e.key.scancode=(SDL_Scancode)scan;e.key.down=!(key->Flags&RI_KEY_BREAK);
                noble_input_event(&e);
            }
        } else if(input.data.header.dwType==RIM_TYPEMOUSE) {
            RAWMOUSE *mouse=&input.data.data.mouse;unsigned i;
            if(!(mouse->usFlags&MOUSE_MOVE_ABSOLUTE)&&(mouse->lLastX||mouse->lLastY)) {
                e.type=SDL_EVENT_MOUSE_MOTION;e.motion.xrel=(float)mouse->lLastX;e.motion.yrel=(float)mouse->lLastY;
                noble_input_event(&e);
            }
            for(i=0;i<5;i++) {
                unsigned down=1u<<(2*i),up=2u<<(2*i);
                if(mouse->usButtonFlags&(down|up)) {
                    memset(&e,0,sizeof(e));e.type=(mouse->usButtonFlags&down)?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
                    e.button.button=(Uint8)noble_win32_mouse_button(i);e.button.down=(mouse->usButtonFlags&down)!=0;
                    noble_input_event(&e);
                }
            }
            if(mouse->usButtonFlags&RI_MOUSE_WHEEL) {
                memset(&e,0,sizeof(e));e.type=SDL_EVENT_MOUSE_WHEEL;e.wheel.y=(SHORT)mouse->usButtonData/(float)WHEEL_DELTA;
                noble_input_event(&e);
            }
        }
    }
    return DefWindowProc(h,message,w,l);
}

int noble_win32_input_init(void)
{
    WNDCLASSA cls; RAWINPUTDEVICE devices[2];
    if(raw_window)return 1;
    memset(&cls,0,sizeof cls);cls.lpfnWndProc=raw_proc;
    cls.hInstance=GetModuleHandle(NULL);cls.lpszClassName="NobleControlsRawInput";
    if(!RegisterClassA(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 0;
    raw_window=CreateWindowA(cls.lpszClassName,"",0,0,0,0,0,HWND_MESSAGE,NULL,cls.hInstance,NULL);
    if(!raw_window)return 0;
    memset(devices,0,sizeof devices);
    devices[0].usUsagePage=devices[1].usUsagePage=1;
    devices[0].usUsage=2;devices[1].usUsage=6;
    devices[0].dwFlags=devices[1].dwFlags=RIDEV_INPUTSINK;
    devices[0].hwndTarget=devices[1].hwndTarget=raw_window;
    if(!RegisterRawInputDevices(devices,2,sizeof devices[0])) {
        DestroyWindow(raw_window);raw_window=NULL;return 0;
    }
    noble_input_set_native_source(1);return 1;
}

void noble_win32_input_pump(void)
{
    MSG message; HWND foreground;int focus;
    if(!raw_window)return;
    /* Drain the sink even while SDL owns the active window. raw_proc ignores
     * those messages; leaving them queued would replay stale keys after F2. */
    while(PeekMessage(&message,raw_window,0,0,PM_REMOVE)) {
        TranslateMessage(&message);DispatchMessage(&message);
    }
    /* SDL owns keyboard/mouse input whenever its render window has focus. */
    if(SDL_GetKeyboardFocus()){release_cursor();had_focus=0;return;}
    focus=process_foreground(&foreground);
    if(focus!=had_focus){noble_input_set_focus(focus);had_focus=focus;}
    if(focus&&noble_input_mouse_capture_active()) {
        RECT r;GetClientRect(foreground,&r);
        MapWindowPoints(foreground,NULL,(POINT*)&r,2);ClipCursor(&r);was_capture=1;
        if(!cursor_hidden){ShowCursor(FALSE);cursor_hidden=1;}
    } else release_cursor();
}

void noble_win32_input_shutdown(void)
{
    release_cursor();
    if(raw_window) {
        RAWINPUTDEVICE devices[2]={{1,2,RIDEV_REMOVE,NULL},{1,6,RIDEV_REMOVE,NULL}};
        RegisterRawInputDevices(devices,2,sizeof devices[0]);
        DestroyWindow(raw_window);raw_window=NULL;
    }
    noble_input_set_native_source(0);had_focus=0;
}
