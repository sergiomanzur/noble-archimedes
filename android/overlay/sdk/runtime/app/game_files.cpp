// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef _WIN32
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cwctype>
#include <fstream>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake
// its certificate's title name (80 bytes), then allowed media, region and ratings (12)
extern const unsigned char kImageCertificate[92];

namespace app {

namespace {

namespace fs = std::filesystem;

// Copies of a release often have their certificate's title name, allowed media and region patched (for a hard disk
// loader, or for any region's console), the signed sections left alone. Put back as the release with this SHA-1 has
// them, such a copy is that release, and the game runs that: true with the xbe restored.
bool RestoreCertificate(std::vector<uint8_t>& xbe, const char* sha1, const unsigned char* certificate) {
    if (xbe.size() < 0x11C) return false;
    uint32_t base, cert;
    memcpy(&base, &xbe[0x104], 4);
    memcpy(&cert, &xbe[0x118], 4);
    if (cert < base || uint64_t(cert - base) + 0xA8 > xbe.size()) return false;
    std::vector<uint8_t> copy = xbe;
    memcpy(&copy[cert - base + 0x0C], certificate, 80);
    memcpy(&copy[cert - base + 0x9C], certificate + 80, 12);
    if (Sha1(copy) != sha1) return false;
    xbe = std::move(copy);
    return true;
}

// the game's other release this xbe is, as GameInfo::releases names them, with its executable beside this one
const GameInfo::Release* OtherRelease(const std::vector<uint8_t>& xbe, const std::string& sha) {
    for (const GameInfo::Release& release : kGame.releases) {
        std::vector<uint8_t> copy = xbe;
        std::error_code ec;
        if ((sha == release.sha1 || RestoreCertificate(copy, release.sha1, release.certificate)) &&
            fs::exists(ExeDir() / ExeFile(release.exe), ec))
            return &release;
    }
    return nullptr;
}

// The release the settings choose (Settings::release), when it is another executable's: started by hand, or by a
// shortcut to this executable, the game plays the release chosen. Not once handed over, which a choice of this
// executable's release also skips.
const GameInfo::Release* ChosenRelease() {
#ifdef _WIN32
    if (std::wcsstr(GetCommandLineW(), L"--handed-over")) return nullptr;
#else
    for (int i = 1; i < Argc(); i++)
        if (!std::strcmp(Argv()[i], "--handed-over")) return nullptr;
#endif
    const std::string& exe = Get().release;
    for (const GameInfo::Release& release : kGame.releases)
        if (!exe.empty() && exe == release.exe) return std::strcmp(release.sha1, kImageSha1) ? &release : nullptr;
    return nullptr;
}

// That release's executable with this one's arguments, for as long as it runs: this process ends with its exit code,
// so whatever started the game (a launcher, Steam) sees it run until it ends. Returns when it could not start.
void RunRelease(const GameInfo::Release& release) {
#if defined(__ANDROID__)
    // Android ships one APK per release; there is no sibling executable to hand over to.
    LOGE("game: another release's executable is not available on Android (%s)", release.exe);
#elif !defined(_WIN32)
    std::string exe = (ExeDir() / ExeFile(release.exe)).string();
    std::vector<char*> args{exe.data()};
    for (int i = 1; i < Argc(); i++) args.push_back(Argv()[i]);
    std::string handed = "--handed-over";  // its log carries on this one's (linux/main.cpp)
    args.push_back(handed.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (int e = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args.data(), environ)) {
        LOGE("game: cannot start %s (%s)", release.exe, std::strerror(e));
        return;
    }
    LOGR("Game: another release's, handed to %s", release.exe);
    logx::Flush();
    // asked to end (a desktop or Steam closing the game it started): that release is asked to, as it plays the game
    static pid_t child = 0;
    child = pid;
    struct sigaction pass{};
    pass.sa_handler = [](int sig) { kill(child, sig); };
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) sigaction(sig, &pass, nullptr);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::_Exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
#else
    fs::path exe = ExeDir() / (std::string(release.exe) + ".exe");
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + exe.wstring() + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    line += L" --handed-over";  // its log carries on this one's (main.cpp)
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        LOGE("game: cannot start %s.exe (%lu)", release.exe, GetLastError());
        return;
    }
    LOGR("Game: another release's, handed to %s.exe", release.exe);
    logx::Flush();
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
#endif
}

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool FindGame(GameFiles& game) {
    fs::path assets = FindAssets();
    std::vector<fs::path> isos;
    std::error_code ec;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (ext == L".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    std::sort(isos.begin(), isos.end());

    // a copy of the release the settings choose, another executable's: that executable plays it
    if (const GameInfo::Release* chosen = ChosenRelease()) {
        auto of_chosen = [&](const std::vector<uint8_t>& xbe) { return OtherRelease(xbe, Sha1(xbe)) == chosen; };
        std::vector<uint8_t> xbe;
        bool found = !assets.empty() && ReadHostFile(assets / IMAGE_XBE, xbe) && of_chosen(xbe);
        for (size_t i = 0; i < isos.size() && !found; i++) {
            kernel::Disc disc;
            std::string error;
            found = disc.Open(isos[i], error) && disc.Load(IMAGE_XBE, xbe) && of_chosen(xbe);
        }
        LOGR("Game: the settings choose %s, %s", chosen->exe, found ? "a copy of it is here" : "with no copy here");
        if (found) RunRelease(*chosen);  // returns only when it could not start
    }

    // the first copy with this xbe wins, the extracted files before any image; else the first of a release another
    // executable was made from runs there; another release is kept to offer
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    const GameInfo::Release* release = nullptr;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc) {
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        bool restored = !match && RestoreCertificate(xbe, kImageSha1, kImageCertificate);
        const GameInfo::Release* known = match || restored ? nullptr : OtherRelease(xbe, sha);
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(),
             match      ? ""
             : restored ? ", this release with its certificate patched, put back"
             : known    ? ", the release of another executable"
                        : ", another release");
        if (match || restored) {
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (known && !release) release = known;
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    if (!assets.empty() && ReadHostFile(assets / IMAGE_XBE, xbe) && consider(assets, std::move(xbe), nullptr)) return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc))) return true;
    }

    if (release) RunRelease(*release);  // returns only when it could not start
    if (other_name.empty()) {
        LOGR("Game: no copy of the game in %s", assets.empty() ? "a folder named assets" : assets.string().c_str());
        // tests (XBE_BACKGROUND): the message in the log, no window taking the focus
        if (Background()) LOGR("message box: %ls", kGame.missing);
        else MessageBoxW(nullptr, kGame.missing, kGame.short_title, MB_ICONERROR);
        return false;
    }
    std::wstring text = other_name.filename().wstring() +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    if (Background()) {  // tests: the question in the log, answered as its default button (No)
        LOGR("message box: %ls (No)", text.c_str());
        return false;
    }
    if (MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return false;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app
