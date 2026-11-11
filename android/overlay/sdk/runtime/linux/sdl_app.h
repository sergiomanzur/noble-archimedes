// Linux/Android: the window, its pixel size and SDL's ImGui platform side, shared by the runtime's sources.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

namespace linuxapp {

// the game window and the size in pixels of the picture drawn into it; the main thread sets both
inline std::atomic<SDL_Window*> g_window{nullptr};
inline std::atomic<uint32_t> g_pixel_w{1280};
inline std::atomic<uint32_t> g_pixel_h{720};
inline std::atomic<bool> g_focused{false};
// the cursor ImGui asked for: -1 hidden, otherwise an ImGuiMouseCursor
inline std::atomic<int> g_cursor{-1};
// the controller ImGui's gamepad navigation reads, when the host has one
inline SDL_Gamepad* (*g_nav_pad)() = nullptr;

std::string Utf8(const wchar_t* text);
inline std::string Utf8(const std::wstring& text) { return Utf8(text.c_str()); }
std::wstring Wide(const std::string& text);

int VirtualKey(SDL_Scancode sc);

void ImGuiInit(SDL_Window* window);
void ImGuiEvent(const SDL_Event& event);
void ImGuiNewFrame();
void ApplyCursor();

}  // namespace linuxapp
