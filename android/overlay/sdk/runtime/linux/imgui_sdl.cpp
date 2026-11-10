// Linux: ImGui's platform side on SDL. The main thread hands it SDL's events (ImGuiEvent) and the render thread starts
// each frame (ImGuiNewFrame), both under app::UiLock, so the render thread never calls SDL's window functions, which
// Wayland wants on the main thread alone. The cursor ImGui asks for goes back to the main loop (g_cursor).
#ifndef _WIN32
#include <imgui.h>

#include <chrono>
#include <cfloat>

#include "core/log.h"
#include "linux/sdl_app.h"

namespace linuxapp {

namespace {

SDL_Window* g_imgui_window = nullptr;
std::chrono::steady_clock::time_point g_last;

ImGuiKey Key(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return ImGuiKey(ImGuiKey_A + (k - SDLK_A));
    if (k >= SDLK_0 && k <= SDLK_9) return ImGuiKey(ImGuiKey_0 + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return ImGuiKey(ImGuiKey_F1 + (k - SDLK_F1));
    switch (k) {
        case SDLK_TAB: return ImGuiKey_Tab;
        case SDLK_LEFT: return ImGuiKey_LeftArrow;
        case SDLK_RIGHT: return ImGuiKey_RightArrow;
        case SDLK_UP: return ImGuiKey_UpArrow;
        case SDLK_DOWN: return ImGuiKey_DownArrow;
        case SDLK_PAGEUP: return ImGuiKey_PageUp;
        case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
        case SDLK_HOME: return ImGuiKey_Home;
        case SDLK_END: return ImGuiKey_End;
        case SDLK_INSERT: return ImGuiKey_Insert;
        case SDLK_DELETE: return ImGuiKey_Delete;
        case SDLK_BACKSPACE: return ImGuiKey_Backspace;
        case SDLK_SPACE: return ImGuiKey_Space;
        case SDLK_RETURN: return ImGuiKey_Enter;
        case SDLK_ESCAPE: return ImGuiKey_Escape;
        case SDLK_APOSTROPHE: return ImGuiKey_Apostrophe;
        case SDLK_COMMA: return ImGuiKey_Comma;
        case SDLK_MINUS: return ImGuiKey_Minus;
        case SDLK_PERIOD: return ImGuiKey_Period;
        case SDLK_SLASH: return ImGuiKey_Slash;
        case SDLK_SEMICOLON: return ImGuiKey_Semicolon;
        case SDLK_EQUALS: return ImGuiKey_Equal;
        case SDLK_LEFTBRACKET: return ImGuiKey_LeftBracket;
        case SDLK_BACKSLASH: return ImGuiKey_Backslash;
        case SDLK_RIGHTBRACKET: return ImGuiKey_RightBracket;
        case SDLK_GRAVE: return ImGuiKey_GraveAccent;
        case SDLK_CAPSLOCK: return ImGuiKey_CapsLock;
        case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
        case SDLK_KP_0: return ImGuiKey_Keypad0;
        case SDLK_KP_1: return ImGuiKey_Keypad1;
        case SDLK_KP_2: return ImGuiKey_Keypad2;
        case SDLK_KP_3: return ImGuiKey_Keypad3;
        case SDLK_KP_4: return ImGuiKey_Keypad4;
        case SDLK_KP_5: return ImGuiKey_Keypad5;
        case SDLK_KP_6: return ImGuiKey_Keypad6;
        case SDLK_KP_7: return ImGuiKey_Keypad7;
        case SDLK_KP_8: return ImGuiKey_Keypad8;
        case SDLK_KP_9: return ImGuiKey_Keypad9;
        case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
        case SDLK_LSHIFT: return ImGuiKey_LeftShift;
        case SDLK_LALT: return ImGuiKey_LeftAlt;
        case SDLK_LGUI: return ImGuiKey_LeftSuper;
        case SDLK_RCTRL: return ImGuiKey_RightCtrl;
        case SDLK_RSHIFT: return ImGuiKey_RightShift;
        case SDLK_RALT: return ImGuiKey_RightAlt;
        case SDLK_RGUI: return ImGuiKey_RightSuper;
        default: return ImGuiKey_None;
    }
}

// a window point in the drawn picture's pixels, which ImGui lays out in
ImVec2 Pixels(SDL_Window* window, float x, float y) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    float sx = w ? float(g_pixel_w.load()) / float(w) : 1.0f, sy = h ? float(g_pixel_h.load()) / float(h) : 1.0f;
    return {x * sx, y * sy};
}

// a controller's buttons and sticks as ImGui's gamepad keys, from port 1's
void Gamepad(ImGuiIO& io) {
    SDL_Gamepad* pad = g_nav_pad ? g_nav_pad() : nullptr;
    if (!pad) {
        io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        return;
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    struct Map {
        ImGuiKey key;
        SDL_GamepadButton button;
    };
    constexpr Map kButtons[] = {
        {ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START},       {ImGuiKey_GamepadBack, SDL_GAMEPAD_BUTTON_BACK},
        {ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST},     {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH},      {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},    {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},  {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {ImGuiKey_GamepadL3, SDL_GAMEPAD_BUTTON_LEFT_STICK},     {ImGuiKey_GamepadR3, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    };
    for (const Map& m : kButtons) io.AddKeyEvent(m.key, SDL_GetGamepadButton(pad, m.button));
    // sticks and triggers past a dead zone, as an analog key from 0 to 1
    auto analog = [&](ImGuiKey key, SDL_GamepadAxis axis, float from, float to) {
        float v = (float(SDL_GetGamepadAxis(pad, axis)) - from) / (to - from);
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        io.AddKeyAnalogEvent(key, v > 0.1f, v);
    };
    constexpr float kDead = 8000;
    analog(ImGuiKey_GamepadL2, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0f, 32767.0f);
    analog(ImGuiKey_GamepadR2, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f, 32767.0f);
    analog(ImGuiKey_GamepadLStickLeft, SDL_GAMEPAD_AXIS_LEFTX, -kDead, -32768.0f);
    analog(ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX, kDead, 32767.0f);
    analog(ImGuiKey_GamepadLStickUp, SDL_GAMEPAD_AXIS_LEFTY, -kDead, -32768.0f);
    analog(ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY, kDead, 32767.0f);
    analog(ImGuiKey_GamepadRStickLeft, SDL_GAMEPAD_AXIS_RIGHTX, -kDead, -32768.0f);
    analog(ImGuiKey_GamepadRStickRight, SDL_GAMEPAD_AXIS_RIGHTX, kDead, 32767.0f);
    analog(ImGuiKey_GamepadRStickUp, SDL_GAMEPAD_AXIS_RIGHTY, -kDead, -32768.0f);
    analog(ImGuiKey_GamepadRStickDown, SDL_GAMEPAD_AXIS_RIGHTY, kDead, 32767.0f);
}

}  // namespace

void ImGuiInit(SDL_Window* window) {
    g_imgui_window = window;
    g_last = std::chrono::steady_clock::now();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "xbe_sdl3";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
}

void ImGuiEvent(const SDL_Event& e) {
    if (!ImGui::GetCurrentContext() || !g_imgui_window) return;
    ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            ImVec2 p = Pixels(g_imgui_window, e.motion.x, e.motion.y);
            io.AddMousePosEvent(p.x, p.y);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int button = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1
                         : e.button.button == SDL_BUTTON_MIDDLE                                       ? 2
                                                                                                      : -1;
            if (button >= 0) io.AddMouseButtonEvent(button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            io.AddMouseWheelEvent(-e.wheel.x, e.wheel.y);
            break;
        case SDL_EVENT_TEXT_INPUT:
            io.AddInputCharactersUTF8(e.text.text);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            io.AddKeyEvent(ImGuiMod_Ctrl, (e.key.mod & SDL_KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (e.key.mod & SDL_KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (e.key.mod & SDL_KMOD_ALT) != 0);
            io.AddKeyEvent(ImGuiMod_Super, (e.key.mod & SDL_KMOD_GUI) != 0);
            if (ImGuiKey k = Key(e.key.key); k != ImGuiKey_None) io.AddKeyEvent(k, down);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            io.AddFocusEvent(true);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            io.AddFocusEvent(false);
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            break;
        default:
            break;
    }
}

void ImGuiNewFrame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(g_pixel_w.load()), float(g_pixel_h.load()));
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last).count();
    io.DeltaTime = dt > 0 ? dt : 1.0f / 60.0f;
    g_last = now;
    if (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) Gamepad(io);
    // the frame before's cursor, which the main loop shows: none when ImGui draws its own or wants none
    ImGuiMouseCursor c = ImGui::GetMouseCursor();
    g_cursor = io.MouseDrawCursor || c == ImGuiMouseCursor_None ? -1 : int(c);
}

void ApplyCursor() {
    static SDL_Cursor* cursors[ImGuiMouseCursor_COUNT] = {};
    static int shown = -2;
    int want = g_cursor;
    if (want == shown) return;
    shown = want;
    if (want < 0) {
        SDL_HideCursor();
        return;
    }
    static const SDL_SystemCursor kSystem[ImGuiMouseCursor_COUNT] = {
        SDL_SYSTEM_CURSOR_DEFAULT,     SDL_SYSTEM_CURSOR_TEXT,      SDL_SYSTEM_CURSOR_MOVE,
        SDL_SYSTEM_CURSOR_NS_RESIZE,   SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE,
        SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_POINTER,   SDL_SYSTEM_CURSOR_NOT_ALLOWED};
    if (want < ImGuiMouseCursor_COUNT) {
        if (!cursors[want]) cursors[want] = SDL_CreateSystemCursor(kSystem[want]);
        SDL_SetCursor(cursors[want]);
    }
    SDL_ShowCursor();
}

}  // namespace linuxapp
#endif
