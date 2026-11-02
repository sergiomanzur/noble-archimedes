#include "app/app.h"
#include "common/release.h"
#include "core/input.h"

void KeyboardWindow(void* hwnd);              // keyboard.cpp
void KeyboardPad(int port, input::Pad& pad);  // keyboard.cpp
bool MenuMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);  // menu.cpp
void MenuUi(double fps);                      // menu.cpp
extern bool g_fps_counter;                    // menu.cpp
extern bool g_pause_on_focus_loss;            // menu.cpp
extern uint32_t g_menu_key;                   // menu.cpp
extern bool g_launcher_keep_open;             // menu.cpp
extern bool g_texture_pack;                   // texpack.cpp
extern uint32_t g_texture_pack_size;          // texpack.cpp
extern uint32_t g_button_layout;              // texpack.cpp
extern bool g_lament_save;                    // menu.cpp, read by hooks.cpp
void GameInit();                              // setup.cpp
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
    .title = L"Castlevania: Curse of Darkness",  // window and splash
    .short_title = L"Castlevania",        // message boxes
    .name = L"Castlevania",               // window classes, crash dumps, handover event
    .log = "castlevania",                 // log file prefix
    .settings = "castlevania.toml",       // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", CV_PICK("Castlevania", "CastlevaniaEU")}},  // each recompiled xbe and its exe
    .releases = {kReleases, kReleases + kReleaseCount},  // the other region's xbe starts its executable instead
    .options = {{"fps_counter", &g_fps_counter, nullptr},  // game settings kept in the settings file
                {"pause_on_focus_loss", &g_pause_on_focus_loss, nullptr},
                {"menu_key", nullptr, &g_menu_key},
                {"launcher_keep_open", &g_launcher_keep_open, nullptr},
                {"texture_pack", &g_texture_pack, nullptr},
                {"texture_pack_size", nullptr, &g_texture_pack_size},
                {"button_layout", nullptr, &g_button_layout},
                {"lament_save", &g_lament_save, nullptr},
                {"game_language", nullptr, &g_game_language}},
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = GameInit,                     // after the settings load, before the window and boot: game files, game code, renderer extension
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = KeyboardWindow,             // the game window is up: keyboard focus and the menu's font
    .message = MenuMessage,               // window messages first; true takes one: F10 for the menu
    .ui = MenuUi,                         // ImGui drawing each frame: the settings menu and FPS counter
    .pad = KeyboardPad,                   // changes a polled pad: keyboard play, CV_PAD_SCRIPT, pause, menu
    .rumble = nullptr,                    // changes a rumble before it is set
    .language = Language,                 // the console's language the game reads
};
