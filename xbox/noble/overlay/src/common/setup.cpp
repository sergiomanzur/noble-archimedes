// First start: the game files. No game found: the assets folder is made beside the exe and opened, and the player is
// told what goes in it. (A release's game code is the SDK's: runtime/app/game_code.cpp builds Castlevania.dll from
// the player's own default.xbe on the first start.)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

namespace {

namespace fs = std::filesystem;

void Fail(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), app::kGame.short_title, MB_ICONWARNING | MB_SETFOREGROUND);
    ExitProcess(1);
}

// the runtime's search: assets beside the exe or in a folder above it
fs::path FindAssets() {
    fs::path dir = app::ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

bool HasImage(const fs::path& assets) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(assets, ec)) {
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
        if (ext == L".iso" && e.is_regular_file(ec)) return true;
    }
    return false;
}

// nothing to play: an assets folder to put the game in, opened for the player
void AskForGame() {
    fs::path assets = FindAssets();
    std::error_code ec;
    if (!assets.empty() && (fs::exists(assets / IMAGE_XBE, ec) || HasImage(assets))) return;
    if (assets.empty()) {
        assets = app::ExeDir() / "assets";
        fs::create_directories(assets, ec);
    }
    LOGR("setup: no game in %s, asking for it", assets.string().c_str());
    ShellExecuteW(nullptr, L"open", assets.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    Fail(L"Put your copy of Castlevania: Curse of Darkness (USA, original Xbox) in the assets folder that just "
         L"opened:\n\n  - its disc image (.iso), or\n  - the extracted disc: default.xbe with its data, movie and "
         L"sound folders.\n\nThen start the game again.");
}

}  // namespace

void WidescreenInit();    // widescreen.cpp
void MoviesInit();
void TexturePackInit();   // texpack.cpp

// GameInfo::init: settings loaded, nothing of the game touched yet
void GameInit() {
    AskForGame();
    TexturePackInit();
    MoviesInit();
    WidescreenInit();
}
