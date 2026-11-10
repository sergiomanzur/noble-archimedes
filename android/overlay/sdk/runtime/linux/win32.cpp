// Linux: the Win32 functions the shared sources call (linux/windows.h), over SDL's window, keyboard and message boxes
#ifndef _WIN32
#include "windows.h"

#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "linux/sdl_app.h"

extern char** environ;

namespace linuxapp {

namespace {

// SDL scancode to virtual key: letters, digits and the keys the shared sources name
struct KeyPair {
    SDL_Scancode sc;
    int vk;
};
constexpr KeyPair kKeys[] = {
    {SDL_SCANCODE_BACKSPACE, VK_BACK},     {SDL_SCANCODE_TAB, VK_TAB},          {SDL_SCANCODE_RETURN, VK_RETURN},
    {SDL_SCANCODE_KP_ENTER, VK_RETURN},    {SDL_SCANCODE_PAUSE, VK_PAUSE},      {SDL_SCANCODE_CAPSLOCK, VK_CAPITAL},
    {SDL_SCANCODE_ESCAPE, VK_ESCAPE},      {SDL_SCANCODE_SPACE, VK_SPACE},      {SDL_SCANCODE_PAGEUP, VK_PRIOR},
    {SDL_SCANCODE_PAGEDOWN, VK_NEXT},      {SDL_SCANCODE_END, VK_END},          {SDL_SCANCODE_HOME, VK_HOME},
    {SDL_SCANCODE_LEFT, VK_LEFT},          {SDL_SCANCODE_UP, VK_UP},            {SDL_SCANCODE_RIGHT, VK_RIGHT},
    {SDL_SCANCODE_DOWN, VK_DOWN},          {SDL_SCANCODE_PRINTSCREEN, VK_SNAPSHOT}, {SDL_SCANCODE_INSERT, VK_INSERT},
    {SDL_SCANCODE_DELETE, VK_DELETE},      {SDL_SCANCODE_LGUI, VK_LWIN},        {SDL_SCANCODE_RGUI, VK_RWIN},
    {SDL_SCANCODE_KP_0, VK_NUMPAD0},       {SDL_SCANCODE_KP_1, VK_NUMPAD1},     {SDL_SCANCODE_KP_2, VK_NUMPAD2},
    {SDL_SCANCODE_KP_3, VK_NUMPAD3},       {SDL_SCANCODE_KP_4, VK_NUMPAD4},     {SDL_SCANCODE_KP_5, VK_NUMPAD5},
    {SDL_SCANCODE_KP_6, VK_NUMPAD6},       {SDL_SCANCODE_KP_7, VK_NUMPAD7},     {SDL_SCANCODE_KP_8, VK_NUMPAD8},
    {SDL_SCANCODE_KP_9, VK_NUMPAD9},       {SDL_SCANCODE_KP_MULTIPLY, VK_MULTIPLY}, {SDL_SCANCODE_KP_PLUS, VK_ADD},
    {SDL_SCANCODE_KP_MINUS, VK_SUBTRACT},  {SDL_SCANCODE_KP_PERIOD, VK_DECIMAL}, {SDL_SCANCODE_KP_DIVIDE, VK_DIVIDE},
    {SDL_SCANCODE_F1, VK_F1},              {SDL_SCANCODE_F2, VK_F2},            {SDL_SCANCODE_F3, VK_F3},
    {SDL_SCANCODE_F4, VK_F4},              {SDL_SCANCODE_F5, VK_F5},            {SDL_SCANCODE_F6, VK_F6},
    {SDL_SCANCODE_F7, VK_F7},              {SDL_SCANCODE_F8, VK_F8},            {SDL_SCANCODE_F9, VK_F9},
    {SDL_SCANCODE_F10, VK_F10},            {SDL_SCANCODE_F11, VK_F11},          {SDL_SCANCODE_F12, VK_F12},
    {SDL_SCANCODE_NUMLOCKCLEAR, VK_NUMLOCK}, {SDL_SCANCODE_SCROLLLOCK, VK_SCROLL}, {SDL_SCANCODE_LSHIFT, VK_LSHIFT},
    {SDL_SCANCODE_RSHIFT, VK_RSHIFT},      {SDL_SCANCODE_LCTRL, VK_LCONTROL},   {SDL_SCANCODE_RCTRL, VK_RCONTROL},
    {SDL_SCANCODE_LALT, VK_LMENU},         {SDL_SCANCODE_RALT, VK_RMENU},       {SDL_SCANCODE_SEMICOLON, VK_OEM_1},
    {SDL_SCANCODE_EQUALS, VK_OEM_PLUS},    {SDL_SCANCODE_COMMA, VK_OEM_COMMA},  {SDL_SCANCODE_MINUS, VK_OEM_MINUS},
    {SDL_SCANCODE_PERIOD, VK_OEM_PERIOD},  {SDL_SCANCODE_SLASH, VK_OEM_2},      {SDL_SCANCODE_GRAVE, VK_OEM_3},
    {SDL_SCANCODE_LEFTBRACKET, VK_OEM_4},  {SDL_SCANCODE_BACKSLASH, VK_OEM_5},  {SDL_SCANCODE_RIGHTBRACKET, VK_OEM_6},
    {SDL_SCANCODE_APOSTROPHE, VK_OEM_7},   {SDL_SCANCODE_NONUSBACKSLASH, VK_OEM_102},
};

bool Held(SDL_Scancode sc) {
    int n = 0;
    const bool* keys = SDL_GetKeyboardState(&n);
    return keys && int(sc) < n && keys[sc];
}

}  // namespace

int VirtualKey(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    for (const KeyPair& k : kKeys)
        if (k.sc == sc) return k.vk;
    return 0;
}

std::string Utf8(const wchar_t* text) {
    std::string out;
    for (; text && *text; text++) {
        uint32_t c = uint32_t(*text);
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | c >> 6);
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | c >> 12);
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | c >> 18);
            out += char(0x80 | (c >> 12 & 0x3F));
            out += char(0x80 | (c >> 6 & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring Wide(const std::string& text) {
    std::wstring out;
    for (size_t i = 0; i < text.size();) {
        uint8_t b = uint8_t(text[i]);
        int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
        uint32_t c = more ? b & (0x3F >> more) : b;
        for (i++; more-- > 0 && i < text.size(); i++) c = c << 6 | (uint8_t(text[i]) & 0x3F);
        out += wchar_t(c);
    }
    return out;
}

void Spawn(const char* program, const char* arg) {
#ifdef __ANDROID__
    // Android has no xdg-open; SDL's Android URL opener covers the useful case.
    (void)program;
    SDL_OpenURL(arg);
#else
    pid_t pid = 0;
    char* argv[] = {const_cast<char*>(program), const_cast<char*>(arg), nullptr};
    if (posix_spawnp(&pid, program, nullptr, nullptr, argv, environ) == 0) {
        // reaped by a thread of its own, so it leaves no zombie
        std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    }
#endif
}

}  // namespace linuxapp

using namespace linuxapp;

HMODULE GetModuleHandleA(const char* name) {
    // D3D12SerializeRootSignature is vkd3d-utils' here
#ifdef __ANDROID__
    return name && !std::strcmp(name, "d3d12.dll") ? dlopen("libvkd3d-utils.so", RTLD_NOW) : nullptr;
#else
    return name && !std::strcmp(name, "d3d12.dll") ? dlopen("libvkd3d-utils.so.1", RTLD_NOW) : nullptr;
#endif
}
HMODULE LoadLibraryA(const char* name) { return GetModuleHandleA(name); }
void* GetProcAddress(HMODULE module, const char* name) { return module ? dlsym(module, name) : nullptr; }

HWND GetForegroundWindow() { return g_focused ? g_window.load() : nullptr; }
BOOL IsWindow(HWND hwnd) { return hwnd && hwnd == g_window.load(); }
uint32_t GetWindowThreadProcessId(HWND, DWORD* pid) {
    if (pid) *pid = DWORD(getpid());
    return 1;
}

short GetAsyncKeyState(int vk) {
    bool down = false;
    switch (vk) {
        case VK_SHIFT:
            down = Held(SDL_SCANCODE_LSHIFT) || Held(SDL_SCANCODE_RSHIFT);
            break;
        case VK_CONTROL:
            down = Held(SDL_SCANCODE_LCTRL) || Held(SDL_SCANCODE_RCTRL);
            break;
        case VK_MENU:
            down = Held(SDL_SCANCODE_LALT) || Held(SDL_SCANCODE_RALT);
            break;
        case VK_LBUTTON:
            down = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
            break;
        default:
            for (int sc = 0; sc < SDL_SCANCODE_COUNT && !down; sc++)
                down = VirtualKey(SDL_Scancode(sc)) == vk && Held(SDL_Scancode(sc));
    }
    return down ? short(0x8000) : 0;
}

int GetKeyNameTextW(LONG lp, wchar_t* name, int size) {
    int vk = (lp >> 16) & 0xFF;  // bit 24, the extended key, left out
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++) {
        if (VirtualKey(SDL_Scancode(sc)) != vk) continue;
        const char* key = SDL_GetKeyName(SDL_GetKeyFromScancode(SDL_Scancode(sc), SDL_KMOD_NONE, false));
        std::wstring w = Wide(key ? key : "");
        if (w.empty() || size <= 0) return 0;
        int n = std::min<int>(int(w.size()), size - 1);
        std::wmemcpy(name, w.c_str(), size_t(n));
        name[n] = 0;
        return n;
    }
    return 0;
}

int MessageBoxW(HWND, const wchar_t* text, const wchar_t* title, uint32_t type) {
    std::string t = Utf8(text), caption = Utf8(title);
    SDL_MessageBoxFlags flags = (type & 0xF0) == MB_ICONERROR     ? SDL_MESSAGEBOX_ERROR
                                : (type & 0xF0) == MB_ICONWARNING ? SDL_MESSAGEBOX_WARNING
                                                                  : SDL_MESSAGEBOX_INFORMATION;
    std::vector<SDL_MessageBoxButtonData> buttons;
    if ((type & 0xF) == MB_YESNO) {
        bool no_first = type & MB_DEFBUTTON2;
        buttons.push_back({Uint32(no_first ? 0 : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), IDYES, "Yes"});
        buttons.push_back({Uint32(SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
                                      (no_first ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0)),
                           IDNO, "No"});
    } else {
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, IDOK, "OK"});
    }
    SDL_MessageBoxData data{flags, g_window.load(), caption.c_str(), t.c_str(), int(buttons.size()), buttons.data(),
                            nullptr};
    int pressed = (type & 0xF) == MB_YESNO ? IDNO : IDOK;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        // no display to show it on: the log has it
        std::fprintf(stderr, "%s: %s\n", caption.c_str(), t.c_str());
    }
    return pressed;
}

HINSTANCE ShellExecuteW(HWND, const wchar_t*, const wchar_t* file, const wchar_t*, const wchar_t*, int) {
    std::string path = Utf8(file);
    Spawn("xdg-open", path.c_str());
    return reinterpret_cast<HINSTANCE>(intptr_t(33));  // above 32: started
}

const wchar_t* _wgetenv(const wchar_t* name) {
    const char* v = std::getenv(Utf8(name).c_str());
    if (!v) return nullptr;
    thread_local std::wstring value;
    value = Wide(v);
    return value.c_str();
}

HINSTANCE ShellExecuteW(HWND, const wchar_t*, const char* file, const wchar_t*, const wchar_t*, int) {
    Spawn("xdg-open", file);
    return reinterpret_cast<HINSTANCE>(intptr_t(33));
}

int WideCharToMultiByte(uint32_t, uint32_t, const wchar_t* in, int in_len, char* out, int out_size, const char*, BOOL*) {
    std::wstring w = in_len < 0 ? std::wstring(in) : std::wstring(in, size_t(in_len));
    std::string s = Utf8(w.c_str());
    if (in_len < 0) s.push_back('\0');
    if (!out_size) return int(s.size());
    int n = std::min<int>(int(s.size()), out_size);
    std::memcpy(out, s.data(), size_t(n));
    return n;
}

int MultiByteToWideChar(uint32_t, uint32_t, const char* in, int in_len, wchar_t* out, int out_size) {
    std::wstring w = Wide(in_len < 0 ? std::string(in) : std::string(in, size_t(in_len)));
    if (in_len < 0) w.push_back(L'\0');
    if (!out_size) return int(w.size());
    int n = std::min<int>(int(w.size()), out_size);
    std::wmemcpy(out, w.data(), size_t(n));
    return n;
}
#endif
