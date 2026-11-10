#pragma once
// Linux: the Win32 names the shared sources use. The COM and base types are vkd3d's (D3D12 runs on it here); the few
// functions are SDL's window and keyboard underneath (linux/win32.cpp), the main loop's (app/main.cpp).
#include <vkd3d_windows.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <unistd.h>

typedef void* HWND;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HCURSOR;
typedef void* HMONITOR;
typedef const wchar_t* PCWSTR;
typedef const wchar_t* LPCWSTR;
typedef const char* LPCSTR;
typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef intptr_t LPARAM;
typedef int64_t LONG_PTR;
typedef unsigned short WORD;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

// modules: "d3d12.dll" is vkd3d's (ImGui's DX12 backend looks up D3D12SerializeRootSignature in it); no others
HMODULE GetModuleHandleA(const char* name);
HMODULE LoadLibraryA(const char* name);
inline HMODULE GetModuleHandleW(const wchar_t*) { return nullptr; }
void* GetProcAddress(HMODULE module, const char* name);
inline HANDLE GetCurrentThread() { return nullptr; }
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
inline uint32_t GetCurrentProcessId() { return uint32_t(getpid()); }
inline uint32_t GetCurrentThreadId() { return uint32_t(gettid()); }
// a thread's priority: Linux's threads keep theirs (the pipelines made ahead ask for below normal)
enum { THREAD_PRIORITY_BELOW_NORMAL = -1 };
inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
// the C runtime's 64 bit seeks
inline int _fseeki64(FILE* f, int64_t at, int whence) { return fseeko(f, off_t(at), whence); }
inline int64_t _ftelli64(FILE* f) { return int64_t(ftello(f)); }
inline void Sleep(uint32_t ms) { usleep(useconds_t(ms) * 1000); }
[[noreturn]] inline void ExitProcess(uint32_t code) { std::exit(int(code)); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }

// the C runtime's wide forms: paths here are UTF-8 bytes (std::filesystem::path::c_str() is char), modes are ASCII
inline FILE* _wfopen(const char* path, const wchar_t* mode) {
    char m[8] = {};
    for (int i = 0; i < 7 && mode[i]; i++) m[i] = char(mode[i]);
    return std::fopen(path, m);
}
const wchar_t* _wgetenv(const wchar_t* name);  // the environment's value, null when unset
inline DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t*, DWORD) { return _wgetenv(name) ? 1 : 0; }

// fence events (vkd3d signals them, linux/present.cpp): auto reset, waited on in full
HANDLE CreateEventW(void* security, BOOL manual, BOOL initial, const wchar_t* name);
#define CreateEvent CreateEventW
enum : uint32_t { INFINITE = 0xFFFFFFFF, WAIT_OBJECT_0 = 0 };
uint32_t WaitForSingleObject(HANDLE handle, uint32_t ms);
BOOL CloseHandle(HANDLE handle);
#define ZeroMemory(p, n) std::memset((p), 0, (n))
#define _countof(a) (sizeof(a) / sizeof((a)[0]))

// the game window, as its keyboard focus has it
HWND GetForegroundWindow();
BOOL IsWindow(HWND hwnd);
uint32_t GetWindowThreadProcessId(HWND hwnd, DWORD* pid);
// a key held now, by its Win32 virtual key code: bit 15 set while down
short GetAsyncKeyState(int vk);
// a key's name for a menu, by its virtual key code in bits 16 to 24 of lp (the scan code there on Windows)
int GetKeyNameTextW(LONG lp, wchar_t* name, int size);
enum : uint32_t { MAPVK_VK_TO_VSC = 0 };
inline uint32_t MapVirtualKeyW(uint32_t vk, uint32_t) { return vk; }  // the key's own code stands in for its scan code

// a message to the player; MB_YESNO answers IDYES or IDNO
enum : uint32_t {
    MB_OK = 0, MB_YESNO = 4, MB_ICONERROR = 0x10, MB_ICONQUESTION = 0x20, MB_ICONWARNING = 0x30, MB_ICONINFORMATION = 0x40,
    MB_DEFBUTTON2 = 0x100, MB_SETFOREGROUND = 0x10000, MB_TOPMOST = 0x40000,
};
enum { IDOK = 1, IDCANCEL = 2, IDYES = 6, IDNO = 7 };
int MessageBoxW(HWND parent, const wchar_t* text, const wchar_t* title, uint32_t type);
// open a folder or a page in the desktop's own program (xdg-open)
HINSTANCE ShellExecuteW(HWND, const wchar_t* verb, const wchar_t* file, const wchar_t* args, const wchar_t* dir, int show);
// a path as std::filesystem gives it here (bytes)
HINSTANCE ShellExecuteW(HWND, const wchar_t* verb, const char* file, const wchar_t* args, const wchar_t* dir, int show);
enum { SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOW = 5 };

// UTF-8 to and from wide text, as CP_UTF8 converts it
enum : uint32_t { CP_UTF8 = 65001 };
int WideCharToMultiByte(uint32_t cp, uint32_t flags, const wchar_t* in, int in_len, char* out, int out_size,
                        const char*, BOOL*);
int MultiByteToWideChar(uint32_t cp, uint32_t flags, const char* in, int in_len, wchar_t* out, int out_size);

// window messages GameInfo::message gets: the main loop makes them from SDL's events
enum : uint32_t {
    WM_ACTIVATE = 0x0006, WM_CLOSE = 0x0010, WM_SETCURSOR = 0x0020, WM_INPUT = 0x00FF, WM_KEYDOWN = 0x0100,
    WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105, WM_MOUSEMOVE = 0x0200,
    WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_APP = 0x8000,
};
enum { WA_INACTIVE = 0, WA_ACTIVE = 1, HTCLIENT = 1 };
#define LOWORD(v) ((WORD)((uintptr_t)(v) & 0xFFFF))
#define HIWORD(v) ((WORD)(((uintptr_t)(v) >> 16) & 0xFFFF))
#define MAKEINTRESOURCEW(i) ((const wchar_t*)(uintptr_t)(uint16_t)(i))
inline HCURSOR LoadCursorW(void*, const wchar_t*) { return nullptr; }
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }

// virtual key codes (letters and digits are their capitals' and digits' ASCII codes)
enum {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_CANCEL = 0x03, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05,
    VK_XBUTTON2 = 0x06, VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
    VK_MENU = 0x12, VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21,
    VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_NUMPAD0 = 0x60,
    VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9,
    VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2,
    VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91,
    VK_LSHIFT = 0xA0, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB,
    VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB,
    VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE, VK_OEM_102 = 0xE2,
};
