#include "common/modern_controls.h"
#include "app/app.h"
#include "common/release.h"
#include "core/input.h"

void KeyboardWindow(void* hwnd);              // keyboard.cpp
void KeyboardPad(int port, input::Pad& pad);  // keyboard.cpp
void NoblePad(int port, input::Pad& pad);  // android_native.cpp (keyboard + touch)
void KeyboardRumble(int port, uint16_t& left, uint16_t& right);  // keyboard.cpp
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // menu.cpp
void MenuUi(double fps);                      // menu.cpp
extern bool g_fps_counter;                    // menu.cpp
extern bool g_pause_on_focus_loss;            // menu.cpp
extern uint32_t g_menu_key;                   // menu.cpp
extern bool g_launcher_keep_open;             // menu.cpp
extern bool g_texture_pack;                   // texpack.cpp
extern uint32_t g_texture_pack_size;          // texpack.cpp
extern bool g_texture_pack_materials;         // texpack.cpp
extern uint32_t g_button_layout;              // texpack.cpp
extern bool g_lament_save;                    // menu.cpp, read by hooks.cpp
extern bool g_ps2_stick;                      // menu.cpp, read by hooks.cpp and keyboard.cpp
extern bool g_invert_camera_x;                // menu.cpp, read by keyboard.cpp
extern bool g_invert_camera_y;                // menu.cpp, read by keyboard.cpp
extern uint32_t g_fov;                        // menu.cpp, read by game/view.cpp: percent of the game's
extern uint32_t g_draw_distance;              // menu.cpp, read by game/view.cpp
extern uint32_t g_fog_distance;               // menu.cpp, read by game/view.cpp
extern bool g_hud_edges;                      // menu.cpp, read by widescreen.cpp
extern bool g_menu_spread;                    // widescreen.cpp
extern uint32_t g_sprint;                     // keyboard.cpp, read by game/hooks.cpp
extern uint32_t g_speed_up;                   // keyboard.cpp
extern bool g_hd_movies;                      // movies.cpp
extern uint32_t g_movie_view;                 // movies.cpp
extern uint32_t g_surface_depth;              // menu.cpp, read by the SDK's renderer (gpu::g_surface_depth)
extern uint32_t g_antialias;                  // menu.cpp, read by the SDK's renderer (gpu::g_antialias)
extern uint32_t g_upscale;                    // menu.cpp, read by the SDK's renderer (gpu::g_upscale)
extern uint32_t g_sharpening;                 // menu.cpp, read by the SDK's renderer (gpu::g_aa_sharpness)
extern uint32_t g_ambient_occlusion;          // menu.cpp, read by the SDK's renderer (gpu::g_ambient_occlusion)
extern uint32_t g_ray_tracing;                // menu.cpp, read by the SDK's renderer (gpu::g_ray_tracing)
extern uint32_t g_rt_lighting;                // menu.cpp, read by the SDK's renderer (gpu::g_rt_lighting_strength)
extern uint32_t g_rt_quality;                 // menu.cpp, read by the SDK's renderer (gpu::g_rt_quality)
void GameInit();                              // setup.cpp
bool GameIcon(const std::vector<uint8_t>& xbe, std::vector<uint8_t>& bgra, int& size);  // icon.cpp
extern const app::GameInfo::Release kReleases[];  // each release's xbe and executable, written by CMake
extern const unsigned kReleaseCount;

namespace {

// The European release's language, as the console's (XGetLanguage): 1 English, 3 German, 4 French, 5 Spanish,
// 6 Italian, which it loads its text and pictures by; the launcher's Game page sets it. The USA release has only
// English (its disc's other language files are empty), so it plays English whatever the setting.
uint32_t g_game_language = 1;
uint32_t Language() { return cv::kEurope ? g_game_language : 1; }

}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Noble Archimedes",  // window and splash
    .short_title = L"Noble Archimedes",        // message boxes
    .name = L"Castlevania",               // window classes, crash dumps, handover event
    .log = "castlevania",                 // log file prefix
    .settings = "castlevania.toml",       // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", CV_PICK("NobleArchimedes", "NobleArchimedesEU")}},  // each recompiled xbe and its exe
    .releases = {kReleases, kReleases + kReleaseCount},  // the other region's xbe starts its executable instead
    .options = {{"fps_counter", &g_fps_counter, nullptr},  // game settings kept in the settings file
                {"pause_on_focus_loss", &g_pause_on_focus_loss, nullptr},
                {"menu_key", nullptr, &g_menu_key},
                {"launcher_keep_open", &g_launcher_keep_open, nullptr},
                {"texture_pack", &g_texture_pack, nullptr},
                {"texture_pack_size", nullptr, &g_texture_pack_size},
                {"texture_pack_materials", &g_texture_pack_materials, nullptr},
                {"button_layout", nullptr, &g_button_layout},
                {"lament_save", &g_lament_save, nullptr},
                {"game_language", nullptr, &g_game_language},
                {"ps2_stick", &g_ps2_stick, nullptr},
                {"invert_camera_x", &g_invert_camera_x, nullptr},
                {"invert_camera_y", &g_invert_camera_y, nullptr},
                {"fov", nullptr, &g_fov},
                {"draw_distance", nullptr, &g_draw_distance},
                {"fog_distance", nullptr, &g_fog_distance},
                {"hud_edges", &g_hud_edges, nullptr},
                {"menu_spread", &g_menu_spread, nullptr},
                {"sprint", nullptr, &g_sprint},
                {"speed_up", nullptr, &g_speed_up},
                {"hd_movies", &g_hd_movies, nullptr},
                {"movie_view", nullptr, &g_movie_view},
                {"surface_depth", nullptr, &g_surface_depth},
                {"antialiasing", nullptr, &g_antialias},
                {"upscaling", nullptr, &g_upscale},
                {"sharpening", nullptr, &g_sharpening},
                {"ambient_occlusion", nullptr, &g_ambient_occlusion},
                {"ray_tracing", nullptr, &g_ray_tracing},
                {"ray_tracing_lighting", nullptr, &g_rt_lighting},
                {"ray_tracing_quality", nullptr, &g_rt_quality}},
    .load = modern::LoadSettings,                      // reads more from the settings file's toml::table
    .save = modern::SaveSettings,                      // writes more into it
    .init = GameInit,                     // after the settings load, before the window and boot: game files, game code, renderer extension
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = KeyboardWindow,             // the game window is up: keyboard focus and the menu's font
    .message = MenuMessage,               // window messages first; true takes one: F10 for the menu
    .ui = MenuUi,                         // ImGui drawing each frame: the settings menu and FPS counter
    .pad = NoblePad,                   // changes a polled pad: keyboard play, CV_PAD_SCRIPT, pause, menu
    .rumble = KeyboardRumble,             // changes a rumble before it is set: CV_RUMBLE_TRACE
    .language = Language,                 // the console's language the game reads
    .icon = nullptr,                     // the windows' icon without the exe's own (a release): the crest (icon.cpp)
};
