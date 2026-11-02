// The in-game settings menu (F10) and the FPS counter, drawn with ImGui over the game.
//
// Every change applies at once and is saved to the settings file. While the menu is open the game gets no input:
// the keyboard and controller drive the menu instead.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/modern_controls.h"

// game settings beside the runtime's, in the settings file (game_info.cpp lists them)
bool g_fps_counter = false;
bool g_pause_on_focus_loss = true;
uint32_t g_menu_key = VK_F10;  // virtual key that opens the menu
bool g_launcher_keep_open = false;  // the launcher's, kept when the game writes the file
bool g_lament_save = false;  // as with a Lament of Innocence save on the PS2's memory card (hooks.cpp)
extern uint32_t g_button_layout;    // texpack.cpp

namespace {

std::atomic<bool> g_open{false};
bool g_scaled = false;

std::string KeyName(uint32_t vk);  // the key as the keyboard labels it

struct Res {
    uint32_t w, h;
};

// the window's monitor's modes, largest first, with 0x0 for the desktop's own
std::vector<Res> Resolutions() {
    std::vector<Res> list{{0, 0}};
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
        if (dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
        bool seen = false;
        for (const Res& r : list) seen |= r.w == dm.dmPelsWidth && r.h == dm.dmPelsHeight;
        if (!seen) list.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    }
    std::sort(list.begin() + 1, list.end(), [](Res a, Res b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return list;
}

std::string ResName(Res r) {
    if (!r.w) {
        app::Mode d = app::Desktop();
        return "Desktop (" + std::to_string(d.width) + "x" + std::to_string(d.height) + ")";
    }
    return std::to_string(r.w) + "x" + std::to_string(r.h);
}

// a combo over a list of values; true when the choice changed
template <typename T>
bool Choice(const char* label, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    size_t cur = 0;
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == value) cur = i;
    bool changed = false;
    if (ImGui::BeginCombo(label, names[cur].c_str())) {
        for (size_t i = 0; i < values.size(); i++) {
            if (ImGui::Selectable(names[i].c_str(), i == cur) && i != cur) {
                value = values[i];
                changed = true;
            }
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void Row(const char* name, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
    return true;
}

void DisplayTab(bool& save) {
    app::Settings& s = app::Get();
    if (!BeginRows("display")) return;
    Row("Display mode", nullptr);
    if (Choice("##mode", s.display_mode,
               {app::DisplayMode::Fullscreen, app::DisplayMode::Borderless, app::DisplayMode::Windowed},
               {"Fullscreen", "Borderless", "Windowed"})) {
        app::ApplyDisplay();
        save = true;
    }
    Row("Resolution", "The output size. Fullscreen switches the monitor to it; borderless always fills the monitor.");
    static std::vector<Res> modes = Resolutions();
    {
        size_t cur = 0;
        for (size_t i = 0; i < modes.size(); i++)
            if (modes[i].w == s.width && modes[i].h == s.height) cur = i;
        if (ImGui::BeginCombo("##res", ResName(modes[cur]).c_str())) {
            for (size_t i = 0; i < modes.size(); i++)
                if (ImGui::Selectable(ResName(modes[i]).c_str(), i == cur) && i != cur) {
                    s.width = modes[i].w;
                    s.height = modes[i].h;
                    app::ApplyDisplay();
                    save = true;
                }
            ImGui::EndCombo();
        }
    }
    // no frame rate cap here: the game's logic steps once a frame, so it is held at the 60 it was made for
    Row("V-Sync", "Off can tear, and lowers input latency a little.");
    save |= ImGui::Checkbox("##vsync", &s.vsync);
    ImGui::EndTable();
}

void GraphicsTab(bool& save) {
    app::Settings& s = app::Get();
    if (!BeginRows("graphics")) return;
    Row("Render scale", "Of the output height. Above 100% supersamples for a sharper, smoother picture.");
    save |= Choice<uint32_t>("##scale", s.render_percent, {50, 75, 100, 150, 200},
                             {"50%", "75%", "100%", "150%", "200%"});
    Row("Anisotropic filtering", "Sharper textures at an angle.");
    save |= Choice<uint32_t>("##aniso", s.aniso, {0, 2, 4, 8, 16}, {"Off", "2x", "4x", "8x", "16x"});
    Row("Widescreen", "16:9 with more of the world at the sides; the HUD, menus and movies stay 4:3 in the middle. "
                      "Off shows the game 4:3, as on a 4:3 TV. Needs a 16:9 or wider resolution.");
    save |= ImGui::Checkbox("##wide", &s.widescreen);
    ImGui::EndTable();
}

void GameTab(bool& save) {
    if (!BeginRows("game")) return;
    Row("FPS counter", nullptr);
    save |= ImGui::Checkbox("##fpsc", &g_fps_counter);
    Row("Pause on focus loss", "Stops the game while another window is in front.");
    save |= ImGui::Checkbox("##pause", &g_pause_on_focus_loss);
    Row("Lament of Innocence save", "As the PS2 game with a Lament of Innocence save on the memory card: a Moai "
                                    "at the far end of the room behind the first courtyard, beside the potion.");
    save |= ImGui::Checkbox("##lament", &g_lament_save);
    ImGui::EndTable();
}

void ControlsTab(bool& save) {
    if (BeginRows("layout")) {
        Row("Button layout", "PS2: the PS2's button icons (from an HD texture pack made from the PS2 game) and its "
                             "shoulders: L1 resets the camera, R1 guards, L2 and R2 lock on. From the next start.");
        save |= Choice<uint32_t>("##layout", g_button_layout, {0, 1}, {"Xbox", "PS2"});
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Xbox, PlayStation and Switch controllers work as player 1. The keyboard plays alongside:");
    ImGui::Spacing();
    static const char* kKeys[][2] = {
        {"W A S D", "Move"},           {"Mouse", "Look (captured)"}, {"Arrow keys", "Camera"},
        {"K / Space", "A (jump)"},     {"J / Left mouse", "X (attack)"}, {"L", "B"},
        {"I / Middle mouse", "Y"},     {"Right mouse (hold)", "Aim / slow walk"},
        {"Q / E", "Left / right trigger"}, {"Z / C", "White / black"}, {"Enter", "Start"},
        {"Backspace", "Back"},         {"Numpad 8 2 4 6", "D-pad"},
        {"F5 / F6", "Camera mode / release mouse"}, {"L3 + R3 (controller)", "This menu"},
    };
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(KeyName(g_menu_key).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("This menu (change it in the launcher)");
        for (auto& k : kKeys) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k[1]);
        }
        ImGui::EndTable();
    }
}

void CameraTab(bool& save) {
    ImGui::TextWrapped("Mouse look rotates the selected camera directly, so it stops as soon as the mouse stops. "
                       "The title camera still supplies the player target and a safe distance; over-the-shoulder "
                       "orbits around Hector, and first person sits at his eye line. Classic leaves the original "
                       "camera alone.");
    ImGui::Spacing();
    save |= modern::DrawSettings();
}

void Defaults() {
    app::Settings d;
    app::Settings& s = app::Get();
    d.join_ip = s.join_ip;
    s = d;
    g_fps_counter = false;
    g_pause_on_focus_loss = true;
    g_button_layout = 0;
    g_lament_save = false;
    modern::ResetDefaults();
    app::ApplyDisplay();
}

std::string KeyName(uint32_t vk) {
    wchar_t name[64] = {};
    LONG scan = LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    // keys of the navigation block share scan codes with the numpad: the extended bit tells them apart
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) scan |= 1 << 24;
    if (!GetKeyNameTextW(scan, name, 64)) return "F10";
    char out[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

bool Foreground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return fg && pid == GetCurrentProcessId();
}

bool Down(uint32_t vk) { return (GetAsyncKeyState(int(vk)) & 0x8000) != 0; }

// render thread, each frame: the menu key's state is read rather than its messages, which an overlay or another
// program's hotkey can take before the window sees them
void PollKeys() {
    static bool menu_was = false, escape_was = false;
    bool fg = Foreground();
    bool menu = fg && Down(g_menu_key ? g_menu_key : VK_F10);
    if (menu && !menu_was) g_open = !g_open;
    menu_was = menu;
    bool escape = fg && Down(VK_ESCAPE);
    if (escape && !escape_was) g_open = false;
    escape_was = escape;
}

void Style(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6;
    st.FrameRounding = 4;
    st.TabRounding = 4;
    st.WindowPadding = {14, 12};
    st.FramePadding = {8, 5};
    st.ItemSpacing = {10, 8};
    ImVec4* c = st.Colors;
    // Castlevania red over near black
    ImVec4 red{0.55f, 0.08f, 0.10f, 1}, red_hi{0.72f, 0.12f, 0.14f, 1}, red_lo{0.40f, 0.06f, 0.08f, 1};
    c[ImGuiCol_WindowBg] = {0.06f, 0.05f, 0.06f, 0.94f};
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = red_lo;
    c[ImGuiCol_Tab] = red_lo;
    c[ImGuiCol_TabHovered] = red_hi;
    c[ImGuiCol_TabSelected] = red;
    c[ImGuiCol_Button] = red_lo;
    c[ImGuiCol_ButtonHovered] = red_hi;
    c[ImGuiCol_ButtonActive] = red;
    c[ImGuiCol_FrameBg] = {0.16f, 0.13f, 0.14f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.26f, 0.16f, 0.18f, 1};
    c[ImGuiCol_FrameBgActive] = {0.32f, 0.18f, 0.20f, 1};
    c[ImGuiCol_Header] = red_lo;
    c[ImGuiCol_HeaderHovered] = red_hi;
    c[ImGuiCol_HeaderActive] = red;
    c[ImGuiCol_CheckMark] = {0.95f, 0.80f, 0.45f, 1};
    c[ImGuiCol_NavHighlight] = {0.95f, 0.80f, 0.45f, 1};
    st.ScaleAllSizes(scale);
}

}  // namespace

bool MenuOpen() { return g_open; }
void MenuToggle() { g_open = !g_open; }

// the game window is up, before the first frame: the font, sized for the monitor
void MenuWindow() {
    std::lock_guard lock(app::UiLock());
    float scale = std::max(1.0f, app::Desktop().height / 1080.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 20.0f * scale)) {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&cfg);
    }
    Style(scale);
    g_scaled = true;
}

// window thread: the menu key and Escape are taken here so the host does nothing with them (F10 alone would put
// the window in menu mode); PollKeys opens and closes the menu
extern std::atomic<bool> g_closing;  // keyboard.cpp

bool MenuMessage(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (msg == WM_CLOSE) {
        g_closing = true;
        modern::Shutdown();
    }
    bool key = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP;
    if (key && (wp == VK_F10 || wp == g_menu_key || (wp == VK_ESCAPE && g_open))) {
        *result = 0;
        return true;
    }
    return false;
}

void MenuUi(double fps) {
    if (!g_scaled) return;
    PollKeys();
    modern::SetMenuOpen(g_open);
    ImGuiIO& io = ImGui::GetIO();
    if (g_fps_counter) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f FPS", fps);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 at{io.DisplaySize.x * 0.01f, io.DisplaySize.y * 0.01f};
        dl->AddText({at.x + 2, at.y + 2}, IM_COL32(0, 0, 0, 200), text);
        dl->AddText(at, IM_COL32(255, 230, 140, 255), text);
    }
    io.MouseDrawCursor = false;
    if (!g_open) return;

    ImVec2 size{io.DisplaySize.y * 0.75f, io.DisplaySize.y * 0.55f};
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    bool open = true;
    if (ImGui::Begin("Castlevania: Curse of Darkness - Settings", &open,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings)) {
        bool save = false;
        float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("tabs", {0, -footer})) {
            if (ImGui::BeginTabBar("tabbar")) {
                if (ImGui::BeginTabItem("Display")) {
                    DisplayTab(save);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics")) {
                    GraphicsTab(save);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Game")) {
                    GameTab(save);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls")) {
                    ControlsTab(save);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Camera")) {
                    CameraTab(save);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Reset to defaults")) {
            Defaults();
            save = true;
        }
        ImGui::SameLine();
        std::string close = "Close (" + KeyName(g_menu_key) + ")";
        float close_w = ImGui::CalcTextSize(close.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - close_w);
        if (ImGui::Button(close.c_str())) open = false;
        if (save) app::Save();
    }
    ImGui::End();
    if (!open) g_open = false;
}
