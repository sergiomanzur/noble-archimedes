// The launcher: checks the game files, edits the settings file the game reads, and starts the game.
//
// It sits beside NobleArchimedes.exe and writes the same castlevania.toml, keeping any keys it does not know. The game
// files are checked as the game checks them: assets beside the exe or in a folder above it, and default.xbe there or
// on a disc image (.iso) in it must have the SHA-1 the exe was recompiled from, once a patched certificate is put back
// (the SDK's runtime/app/game_files.cpp).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "kernel/disc.h"
#include "words.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
// each release's xbe (its SHA-1, its certificate's title name, media, region and ratings) and the executable it was
// recompiled into, written by CMake: the USA one first, then the European one
extern const app::GameInfo::Release kReleases[];
extern const unsigned kReleaseCount;

namespace {

namespace fs = std::filesystem;
using words::F;
using words::T;

constexpr char kSettings[] = "castlevania.toml";
constexpr char kXbe[] = "default.xbe";
constexpr wchar_t kSaves[] = L"hdd\\E\\UDATA\\4b4e002d";

// D3D11 and the window

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_activated = false;  // the window came back to the front: the files are looked at again (a pack added meanwhile)

void MakeTarget() {
    ID3D11Texture2D* bb = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;  // 10 bits: no bands in the gradients
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL got;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    // no GPU driver: the software rasterizer still draws a launcher
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    MakeTarget();
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_dev && wp != SIZE_MINIMIZED) {
                if (g_rtv) g_rtv->Release(), g_rtv = nullptr;
                g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // Alt alone
            break;
        case WM_ACTIVATEAPP:
            if (wp) g_activated = true;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// files

fs::path ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

// the game's own search: assets beside the exe or in any folder above it
fs::path FindAssets() {
    fs::path dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Sha1(const std::vector<uint8_t>& data) {
    uint8_t digest[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.data()), ULONG(data.size()), digest, 20) != 0)
        return {};
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 1)) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void Open(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// The graphics the game draws with, as the SDK picks them (runtime/gpu/d3d12.cpp): the card the settings name (gpu)
// if it has Direct3D 12, else the fastest, else the default one, at feature level 11_0. Windows 10 and 11 all have
// Direct3D 12, but only graphics with a driver for it run it. CV_NO_D3D12=1 pretends none does, to see the warning.
struct Gpu {
    std::string name;
    bool d3d12 = false;
    bool software = false;  // Windows' own renderer on the CPU, no graphics driver installed
};

// a null device: only whether one would be made; a null adapter is the default one
bool RunsD3D12(IDXGIAdapter1* a) {
    static const bool none = GetEnvironmentVariableW(L"CV_NO_D3D12", nullptr, 0) != 0;
    return !none && SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

Gpu Describe(IDXGIAdapter1* a, bool d3d12) {
    Gpu g{{}, d3d12};
    DXGI_ADAPTER_DESC1 d{};
    if (a && SUCCEEDED(a->GetDesc1(&d))) {
        g.name = Utf8(d.Description);
        g.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    }
    return g;
}

// the graphics cards to choose from, the fastest first, without Windows' software renderer
std::vector<Gpu> Gpus() {
    std::vector<Gpu> list;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIFactory6> f6;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;
    factory.As(&f6);
    for (UINT i = 0;; i++) {
        HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a))
                        : factory->EnumAdapters1(i, &a);
        if (FAILED(hr)) break;
        Gpu g = Describe(a.Get(), RunsD3D12(a.Get()));
        if (!g.software) list.push_back(g);
    }
    return list;
}

// what the game takes with no card named: the fastest, else the default adapter
Gpu Automatic() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIFactory6> f6;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    if (SUCCEEDED(factory.As(&f6))) f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (RunsD3D12(adapter.Get())) return Describe(adapter.Get(), true);
    bool d3d12 = RunsD3D12(nullptr);  // the game's second try
    adapter.Reset();
    factory->EnumAdapters1(0, &adapter);
    return Describe(adapter.Get(), d3d12);
}

// The game's check: an xbe one of the executables was made from, or that with the certificate's title, media and
// region patched; which release it is (kReleases), and so which executable plays it
enum class Release { Missing, Match, Patched, Other };
struct Copy {
    Release release = Release::Missing;  // Missing: no Xbox disc, or no default.xbe on it
    int which = -1;                       // the release in kReleases
    bool europe() const { return which > 0; }
};

Copy Check(const std::vector<uint8_t>& xbe) {
    std::string sha = Sha1(xbe);
    for (unsigned i = 0; i < kReleaseCount; i++)
        if (sha == kReleases[i].sha1) return {Release::Match, int(i)};
    uint32_t base = 0, cert = 0;
    if (xbe.size() >= 0x11C) {
        memcpy(&base, &xbe[0x104], 4);
        memcpy(&cert, &xbe[0x118], 4);
    }
    if (cert < base || uint64_t(cert - base) + 0xA8 > xbe.size()) return {Release::Other};
    for (unsigned i = 0; i < kReleaseCount; i++) {
        std::vector<uint8_t> copy = xbe;
        memcpy(&copy[cert - base + 0x0C], kReleases[i].certificate, 80);
        memcpy(&copy[cert - base + 0x9C], kReleases[i].certificate + 80, 12);
        if (Sha1(copy) == kReleases[i].sha1) return {Release::Patched, int(i)};
    }
    return {Release::Other};
}

fs::path ReleaseExe(int which) {
    return ExeDir() / (which > 0 ? L"NobleArchimedesEU.exe" : L"NobleArchimedes.exe");
}

// what the game will find
// the textures folder the game looks for, found the same way as assets
fs::path FindTextures() {
    fs::path dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "textures", ec)) return dir / "textures";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

struct Files {
    fs::path assets;
    Copy xbe;
    struct Image {
        fs::path path;
        Copy copy;
    };
    std::vector<Image> isos;
    bool exe = false;  // NobleArchimedes.exe, the USA release's, which the other starts when it has no game of its own
    fs::path textures;
    size_t pictures = 0;      // .png and .dds in textures and the folders inside it
    bool ps2_buttons = false;  // the PS2's button icons among them (texpack.cpp, the PS2 button layout)

    void Scan() {
        textures = FindTextures();
        pictures = 0;
        ps2_buttons = false;
        if (!textures.empty()) {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(textures, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::wstring ext = it->path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
                if (ext != L".png" && ext != L".dds") continue;
                pictures++;
                std::wstring name = it->path().filename().wstring();
                ps2_buttons |= name.rfind(L"27818fa40a1117af-66279948575c23d7-", 0) == 0;
            }
        }
        assets = FindAssets();
        xbe = {};
        isos.clear();
        exe = fs::exists(ReleaseExe(0));
        if (assets.empty()) return;
        std::error_code ec;
        if (std::ifstream f{assets / kXbe, std::ios::binary}) xbe = Check({std::istreambuf_iterator<char>(f), {}});
        for (auto& e : fs::directory_iterator(assets, ec)) {
            std::wstring ext = e.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext != L".iso" || !e.is_regular_file(ec)) continue;
            Image image{e.path()};
            kernel::Disc disc;
            std::string error;
            std::vector<uint8_t> data;
            if (disc.Open(e.path(), error) && disc.Load(kXbe, data)) image.copy = Check(data);
            isos.push_back(image);
        }
        std::sort(isos.begin(), isos.end(), [](const Image& a, const Image& b) { return a.path < b.path; });
    }
    static bool Good(const Copy& c) { return c.release == Release::Match || c.release == Release::Patched; }
    // The executable that plays the game found, as each executable picks its game (the SDK's game_files.cpp): a copy
    // of the USA release anywhere first, the extracted files before a disc image; empty with none, or without it.
    fs::path Player() const {
        for (unsigned which = 0; which < kReleaseCount; which++) {
            bool found = (Good(xbe) && xbe.which == int(which)) ||
                         std::any_of(isos.begin(), isos.end(), [&](const Image& i) { return Good(i.copy) && i.copy.which == int(which); });
            std::error_code ec;
            if (found && fs::exists(ReleaseExe(int(which)), ec)) return ReleaseExe(int(which));
        }
        return {};
    }
    bool Playable() const { return !Player().empty(); }
};

// a disc image into assets: a hard link on the same drive, a copy with progress otherwise
struct Import {
    std::atomic<bool> running{false};
    std::atomic<float> progress{0};
    std::string result;
    std::thread worker;

    static DWORD CALLBACK Progress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD,
                                  HANDLE, HANDLE, LPVOID self) {
        static_cast<Import*>(self)->progress = total.QuadPart ? float(double(done.QuadPart) / double(total.QuadPart)) : 0;
        return PROGRESS_CONTINUE;
    }

    void Start(fs::path from, fs::path to) {
        if (running) return;
        if (worker.joinable()) worker.join();
        running = true;
        progress = 0;
        result.clear();
        worker = std::thread([this, from, to] {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) {
                result = F("Linked {} into assets.", {Utf8(to.filename().wstring())});
            } else if (CopyFileExW(from.c_str(), to.c_str(), Progress, this, nullptr, COPY_FILE_FAIL_IF_EXISTS)) {
                result = F("Copied {} into assets.", {Utf8(to.filename().wstring())});
            } else {
                result = F("Could not copy the disc image (error {}).", {std::to_string(GetLastError())});
            }
            running = false;
        });
    }
    ~Import() {
        if (worker.joinable()) worker.join();
    }
};

// The European release's languages, as the console's (XGetLanguage), each by its name in itself; the USA release
// plays English
struct GameLanguage {
    uint32_t console;
    const char* name;
    const char* launcher;  // the launcher's language that picks it at first (words.h)
};
const GameLanguage kGameLanguages[] = {
    {1, "English", "en"}, {3, "Deutsch", "de"}, {5, "Español", "es"}, {4, "Français", "fr"}, {6, "Italiano", "it"},
};

// a player who reads the launcher in one of the game's languages plays in it, until they choose
uint32_t GameLanguageFor(const std::string& launcher) {
    for (const GameLanguage& g : kGameLanguages)
        if (launcher == g.launcher) return g.console;
    return 1;
}

// settings: the keys the game reads, the rest of the file kept

struct Settings {
    std::string display_mode = "borderless";
    uint32_t width = 0, height = 0;
    uint32_t render_percent = 100;
    uint32_t aniso = 0;
    bool vsync = true;
    bool widescreen = true;
    bool fps_counter = false;
    bool pause_on_focus_loss = true;
    uint32_t menu_key = VK_F10;
    bool launcher_music = true;    // original launcher theme, independent of game audio
    bool keep_open = false;        // the launcher's own
    std::string language = "en";   // the launcher's own too, a code of words.h
    uint32_t game_language = 1;    // the European release's, as the console's (XGetLanguage): kGameLanguages
    bool texture_pack = true;
    uint32_t texture_pack_size = 2048;  // 0 full size
    uint32_t button_layout = 0;         // 0 Xbox, 1 PS2
    bool lament_save = false;           // the Moai of a Lament of Innocence save (hooks.cpp)
    std::string gpu;                    // the graphics card by its name, empty the fastest (the SDK's)
    toml::table rest;

    fs::path File() const { return ExeDir() / kSettings; }

    void Load() {
        try {
            rest = toml::parse_file(File().string());
        } catch (...) {
            rest = {};
        }
        display_mode = rest["display_mode"].value_or(display_mode);
        width = rest["width"].value_or(width);
        height = rest["height"].value_or(height);
        render_percent = rest["render_percent"].value_or(render_percent);
        aniso = rest["anisotropic_filtering"].value_or(aniso);
        vsync = rest["vsync"].value_or(vsync);
        widescreen = rest["widescreen"].value_or(widescreen);
        fps_counter = rest["fps_counter"].value_or(fps_counter);
        pause_on_focus_loss = rest["pause_on_focus_loss"].value_or(pause_on_focus_loss);
        menu_key = rest["menu_key"].value_or(menu_key);
        launcher_music = rest["launcher_music"].value_or(launcher_music);
        keep_open = rest["launcher_keep_open"].value_or(keep_open);
        language = rest["launcher_language"].value_or(language);
        game_language = rest["game_language"].value_or(GameLanguageFor(language));
        texture_pack = rest["texture_pack"].value_or(texture_pack);
        texture_pack_size = rest["texture_pack_size"].value_or(texture_pack_size);
        button_layout = rest["button_layout"].value_or(button_layout);
        lament_save = rest["lament_save"].value_or(lament_save);
        gpu = rest["gpu"].value_or(gpu);
    }

    bool Save() {
        rest.insert_or_assign("display_mode", display_mode);
        rest.insert_or_assign("width", int64_t(width));
        rest.insert_or_assign("height", int64_t(height));
        rest.insert_or_assign("render_percent", int64_t(render_percent));
        rest.insert_or_assign("anisotropic_filtering", int64_t(aniso));
        rest.insert_or_assign("vsync", vsync);
        rest.insert_or_assign("widescreen", widescreen);
        rest.insert_or_assign("fps_counter", fps_counter);
        rest.insert_or_assign("pause_on_focus_loss", pause_on_focus_loss);
        rest.insert_or_assign("menu_key", int64_t(menu_key));
        rest.insert_or_assign("launcher_music", launcher_music);
        rest.insert_or_assign("launcher_keep_open", keep_open);
        rest.insert_or_assign("launcher_language", language);
        rest.insert_or_assign("game_language", int64_t(game_language));
        rest.insert_or_assign("texture_pack", texture_pack);
        rest.insert_or_assign("texture_pack_size", int64_t(texture_pack_size));
        rest.insert_or_assign("button_layout", int64_t(button_layout));
        rest.insert_or_assign("lament_save", lament_save);
        rest.insert_or_assign("gpu", gpu);
        std::ofstream f(File());
        f << rest << "\n";
        return bool(f);
    }

    void Defaults() {
        Settings d;
        d.rest = std::move(rest);
        d.launcher_music = launcher_music;
        d.keep_open = keep_open;
        d.language = language;
        d.game_language = game_language;
        *this = std::move(d);
    }
};

// UI pieces

struct Res {
    uint32_t w, h;
};

std::vector<Res> Resolutions() {
    std::vector<Res> list{{0, 0}};
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
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
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(nullptr, ENUM_REGISTRY_SETTINGS, &dm);
        return F("Desktop ({})", {std::to_string(dm.dmPelsWidth) + "x" + std::to_string(dm.dmPelsHeight)});
    }
    return std::to_string(r.w) + "x" + std::to_string(r.h);
}

std::string KeyName(uint32_t vk) {
    wchar_t name[64] = {};
    LONG scan = LONG(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE) scan |= 1 << 24;
    if (!GetKeyNameTextW(scan, name, 64)) return F("Key {}", {std::to_string(vk)});
    return Utf8(name);
}

// Noble Archimedes: original astronomical artwork, dark teal and aged brass.
float g_scale = 1;
ImFont* g_body = nullptr;     // Palatino 16.5, icons merged
ImFont* g_small = nullptr;    // Palatino 14.5, descriptions
ImFont* g_semi = nullptr;     // Palatino Bold 16.5, icons merged
ImFont* g_italic = nullptr;   // Palatino Bold Italic 18: the sidebar, buttons, chosen options
ImFont* g_head = nullptr;     // Palatino Bold Italic 30, a page's name on its banner
ImFont* g_hero = nullptr;     // Palatino Bold Italic 58, the Noble Archimedes wordmark
ImFont* g_heroSub = nullptr;  // Palatino Bold Italic 28
ImFont* g_button = nullptr;   // Palatino Bold Italic 26, Play

float S(float v) { return v * g_scale; }

ImU32 Rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
// Deep teal panels, brass frames and warm ivory text
const ImU32 kBg = Rgb(4, 15, 19), kCard = Rgb(9, 29, 33, 242), kCardLo = Rgb(5, 20, 25, 246), kCardHi = Rgb(22, 51, 55);
const ImU32 kLine = Rgb(118, 101, 66), kLineHi = Rgb(210, 183, 121), kLineLo = Rgb(48, 67, 65);
const ImU32 kText = Rgb(242, 240, 232), kMuted = Rgb(157, 177, 174);
const ImU32 kTealHi = Rgb(92, 178, 172);
// Brass highlights and supporting status colours
const ImU32 kGoldHi = Rgb(255, 238, 176), kGoldLo = Rgb(150, 100, 34);
const ImU32 kPurple = Rgb(42, 103, 103);
// Brass marks the selected action
const ImU32 kAccent = Rgb(175, 145, 82), kAccentHi = Rgb(230, 205, 147), kAccentLo = Rgb(83, 98, 74);
const ImU32 kGreen = Rgb(86, 234, 124), kRed = Rgb(242, 84, 72), kAmber = Rgb(252, 166, 66);
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }

// Segoe Fluent / MDL2 icons, for status lines and the language
enum : uint32_t {
    kIconCheck = 0xE73E, kIconGlobe = 0xE774, kIconWarn = 0xE7BA, kIconDisc = 0xE958,
};
const ImWchar kIconRanges[] = {0xE73E, 0xE73E, 0xE774, 0xE774, 0xE7BA, 0xE7BA, 0xE958, 0xE958, 0};
// the letters of the launcher's languages: Latin with the accents of Portuguese, Spanish and Italian, Cyrillic, and
// dashes, quotes and the ellipsis
const ImWchar kTextRanges[] = {0x0020, 0x00FF, 0x0400, 0x045F, 0x2013, 0x2026, 0};

std::string Icon(uint32_t cp) {
    char s[5] = {};
    ImTextCharToUtf8(s, cp);
    return s;
}

void Text(ImFont* f, ImU32 col, const char* s) {
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void Wrapped(ImFont* f, ImU32 col, const char* s) {
    ImGui::PushFont(f);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// an animated 0..1 per id, easing toward on
float Ease(ImGuiID id, bool on, float speed = 12) {
    ImGuiStorage* st = ImGui::GetStateStorage();
    float t = st->GetFloat(id, on ? 1.0f : 0.0f);
    float dt = ImGui::GetIO().DeltaTime;
    t += ((on ? 1.0f : 0.0f) - t) * std::min(1.0f, dt * speed);
    st->SetFloat(id, t);
    return t;
}

ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = V(a), y = V(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t});
}

// c with its alpha times a
ImU32 Fade(ImU32 c, float a) {
    float alpha = float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (ImU32(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

// 0..1, the same for the same i: where a streak or a torn strip ends
float Hash01(int i) {
    float v = std::sin(float(i) * 12.9898f + 4.1414f) * 43758.5453f;
    return v - std::floor(v);
}

// text with a dark edge around it, as the game letters its menus
void Outlined(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, ImU32 edge, const char* s, float px = 1.5f,
              float size = 0) {
    size = size ? size : f->FontSize;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) dl->AddText(f, size, p + ImVec2(S(px) * dx, S(px) * dy), edge, s);
    dl->AddText(f, size, p, col, s);
}

// Warm brass lettering with a dark edge for legibility
void Gold(ImDrawList* dl, ImFont* f, ImVec2 p, const char* s) {
    Outlined(dl, f, p, Rgb(0, 0, 0, 0), Rgb(36, 18, 2, 230), s, 2);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(f, f->FontSize, p, IM_COL32_WHITE, s);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p + ImVec2(0, f->FontSize * 0.25f),
                                                  p + ImVec2(0, f->FontSize * 0.95f), kGoldHi, kGoldLo);
}

// a quad shaded from its top edge to its bottom one: top left, top right, bottom right, bottom left
void Quad(ImDrawList* dl, ImVec2 tl, ImVec2 tr, ImVec2 br, ImVec2 bl, ImU32 top, ImU32 bottom) {
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    ImDrawIdx i = ImDrawIdx(dl->_VtxCurrentIdx);
    for (int k : {0, 1, 2, 0, 2, 3}) dl->PrimWriteIdx(ImDrawIdx(i + k));
    dl->PrimWriteVtx(tl, uv, top);
    dl->PrimWriteVtx(tr, uv, top);
    dl->PrimWriteVtx(br, uv, bottom);
    dl->PrimWriteVtx(bl, uv, bottom);
}

// the menus' bar: its top edge leaning skew to the right, glossy, an edge drawn smooth
void Bar(ImDrawList* dl, ImVec2 a, ImVec2 b, float skew, ImU32 top, ImU32 bottom, ImU32 edge) {
    Quad(dl, {a.x + skew, a.y}, {b.x, a.y}, {b.x - skew, b.y}, {a.x, b.y}, top, bottom);
    float mid = a.y + (b.y - a.y) * 0.45f;
    Quad(dl, {a.x + skew, a.y}, {b.x, a.y}, {b.x - skew * 0.45f, mid}, {a.x + skew * 0.55f, mid}, Rgb(255, 255, 255, 64),
         Rgb(255, 255, 255, 8));
    dl->AddQuad({a.x + skew, a.y}, {b.x, a.y}, {b.x - skew, b.y}, {a.x, b.y}, edge, S(1));
}

// a soft light: col at c, nothing at r
void Glow(ImDrawList* dl, ImVec2 c, ImVec2 r, ImU32 col, int segments = 48) {
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    ImU32 edge = Fade(col, 0);
    dl->PrimReserve(segments * 3, segments + 1);
    ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(c, uv, col);
    for (int i = 0; i < segments; i++) {
        float t = 2 * IM_PI * float(i) / float(segments);
        dl->PrimWriteVtx({c.x + std::cos(t) * r.x, c.y + std::sin(t) * r.y}, uv, edge);
    }
    for (int i = 0; i < segments; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % segments));
    }
}

// a box as the game frames its messages: navy, a light blue line and a fainter one inside it, a point at each corner
void Box(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top = kCard, ImU32 bottom = kCardLo, ImU32 line = kLine) {
    dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom);
    dl->AddRect(a, b, line);
    dl->AddRect(a + ImVec2(S(3), S(3)), b - ImVec2(S(3), S(3)), kLineLo);
    float d = S(3.5f);
    for (ImVec2 c : {a, ImVec2{b.x, a.y}, b, ImVec2{a.x, b.y}})
        dl->AddQuadFilled({c.x, c.y - d}, {c.x + d, c.y}, {c.x, c.y + d}, {c.x - d, c.y}, kLineHi);
}

// the menus' cursor: a silver arrowhead pointing right, its back at at
void Cursor(ImDrawList* dl, ImVec2 at, float len, float alpha = 1) {
    float hh = len * 0.3f;
    ImVec2 tip{at.x + len, at.y}, top{at.x, at.y - hh}, bottom{at.x, at.y + hh}, notch{at.x + len * 0.32f, at.y};
    dl->AddTriangleFilled(top, tip, notch, Fade(Rgb(238, 248, 248), alpha));
    dl->AddTriangleFilled(notch, tip, bottom, Fade(Rgb(112, 170, 178), alpha));
    dl->AddQuad(top, tip, bottom, notch, Fade(Rgb(10, 40, 52), alpha), S(1));
}

// a line fading out at both ends
void Rule(ImDrawList* dl, ImVec2 a, float w, ImU32 col) {
    float h = std::max(1.0f, S(1));
    dl->AddRectFilledMultiColor(a, {a.x + w * 0.5f, a.y + h}, Fade(col, 0), col, col, Fade(col, 0));
    dl->AddRectFilledMultiColor({a.x + w * 0.5f, a.y}, {a.x + w, a.y + h}, col, Fade(col, 0), Fade(col, 0), col);
}

// A page heading with a brass rule and a teal inset.
void Banner(ImDrawList* dl, ImVec2 a, float w, float h) {
    dl->AddRectFilledMultiColor(a, a + ImVec2(w, h), kCardHi, Fade(kCardHi, 0), Fade(kCard, 0), kCard);
    dl->AddRectFilled(a, a + ImVec2(S(3), h), kLineHi);
    Rule(dl, a + ImVec2(0, h + S(3)), w, kLine);
}

// an on/off switch; true when flipped
bool Toggle(const char* id, bool& v) {
    ImVec2 size{S(46), S(22)};
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, size);
    if (pressed) v = !v;
    float t = Ease(ImGui::GetItemID(), v);
    bool hover = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    dl->AddRectFilled(a, a + size, Rgb(2, 8, 32), r);
    if (t > 0.01f) {
        dl->AddRectFilled(a, a + size, Fade(Mix(kAccentLo, hover ? kAccentHi : kAccent, 0.55f), t), r);
        dl->AddRectFilled(a + ImVec2(S(4), S(2)), {a.x + size.x - S(4), a.y + size.y * 0.45f}, Rgb(255, 255, 255, int(60 * t)), r);
    }
    dl->AddRect(a, a + size, hover ? kLineHi : kLine, r);
    ImVec2 c{a.x + r + t * (size.x - 2 * r), a.y + r};
    dl->AddCircleFilled(c, r - S(4), Rgb(230, 236, 240), 24);
    dl->AddCircle(c, r - S(4), Rgb(16, 40, 60), 24);
    return pressed;
}

// joined choices, the one taken on the menus' green bar
template <typename T>
bool Segmented(const char* id, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    ImGui::PushID(id);
    float avail = ImGui::GetContentRegionAvail().x;
    float h = S(32), w = avail / float(values.size());
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, a + ImVec2(avail, h), Rgb(4, 17, 22));
    dl->AddRect(a, a + ImVec2(avail, h), kLineLo);
    bool changed = false;
    for (size_t i = 0; i < values.size(); i++) {
        ImVec2 p{a.x + w * float(i), a.y};
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(int(i));
        if (ImGui::InvisibleButton("seg", {w, h}) && !(values[i] == value)) {
            value = values[i];
            changed = true;
        }
        bool on = values[i] == value, hover = ImGui::IsItemHovered();
        float t = Ease(ImGui::GetItemID(), on, 16);
        ImVec2 in{S(3), S(3)};
        if (t > 0.01f)
            Bar(dl, p + in, p + ImVec2(w, h) - in, S(7), Fade(hover ? kAccentHi : kAccent, t), Fade(kAccentLo, t),
                Fade(Rgb(0, 60, 14), t));
        else if (hover)
            dl->AddRectFilled(p + in, p + ImVec2(w, h) - in, Rgb(255, 255, 255, 14));
        // a name longer than its part, as some languages' are, a little smaller
        ImFont* f = on ? g_italic : g_semi;
        float size = f->FontSize, room = w - S(14);
        ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, names[i].c_str());
        if (ts.x > room) {
            size *= std::max(0.72f, room / ts.x);
            ts = f->CalcTextSizeA(size, FLT_MAX, 0, names[i].c_str());
        }
        ImVec2 tp = p + ImVec2((w - ts.x) * 0.5f, (h - ts.y) * 0.5f);
        if (on) Outlined(dl, f, tp, kText, Rgb(0, 50, 12, 220), names[i].c_str(), 1, size);
        else dl->AddText(f, size, tp, hover ? kText : kMuted, names[i].c_str());
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy({avail, h});
    ImGui::PopID();
    return changed;
}

template <typename T>
void Combo(const char* id, T& value, const std::vector<T>& values, const std::vector<std::string>& names) {
    size_t cur = 0;
    for (size_t i = 0; i < values.size(); i++)
        if (values[i] == value) cur = i;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, names[cur].c_str())) {
        for (size_t i = 0; i < values.size(); i++) {
            if (ImGui::Selectable(names[i].c_str(), i == cur)) value = values[i];
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

// Primary: the menus' green bar; Secondary: their grey one; Ghost: words, lit purple under the pointer
enum class Kind { Primary, Secondary, Ghost };

bool Button(const char* label, ImVec2 size, Kind kind = Kind::Secondary, ImFont* font = nullptr) {
    font = font ? font : g_italic;
    ImVec2 ts = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, label);
    if (size.y == 0) size.y = S(38);
    if (size.x == 0) size.x = ts.x + S(40) + size.y * 0.6f;
    if (size.x < 0) size.x = ImGui::GetContentRegionAvail().x;
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
    float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered() && !disabled, 14);
    bool held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float skew = size.y * 0.3f;
    ImU32 top = 0, bottom = 0, edge = Rgb(0, 0, 0, 170), text_col = kText;
    switch (kind) {
        case Kind::Primary:
            top = Mix(kAccent, kAccentHi, hv * 0.5f);
            bottom = Mix(kAccentLo, kAccent, hv * 0.35f);
            edge = Rgb(0, 54, 12, 230);
            break;
        case Kind::Secondary:
            top = Mix(Rgb(84, 92, 96), Rgb(110, 120, 126), hv);
            bottom = Mix(Rgb(38, 42, 48), Rgb(52, 58, 66), hv);
            break;
        default: text_col = Mix(kMuted, kText, hv); break;
    }
    if (held && kind != Kind::Ghost) {
        top = Mix(top, bottom, 0.5f);
        bottom = Mix(bottom, Rgb(0, 0, 0), 0.3f);
    }
    if (disabled) {
        top = Rgb(48, 52, 58);
        bottom = Rgb(30, 32, 38);
        text_col = Rgb(118, 122, 132);
    }
    if (kind == Kind::Primary && !disabled)  // a green light under the main action
        Glow(dl, a + ImVec2(size.x * 0.5f, size.y * 0.5f), {size.x * 0.62f, size.y * 1.1f}, Rgb(34, 220, 64, int(26 + 40 * hv)));
    if (kind != Kind::Ghost) {
        Bar(dl, a, a + size, skew, top, bottom, disabled ? Rgb(0, 0, 0, 120) : edge);
    } else if (hv > 0.01f) {
        ImU32 p = Fade(kPurple, 0.55f * hv);
        dl->AddRectFilledMultiColor(a, a + size, Fade(p, 0), p, p, Fade(p, 0));
    }
    ImVec2 tp = a + ImVec2((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f);
    if (kind == Kind::Ghost) dl->AddText(font, font->FontSize, tp, text_col, label);
    else Outlined(dl, font, tp, text_col, disabled ? Rgb(0, 0, 0, 90) : edge, label);
    return pressed && !disabled;
}

// a box framed as the game frames its messages; its contents laid out inside
void BeginCard(const char* id, float pad_y = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(22), pad_y ? pad_y : S(18)});
    ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // the frame on the page's draw list, which ImGui draws before the card's own
    Box(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImGui::Dummy({0, S(6)});
}

// a setting: title and description at the left, its control in the column at the right
bool BeginSetting(const char* title, const char* desc, bool first = false) {
    if (!first) {
        Rule(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, kLine);
        ImGui::Dummy({0, S(7)});
    }
    float control = S(300);
    float w = ImGui::GetContentRegionAvail().x - control - S(24);
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    Text(g_semi, kText, title);
    if (desc) Wrapped(g_small, kMuted, desc);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float text_h = ImGui::GetItemRectSize().y;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - control);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (text_h - S(32)) * 0.5f));
    ImGui::BeginGroup();
    return true;
}
void EndSetting() {
    ImGui::EndGroup();
    ImGui::Dummy({0, S(4)});
}

// a switch at the right end of the control column
void ToggleRight(const char* id, bool& v) {
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - S(46));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
    Toggle(id, v);
}

// a page's name on its banner, what it is for under it
void PageTitle(const char* title, const char* desc) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = std::min(ImGui::GetContentRegionAvail().x, S(620)), h = S(42);
    Banner(dl, a, w, h);
    Outlined(dl, g_head, {a.x + S(18), a.y + (h - g_head->FontSize) * 0.5f}, kText, Rgb(80, 26, 4, 235), title);
    ImGui::Dummy({w, h + S(12)});
    if (desc) Wrapped(g_body, kMuted, desc);
    ImGui::Dummy({0, S(10)});
}

// a small heading in the band's teal
void Label(const char* s) {
    Text(g_small, kTealHi, s);
}

// a sidebar entry: the chosen one on the title screen's purple highlight, the menus' cursor at it
bool NavItem(const char* label, bool selected) {
    ImVec2 size{ImGui::GetContentRegionAvail().x, S(38)};
    ImVec2 a = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered(), 14);
    float on = Ease(ImGui::GetItemID() + 1, selected, 14);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y0 = a.y + S(5), y1 = a.y + size.y - S(5);
    float lit = std::max(0.85f * on, 0.35f * hv);
    if (lit > 0.01f) {
        ImU32 p = Fade(kPurple, lit);
        dl->AddRectFilledMultiColor({a.x + S(20), y0}, {a.x + size.x, y1}, p, Fade(p, 0), Fade(p, 0), p);
    }
    if (on > 0.01f) Cursor(dl, {a.x + S(2) - S(8) * (1 - on), (y0 + y1) * 0.5f}, S(20), on);
    ImVec2 p{a.x + S(30), a.y + (size.y - g_italic->FontSize) * 0.5f};
    if (selected) Outlined(dl, g_italic, p, kText, Rgb(40, 0, 40, 200), label, 1);
    else dl->AddText(g_italic, g_italic->FontSize, p, Mix(kMuted, kText, hv), label);
    return pressed;
}

// a status line: coloured icon and text
void Status(uint32_t icon, ImU32 col, const std::string& text) {
    ImGui::PushFont(g_semi);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(Icon(icon).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(0, S(10));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

// a keyboard key drawn as a keycap
void Keycap(const char* key) {
    ImGui::PushFont(g_semi);
    ImVec2 ts = ImGui::CalcTextSize(key);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 size{ts.x + S(18), ts.y + S(6)};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a + ImVec2(0, S(2)), a + size + ImVec2(0, S(2)), Rgb(0, 2, 12));
    dl->AddRectFilledMultiColor(a, a + size, kCardHi, kCardHi, Rgb(4, 14, 50), Rgb(4, 14, 50));
    dl->AddRect(a, a + size, kLine);
    dl->AddText(a + ImVec2(S(9), S(3)), kText, key);
    ImGui::Dummy(size + ImVec2(0, S(2)));
    ImGui::PopFont();
}

// The original astrolabe backdrop has a procedural fallback if artwork is unavailable.
void Backdrop(ImDrawList* dl, ImVec2 size, float left) {
    dl->AddRectFilledMultiColor({0, 0}, size, Rgb(4, 15, 19), Rgb(8, 29, 32), Rgb(3, 12, 17), Rgb(4, 18, 22));
    ImVec2 c{size.x * 0.86f, size.y * 0.44f};
    for (int ring = 0; ring < 4; ++ring)
        dl->AddCircle(c, S(145 + ring * 28), Rgb(166, 145, 92, 22), 128, S(1));
    for (int i = 0; i < 48; ++i) {
        float angle = float(i) * 6.2831853f / 48;
        ImVec2 v{std::cos(angle), std::sin(angle)};
        float r = S(229), inner = r - S(i % 4 == 0 ? 16 : 7);
        dl->AddLine(c + ImVec2(v.x * inner, v.y * inner), c + ImVec2(v.x * r, v.y * r), Rgb(166, 145, 92, 35));
    }
    dl->AddLine({left, S(14)}, {size.x, S(14)}, kLineLo, S(1));
}

void Style() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.ChildRounding = 0;
    st.FrameRounding = 2;
    st.PopupRounding = 2;
    st.GrabRounding = 2;
    st.ScrollbarRounding = 2;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 1;
    st.PopupBorderSize = 1;
    st.WindowPadding = {0, 0};
    st.FramePadding = {12, 7};
    st.ItemSpacing = {10, 8};
    st.ScrollbarSize = 10;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = {0, 0, 0, 0};
    c[ImGuiCol_Text] = V(kText);
    c[ImGuiCol_TextDisabled] = V(kMuted);
    c[ImGuiCol_PopupBg] = V(Rgb(6, 23, 28, 252));
    c[ImGuiCol_Border] = V(kLine);
    c[ImGuiCol_FrameBg] = V(Rgb(4, 17, 22));
    c[ImGuiCol_FrameBgHovered] = V(kCardHi);
    c[ImGuiCol_FrameBgActive] = V(kCardHi);
    c[ImGuiCol_Button] = V(kCardHi);
    c[ImGuiCol_ButtonHovered] = V(Rgb(27, 63, 64));
    c[ImGuiCol_ButtonActive] = V(kCard);
    c[ImGuiCol_Header] = V(Fade(kPurple, 0.6f));
    c[ImGuiCol_HeaderHovered] = V(Fade(kPurple, 0.35f));
    c[ImGuiCol_HeaderActive] = V(Fade(kPurple, 0.8f));
    c[ImGuiCol_CheckMark] = V(kAccent);
    c[ImGuiCol_PlotHistogram] = V(kAccent);
    c[ImGuiCol_NavHighlight] = V(kLineHi);
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = V(Rgb(46, 82, 81));
    c[ImGuiCol_ScrollbarGrabHovered] = V(kLine);
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    st.ScaleAllSizes(g_scale);
}

void Fonts() {
    ImGuiIO& io = ImGui::GetIO();
    std::error_code ec;
    auto path = [&](std::initializer_list<const char*> names) -> std::string {
        for (const char* n : names) {
            std::string p = std::string("C:\\Windows\\Fonts\\") + n;
            if (fs::exists(p, ec)) return p;
        }
        return {};
    };
    // Palatino Linotype, else Georgia: Windows has both
    std::string regular = path({"pala.ttf", "georgia.ttf", "segoeui.ttf"});
    std::string bold = path({"palab.ttf", "georgiab.ttf", "seguisb.ttf"});
    std::string italic = path({"palabi.ttf", "georgiaz.ttf", "seguisb.ttf"});
    std::string icons = path({"SegoeIcons.ttf", "segmdl2.ttf"});
    // the title's fonts write only its own words, in Latin letters: the rest every language's
    auto load = [&](const std::string& file, float size, bool with_icons, const ImWchar* ranges = kTextRanges) -> ImFont* {
        if (file.empty()) return io.Fonts->AddFontDefault();
        ImFont* f = io.Fonts->AddFontFromFileTTF(file.c_str(), size * g_scale, nullptr, ranges);
        if (with_icons && !icons.empty()) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.GlyphOffset = {0, S(2)};
            io.Fonts->AddFontFromFileTTF(icons.c_str(), size * g_scale, &cfg, kIconRanges);
        }
        return f;
    };
    g_body = load(regular, 16.5f, true);
    g_small = load(regular, 14.5f, false);
    g_semi = load(bold, 16.5f, true);
    g_italic = load(italic, 18, false);
    g_head = load(italic, 30, false);
    g_hero = load(italic, 58, false, nullptr);
    g_heroSub = load(italic, 28, false, nullptr);
    g_button = load(italic, 26, false);
}

// a picture the launcher shows: a texture of its own
struct Picture {
    ID3D11ShaderResourceView* view = nullptr;
    float w = 0, h = 0;
    void Release() {
        if (view) view->Release();
        *this = {};
    }
};

// px: BGRA with straight alpha, as ImGui blends, the top row first
Picture Upload(const std::vector<uint8_t>& px, uint32_t w, uint32_t h) {
    Picture p;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, &init, &tex))) return p;
    if (SUCCEEDED(g_dev->CreateShaderResourceView(tex, nullptr, &p.view))) {
        p.w = float(w);
        p.h = float(h);
    }
    tex->Release();
    return p;
}

// Original PNG artwork: standard straight-alpha BGRA, independent of texture packs.
Picture LoadArtwork(const fs::path& file) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) ||
        !w || !h || w > 8192 || h > 8192 || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return {};
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) return {};
    return Upload(px, w, h);
}

// The original Noble Archimedes emblem compiled into icon resource 1.
Picture LoadCrest() {
    HICON icon = HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
    if (!icon) return {};
    Picture p;
    ICONINFO ii{};
    if (GetIconInfo(icon, &ii)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 256;
        bi.bmiHeader.biHeight = -256;  // the top row first
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> px(256 * 256 * 4);
        HDC dc = GetDC(nullptr);
        if (ii.hbmColor && GetDIBits(dc, ii.hbmColor, 0, 256, px.data(), &bi, DIB_RGB_COLORS) == 256) p = Upload(px, 256, 256);
        ReleaseDC(nullptr, dc);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
    }
    DestroyIcon(icon);
    return p;
}

// the launcher's state and its one screen
class Launcher {
public:
    Launcher() {
        settings_.Load();
        saved_ = Snapshot();
        files_.Scan();
        g_activated = false;  // the window's first time in front, just looked at
        gpus_ = Gpus();
        auto_gpu_ = Automatic();
        modes_ = Resolutions();
        LoadArt();
        UpdateMusic();  // ShowWindow has already completed in wWinMain.
    }

    ~Launcher() { Shutdown(); }

    void Shutdown() {
        PlaySoundW(nullptr, nullptr, 0);
        music_playing_ = false;
        background_.Release();
        crest_.Release();
    }

    // the page shown, by the sidebar's order: --page on the command line, for screenshots of a page
    void ShowPage(int page) { page_ = page; }

    // false: the launcher closes
    bool Frame() {
        ShowLanguage();
        if (g_activated) {
            g_activated = false;
            rescan_ = true;
        }
        if (rescan_ && !import_.running) {
            rescan_ = false;
            files_.Scan();
            LoadArt();
            if (!import_.result.empty()) Note(import_.result);
        }
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        Backdrop(bg, io.DisplaySize, S(232));
        if (background_.view) {
            bg->AddImage(background_.view, {0, 0}, io.DisplaySize, {0, 0}, {1, 1}, Rgb(255, 255, 255, 120));
            bg->AddRectFilled({0, 0}, io.DisplaySize, Rgb(3, 16, 21, 110));
        }
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
        bool keep = true;
        Sidebar();
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(36), S(28)});
        ImGui::BeginChild("page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(g_body);
        switch (page_) {
            case 0: keep = HomePage(); break;
            case 1: DisplayPage(); break;
            case 2: GraphicsPage(); break;
            case 3: GamePage(); break;
            case 4: ControlsPage(); break;
            default: FilesPage(); break;
        }
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        AutoSave();
        UpdateMusic();
        return keep;
    }

private:
    // every setting the launcher shows, to see when one changed
    std::string Snapshot() const {
        const Settings& s = settings_;
        return s.display_mode + "|" + std::to_string(s.width) + "x" + std::to_string(s.height) + "|" +
               std::to_string(s.render_percent) + "|" + std::to_string(s.aniso) + "|" + std::to_string(s.vsync) +
               std::to_string(s.widescreen) + std::to_string(s.fps_counter) + std::to_string(s.pause_on_focus_loss) +
               std::to_string(s.keep_open) + std::to_string(s.launcher_music) + "|" + std::to_string(s.menu_key) + "|" + std::to_string(s.texture_pack) +
               std::to_string(s.texture_pack_size) + "|" + std::to_string(s.button_layout) +
               std::to_string(s.lament_save) + "|" + s.gpu + "|" + s.language + "|" + std::to_string(s.game_language);
    }

    void AutoSave() {
        std::string now = Snapshot();
        if (now == saved_) return;
        saved_ = now;
        ShowLanguage();  // a language just chosen: the note saying so in it
        Note(settings_.Save() ? T("Settings saved") : T("Could not write the settings file"));
    }

    // the words and the window's title in the language set
    void ShowLanguage() {
        if (settings_.language == shown_language_) return;
        shown_language_ = settings_.language;
        words::Set(shown_language_);
        SetWindowTextW(g_hwnd, Wide(T("Noble Archimedes - Launcher")).c_str());
    }

    void Note(const std::string& s) {
        status_ = s;
        status_time_ = ImGui::GetTime();
    }

    void Sidebar() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = S(232), h = ImGui::GetIO().DisplaySize.y;
        dl->AddRectFilledMultiColor(a, a + ImVec2(w, h), Rgb(5, 21, 25, 248), Rgb(5, 21, 25, 248), Rgb(3, 14, 19, 252),
                                    Rgb(3, 14, 19, 252));
        // Brass rules separate navigation from the content
        dl->AddRectFilled(a + ImVec2(w - S(1), 0), a + ImVec2(w, h), kLine);
        dl->AddRectFilled(a + ImVec2(w - S(4), 0), a + ImVec2(w - S(3), h), kLineLo);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {S(14), S(20)});
        ImGui::BeginChild("side", {w, h}, ImGuiChildFlags_AlwaysUseWindowPadding);
        // Original emblem and Noble Archimedes wordmark
        dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(S(2), 0);
        float cs = S(44), tx = p.x;
        if (crest_.view) {
            dl->AddImage(crest_.view, p, p + ImVec2(cs, cs));
            tx += cs + S(9);
        }
        Gold(dl, g_heroSub, {tx, p.y - S(3)}, "Noble");
        dl->AddText(g_small, g_small->FontSize, {tx + S(2), p.y + S(27)}, kTealHi, "Archimedes");
        ImGui::Dummy({0, cs + S(22)});

        static const words::Key kPages[] = {"Home", "Display", "Graphics", "Game", "Controls", "Game files"};
        for (int i = 0; i < 6; i++) {
            if (i == 1) {
                ImGui::Dummy({0, S(4)});
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(30));
                Label(T("SETTINGS"));
            }
            if (i == 5) ImGui::Dummy({0, S(6)});
            if (NavItem(T(kPages[i]), page_ == i)) page_ = i;
        }

        // at the bottom: the language, the save note (two lines at most, a language's can be long) and a reset
        float x = ImGui::GetCursorPosX(), line = g_small->FontSize;
        float reset = ImGui::GetWindowHeight() - S(20) - S(36);
        float language = reset - S(4) - 2 * line - S(14) - ImGui::GetFrameHeight();
        if (ImGui::GetCursorPosY() < language) ImGui::SetCursorPosY(language);
        LanguageChoice();
        float age = float(ImGui::GetTime() - status_time_);
        if (!status_.empty() && age < 4) {
            float alpha = std::clamp(4 - age, 0.0f, 1.0f), wrap = ImGui::GetContentRegionAvail().x - S(16);
            float h = g_small->CalcTextSizeA(line, FLT_MAX, wrap, status_.c_str()).y;
            ImGui::SetCursorPos({x + S(16), reset - S(4) - h});
            ImGui::PushTextWrapPos(x + S(16) + wrap);
            Text(g_small, Fade(kMuted, alpha), status_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorPos({x, reset});
        if (Button(T("Reset to defaults"), {-1, S(36)}, Kind::Ghost)) {
            settings_.Defaults();
            Note(T("Defaults restored"));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // the launcher's language, each by its name in itself (ShowLanguage puts it on)
    void LanguageChoice() {
        int cur = words::Current();
        std::string preview = Icon(kIconGlobe) + "  " + words::kLanguages[cur].name;
        ImGui::PushFont(g_body);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##language", preview.c_str())) {
            for (int i : words::kMenu) {
                if (ImGui::Selectable(words::kLanguages[i].name, i == cur)) settings_.language = words::kLanguages[i].code;
                if (i == cur) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Language"));
    }

    bool HomePage() {
        // An original celestial instrument, with a dark reading panel for the title and actions.
        float w = ImGui::GetContentRegionAvail().x, h = S(322);
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Box(dl, a, a + ImVec2(w, h), Rgb(5, 23, 28, 245), Rgb(3, 15, 20, 248));
        dl->PushClipRect(a + ImVec2(S(4), S(4)), a + ImVec2(w - S(4), h - S(4)), true);
        if (background_.view) {
            dl->AddImage(background_.view, a, a + ImVec2(w, h));
            dl->AddRectFilledMultiColor(a, a + ImVec2(w, h), Rgb(3, 18, 23, 248), Rgb(3, 18, 23, 50),
                                        Rgb(3, 18, 23, 150), Rgb(3, 18, 23, 250));
        } else if (crest_.view) {
            float cs = S(240);
            ImVec2 p = a + ImVec2(w - cs - S(20), S(35));
            dl->AddImage(crest_.view, p, p + ImVec2(cs, cs), {0, 0}, {1, 1}, Rgb(220, 203, 154, 100));
        }
        dl->PopClipRect();
        ImVec2 at = a + ImVec2(S(30), S(24));
        Gold(dl, g_hero, at, "Noble");
        Gold(dl, g_hero, at + ImVec2(0, S(55)), "Archimedes");
        ImGui::SetCursorScreenPos(at + ImVec2(S(2), S(133)));
        Label(T("ORIGINAL XBOX  |  NATIVE PC PORT"));
        ImGui::SetCursorScreenPos(at + ImVec2(S(2), S(158)));
        Text(g_small, kText, T("Required input: Castlevania: Curse of Darkness (Xbox)"));
        // The theme can always be muted from Home, and its choice is persisted.
        ImGui::SetCursorScreenPos(a + ImVec2(w - S(230), S(24)));
        Text(g_small, kText, T("Launcher music"));
        ImGui::SetCursorScreenPos(a + ImVec2(w - S(65), S(23)));
        if (Toggle("launchermusic", settings_.launcher_music)) music_suspended_ = false;

        ImGui::SetCursorScreenPos(a + ImVec2(S(30), h - S(26) - S(54)));
        bool keep = true, ready = files_.Playable();
        ImGui::BeginDisabled(!ready || import_.running);
        if (Button(T("Play"), {S(230), S(54)}, Kind::Primary, g_button)) keep = Play();
        ImGui::EndDisabled();
        if (!ready) {
            ImGui::SameLine(0, S(22));
            if (Button(T("Set up game files"), {0, S(54)}, Kind::Secondary)) page_ = 5;
        }
        // beside the buttons, mid way down them (a line of its own: SameLine would put it at their top)
        ImVec2 end = ImGui::GetItemRectMax();
        ImGui::SetCursorScreenPos({end.x + S(20), end.y - S(54) * 0.5f - g_semi->FontSize * 0.5f});
        if (!UsedGpu().d3d12) Status(kIconWarn, kRed, T("No Direct3D 12"));
        else if (ready) Status(kIconCheck, kGreen, T("Ready to play"));
        else if (!files_.exe) Status(kIconWarn, kRed, T("NobleArchimedes.exe is missing"));
        else Status(kIconWarn, kAmber, T("Game files needed"));
        ImGui::SetCursorScreenPos(a + ImVec2(0, h + S(20)));
        if (!UsedGpu().d3d12) {
            BeginCard("gpu");
            GpuStatus();
            EndCard();
        }

        // the HD texture pack: whether there is one, and its switch, a click before Play (its size is on Graphics); one
        // row, the button's height
        BeginCard("pack", S(12));
        float row = S(38), y = ImGui::GetCursorPosY(), mid = y + (row - g_semi->FontSize) * 0.5f;
        ImGui::SetCursorPosY(mid);
        Text(g_semi, kText, T("HD texture pack"));
        ImGui::SameLine(0, S(26));
        ImGui::SetCursorPosY(mid);
        std::string pictures = words::Pictures(files_.pictures);
        if (!files_.pictures) Status(kIconWarn, kMuted, T("None in the textures folder (optional)"));
        else if (settings_.texture_pack) Status(kIconCheck, kGreen, F("{} found, in use", {pictures}));
        else Status(kIconCheck, kMuted, F("{} found, off: the game's own textures", {pictures}));
        ImGui::SameLine();
        if (files_.pictures) {
            ImGui::SetCursorPos({ImGui::GetContentRegionMax().x - S(46), y + (row - S(22)) * 0.5f});
            Toggle("homepack", settings_.texture_pack);
        } else {
            const char* open = T("Open textures folder");
            float bw = g_italic->CalcTextSizeA(g_italic->FontSize, FLT_MAX, 0, open).x + S(40) + row * 0.6f;
            ImGui::SetCursorPos({ImGui::GetContentRegionMax().x - bw, y});
            if (Button(open, {bw, row}, Kind::Secondary)) OpenTextures();
        }
        ImGui::SetCursorPosY(y + row);
        ImGui::Dummy({0, 0});
        EndCard();

        // what is set, at a glance; each opens its page
        Label(T("CURRENT SETUP"));
        ImGui::Dummy({0, S(2)});
        Res r{settings_.width, settings_.height};
        std::string res = r.w ? std::to_string(r.w) + " x " + std::to_string(r.h) : T("Desktop");
        std::string mode = settings_.display_mode == "fullscreen" ? T("Fullscreen")
                           : settings_.display_mode == "windowed" ? T("Windowed")
                                                                  : T("Borderless");
        struct Tile {
            const char* label;
            std::string value;
            int page;
        } tiles[] = {{T("Display"), mode + "  |  " + res, 1},
                     {T("Widescreen"), settings_.widescreen ? T("On (16:9, Hor+)") : T("Off (4:3)"), 2},
                     {T("Render scale"), std::to_string(settings_.render_percent) + "%", 2},
                     {T("Menu key"), KeyName(settings_.menu_key) + "  |  L3 + R3", 3}};
        float gap = S(12), tw = (ImGui::GetContentRegionAvail().x - gap) / 2, th = S(68);
        for (int i = 0; i < 4; i++) {
            if (i % 2) ImGui::SameLine(0, gap);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("tile", {tw, th})) page_ = tiles[i].page;
            float hv = Ease(ImGui::GetItemID(), ImGui::IsItemHovered(), 14);
            ImGui::PopID();
            Box(dl, p, p + ImVec2(tw, th), Mix(kCard, kCardHi, hv), kCardLo, Mix(kLine, kLineHi, hv));
            dl->AddText(g_small, g_small->FontSize, p + ImVec2(S(18), S(13)), kMuted, tiles[i].label);
            dl->AddText(g_semi, g_semi->FontSize, p + ImVec2(S(18), S(34)), kText, tiles[i].value.c_str());
            Cursor(dl, {p.x + tw - S(38) + S(6) * hv, p.y + th * 0.5f}, S(18), 0.3f + 0.7f * hv);
        }
        ImGui::Dummy({0, S(8)});
        std::string tip = F("In game, press {} or click both sticks (L3 + R3) for these settings.", {KeyName(settings_.menu_key)});
        Wrapped(g_small, kMuted, tip.c_str());
        return keep;
    }

    // the card the game will draw with: the one chosen, when it is here with Direct3D 12, else the automatic one
    const Gpu& UsedGpu() const {
        if (!settings_.gpu.empty())
            for (const Gpu& g : gpus_)
                if (g.name == settings_.gpu && g.d3d12) return g;
        return auto_gpu_;
    }

    // the graphics the game will use, and what to do when they cannot run it
    void GpuStatus() {
        const Gpu& gpu = UsedGpu();
        if (!settings_.gpu.empty() && &gpu == &auto_gpu_)
            Status(kIconWarn, kAmber, F("{}  -  not here with Direct3D 12, the game takes the fastest", {settings_.gpu}));
        std::string name = gpu.name.empty() ? T("This PC's graphics") : gpu.name;
        if (gpu.d3d12 && gpu.software) {
            Status(kIconWarn, kAmber, F("{}  -  Direct3D 12 on the processor only", {name}));
            Wrapped(g_small, kMuted, T("No graphics driver is installed, so Windows draws on the processor, far too slowly "
                                       "for the game. Install the driver from NVIDIA, AMD or Intel."));
        } else if (gpu.d3d12) {
            Status(kIconCheck, kGreen, F("{}  -  Direct3D 12, ready", {name}));
        } else {
            Status(kIconWarn, kRed, F("{}  -  no Direct3D 12", {name}));
            Wrapped(g_small, kMuted, T("The game draws with Direct3D 12, which every Windows 10 and 11 has, but only "
                                       "graphics with a driver for it can run. Install the newest driver from NVIDIA, AMD "
                                       "or Intel. Graphics older than NVIDIA's GeForce 600 series, AMD's Radeon HD 7700 or "
                                       "Intel's HD 4200 (2012 and 2013) have none, and the game cannot start on them."));
        }
    }

    void DisplayPage() {
        PageTitle(T("Display"), T("How the game window fills your screen."));
        BeginCard("display");
        BeginSetting(T("Display mode"), T("Borderless fills the monitor without changing its mode and switches away instantly."), true);
        Segmented<std::string>("mode", settings_.display_mode, {"fullscreen", "borderless", "windowed"},
                               {T("Fullscreen"), T("Borderless"), T("Windowed")});
        EndSetting();
        BeginSetting(T("Resolution"), T("The output size. Fullscreen switches the monitor to it; borderless always fills the monitor."));
        {
            size_t cur = 0;
            for (size_t i = 0; i < modes_.size(); i++)
                if (modes_[i].w == settings_.width && modes_[i].h == settings_.height) cur = i;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##res", ResName(modes_[cur]).c_str())) {
                for (size_t i = 0; i < modes_.size(); i++)
                    if (ImGui::Selectable(ResName(modes_[i]).c_str(), i == cur)) {
                        settings_.width = modes_[i].w;
                        settings_.height = modes_[i].h;
                    }
                ImGui::EndCombo();
            }
        }
        EndSetting();
        BeginSetting("V-Sync", T("Off can tear and lowers input latency a little. The game runs at 60 FPS either way."));
        ToggleRight("vsync", settings_.vsync);
        EndSetting();
        EndCard();
    }

    void GraphicsPage() {
        PageTitle(T("Graphics"), T("Picture quality and shape."));
        BeginCard("gpu");
        BeginSetting(T("Graphics card"), T("The one the game draws with, from its next start. Automatic takes the fastest: on "
                                           "a laptop the dedicated GPU, not the one in the processor."), true);
        {
            std::vector<std::string> values{""};
            std::vector<std::string> names{T("Automatic")};  // which one, the line below says
            for (const Gpu& g : gpus_)
                if (g.d3d12 && std::find(values.begin(), values.end(), g.name) == values.end()) {
                    values.push_back(g.name);
                    names.push_back(g.name);
                }
            if (std::find(values.begin(), values.end(), settings_.gpu) == values.end()) {  // chosen, gone since
                values.push_back(settings_.gpu);
                names.push_back(F("{}  (not found)", {settings_.gpu}));
            }
            Combo<std::string>("##gpu", settings_.gpu, values, names);
        }
        EndSetting();
        GpuStatus();
        EndCard();
        BeginCard("presets");
        BeginSetting(T("Quality preset"), T("Sets render scale and texture filtering together."), true);
        int preset = settings_.render_percent == 75 && settings_.aniso == 0      ? 0
                     : settings_.render_percent == 100 && settings_.aniso == 0   ? 1
                     : settings_.render_percent == 150 && settings_.aniso == 8   ? 2
                     : settings_.render_percent == 200 && settings_.aniso == 16  ? 3
                                                                                 : -1;
        if (Segmented<int>("preset", preset, {0, 1, 2, 3}, {T("Low"), T("Original"), T("High"), T("Ultra")})) {
            static const uint32_t kScale[] = {75, 100, 150, 200}, kAniso[] = {0, 0, 8, 16};
            settings_.render_percent = kScale[preset];
            settings_.aniso = kAniso[preset];
        }
        EndSetting();
        EndCard();
        BeginCard("graphics");
        BeginSetting(T("Widescreen"), T("16:9 with more of the world at the sides, nothing stretched. The HUD, menus and "
                                        "movies stay 4:3 in the middle. Needs a 16:9 or wider resolution."), true);
        ToggleRight("wide", settings_.widescreen);
        EndSetting();
        BeginSetting(T("Render scale"), T("Of the output height. Above 100% supersamples for a sharper picture; below is "
                                          "faster on weak GPUs."));
        Segmented<uint32_t>("scale", settings_.render_percent, {50, 75, 100, 150, 200}, {"50%", "75%", "100%", "150%", "200%"});
        EndSetting();
        BeginSetting(T("Anisotropic filtering"), T("Sharper textures on floors and walls seen at an angle."));
        Segmented<uint32_t>("aniso", settings_.aniso, {0, 2, 4, 8, 16}, {T("Off"), "2x", "4x", "8x", "16x"});
        EndSetting();
        EndCard();
        BeginCard("texpack");
        BeginSetting(T("HD texture pack"), T("Texture packs made for PCSX2 replace the game's textures: put a pack's "
                                             "replacement pictures in the textures folder."), true);
        ToggleRight("texpack", settings_.texture_pack);
        EndSetting();
        BeginSetting(T("Texture size"), T("The largest side a picture is loaded at. Larger is sharper and needs more video "
                                          "memory and loading time."));
        Segmented<uint32_t>("texsize", settings_.texture_pack_size, {1024, 2048, 0}, {"1024", "2048", T("Full")});
        EndSetting();
        ImGui::Dummy({0, S(2)});
        if (files_.pictures) Status(kIconCheck, kGreen, F("{} in the textures folder", {words::Pictures(files_.pictures)}));
        else Status(kIconWarn, kMuted, T("No pack in the textures folder"));
        ImGui::Dummy({0, S(4)});
        if (Button(T("Open textures folder"), {0, 0}, Kind::Secondary)) OpenTextures();
        ImGui::SameLine();
        if (Button(T("Check again"), {0, 0}, Kind::Secondary)) rescan_ = true;
        EndCard();
    }

    // made where the game looks for it when there is none
    void OpenTextures() {
        fs::path t = files_.textures.empty() ? ExeDir() / "textures" : files_.textures;
        std::error_code ec;
        fs::create_directories(t, ec);
        Open(t);
        rescan_ = true;
    }

    void GamePage() {
        PageTitle(T("Game"), T("In-game overlay and behaviour."));
        // one card: three would not fit the window
        BeginCard("game");
        BeginSetting(T("Game language"), T("The European release's text, from its next start. The USA release has English only."), true);
        {
            std::vector<uint32_t> values;
            std::vector<std::string> names;
            for (const GameLanguage& g : kGameLanguages) {
                values.push_back(g.console);
                names.push_back(g.name);
            }
            Combo<uint32_t>("##gamelanguage", settings_.game_language, values, names);
        }
        EndSetting();
        BeginSetting(T("FPS counter"), T("Shows the frame rate in the corner."));
        ToggleRight("fps", settings_.fps_counter);
        EndSetting();
        BeginSetting(T("Pause on focus loss"), T("Stops the game while another window is in front."));
        ToggleRight("pause", settings_.pause_on_focus_loss);
        EndSetting();
        BeginSetting(T("Lament of Innocence save"), T("As the PS2 game with a Lament of Innocence save on the memory "
                                                      "card: a Moai at the far end of the room behind the first "
                                                      "courtyard, beside the potion, for the item list."));
        ToggleRight("lament", settings_.lament_save);
        EndSetting();
        BeginSetting(T("Settings menu key"), T("Opens the in-game settings. Pick another if F10 does nothing on your keyboard "
                                               "(laptops often need Fn for it). L3 + R3 on a controller always works."));
        static const std::vector<uint32_t> keys = {VK_F10, VK_F1, VK_F2, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9,
                                                   VK_F11, VK_F12, VK_INSERT, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                                   VK_PAUSE, VK_SCROLL, VK_OEM_3};
        // a key Windows does not name gets one of the language's
        std::vector<std::string> names;
        for (uint32_t k : keys) names.push_back(KeyName(k));
        Combo<uint32_t>("##key", settings_.menu_key, keys, names);
        EndSetting();
        BeginSetting(T("Keep launcher open"), T("Leave this window open while the game runs."));
        ToggleRight("keep", settings_.keep_open);
        EndSetting();
        EndCard();
    }

    void ControlsPage() {
        PageTitle(T("Controls"), T("Xbox, PlayStation and Switch controllers play as player 1, with rumble. The keyboard "
                                   "plays alongside."));
        BeginCard("layout");
        BeginSetting(T("Button layout"), T("PS2 shows the PS2's button icons and gives a controller the PS2's shoulders: L1 "
                                           "resets the camera, R1 guards, L2 and R2 lock on. The keyboard keeps its keys."), true);
        Segmented<uint32_t>("layout", settings_.button_layout, {0, 1}, {"Xbox", "PS2"});
        EndSetting();
        if (settings_.button_layout == 1) {
            ImGui::Dummy({0, S(2)});
            // the icons are the PS2 sheet of an HD pack made from the PS2 game (texpack.cpp)
            if (!settings_.texture_pack) Status(kIconWarn, kMuted, T("The PS2 icons need the HD texture pack on (Graphics)"));
            else if (files_.ps2_buttons) Status(kIconCheck, kGreen, T("PS2 icons found in the texture pack"));
            else Status(kIconWarn, kMuted, T("No PS2 icons in the textures folder: the Xbox ones stay, the shoulders change"));
        }
        EndCard();
        BeginCard("keys");
        // the keys by their names on a keyboard, the controller's buttons (B, Y, Start, Back) by the Xbox's
        std::string menu = KeyName(settings_.menu_key);
        const char* kKeys[][2] = {
            {"W A S D", T("Move")},       {T("Arrows"), T("Camera")}, {T("K / Space"), T("Jump (A)")}, {"J", T("Attack (X)")},
            {"L", "B"},                   {"I", "Y"},                 {"Q / E", T("Triggers")},        {"Z / C", T("White / Black")},
            {T("Enter"), "Start"},        {T("Backspace"), "Back"},   {"Num 8 2 4 6", T("D-pad")},     {menu.c_str(), T("Settings menu")},
        };
        if (ImGui::BeginTable("keys", 4, ImGuiTableFlags_SizingStretchProp)) {
            for (auto& k : kKeys) {
                ImGui::TableNextColumn();
                Keycap(k[0]);
                ImGui::TableNextColumn();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3));
                ImGui::TextUnformatted(k[1]);
            }
            ImGui::EndTable();
        }
        EndCard();
    }

    void FilesPage() {
        PageTitle(T("Game files"), T("Your own copy of the game: the USA (NTSC-U) or European (PAL) release, as a disc image or extracted."));
        BeginCard("status");
        if (files_.assets.empty()) {
            Status(kIconWarn, kRed, T("No assets folder found"));
        } else {
            if (files_.xbe.release != Release::Missing) CopyStatus(kIconCheck, kXbe, files_.xbe);
            else if (files_.isos.empty()) Status(kIconWarn, kMuted, F("{}  -  not extracted", {kXbe}));  // none beside an image
            for (const Files::Image& iso : files_.isos) {
                std::string name = Utf8(iso.path.filename().wstring());
                if (iso.copy.release == Release::Missing) Status(kIconWarn, kRed, F("{}  -  not an Xbox disc image", {name}));
                else CopyStatus(kIconDisc, name, iso.copy);
            }
        }
        if (!files_.exe) Status(kIconWarn, kRed, T("NobleArchimedes.exe is not beside the launcher"));
        if (!files_.assets.empty() && files_.xbe.release == Release::Missing && files_.isos.empty())
            Wrapped(g_small, kMuted, T("Add your disc image (.iso), or extract the disc into the assets folder: default.xbe "
                                       "with its data, movie and sound folders."));
        if (!files_.assets.empty()) Wrapped(g_small, kMuted, Utf8(files_.assets.wstring()).c_str());
        ImGui::Dummy({0, S(6)});
        if (import_.running) {
            ImGui::ProgressBar(import_.progress, {-FLT_MIN, S(36)}, T("Copying disc image..."));
        } else {
            if (Button(T("Add disc image"), {0, 0}, Kind::Primary)) AddImage();
            ImGui::SameLine();
            if (Button(T("Open folder"), {0, 0}, Kind::Secondary)) {
                fs::path a = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
                std::error_code ec;
                fs::create_directories(a, ec);
                Open(a);
                rescan_ = true;
            }
            ImGui::SameLine();
            if (Button(T("Check again"), {0, 0}, Kind::Secondary)) rescan_ = true;
        }
        EndCard();
        BeginCard("folders");
        Text(g_semi, kText, T("Saves and logs"));
        Wrapped(g_small, kMuted, T("Saves are kept beside the game in hdd\\E\\UDATA\\4b4e002d."));
        ImGui::Dummy({0, S(4)});
        if (Button(T("Open saves"), {0, 0}, Kind::Secondary)) {
            fs::path s = ExeDir() / kSaves;
            std::error_code ec;
            Open(fs::exists(s, ec) ? s : ExeDir());
        }
        ImGui::SameLine();
        if (Button(T("Open logs"), {0, 0}, Kind::Secondary)) {
            std::error_code ec;
            Open(fs::exists(ExeDir() / "logs", ec) ? ExeDir() / "logs" : ExeDir());
        }
        EndCard();
    }

    // a copy of the game: which release it is, and that it plays
    void CopyStatus(uint32_t icon, const std::string& name, const Copy& copy) {
        if (copy.release == Release::Other)
            Status(kIconWarn, kRed, F("{}  -  another release, only the USA and European ones work", {name}));
        else if (copy.europe())
            Status(icon, kGreen, copy.release == Release::Match ? F("{}  -  European release, ready", {name})
                                                                : F("{}  -  European release (patched header), ready", {name}));
        else
            Status(icon, kGreen, copy.release == Release::Match ? F("{}  -  USA release, ready", {name})
                                                                : F("{}  -  USA release (patched header), ready", {name}));
    }

    bool Play() {
        saved_ = Snapshot();
        if (!settings_.Save()) {
            Note(T("Could not write the settings file"));
            return true;
        }
        fs::path exe = files_.Player();
        std::wstring cmd = L"\"" + exe.wstring() + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi)) {
            Note(F("Could not start the game (error {})", {std::to_string(GetLastError())}));
            return true;
        }
        PlaySoundW(nullptr, nullptr, 0);
        music_playing_ = false;
        music_suspended_ = true;  // keep-open mode stays quiet while the game runs
        // the game may bring its window to the front
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Note(T("Game started"));
        return settings_.keep_open;
    }

    void AddImage() {
        wchar_t file[MAX_PATH] = {};
        // pairs of a name and its pattern, each ended by a NUL, the list by another (c_str's)
        std::wstring filter = Wide(T("Xbox disc image (*.iso)")) + L'\0' + L"*.iso" + L'\0' + Wide(T("All files")) + L'\0' +
                              L"*.*" + L'\0';
        std::wstring title = Wide(T("Your Castlevania: Curse of Darkness disc image"));
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = g_hwnd;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = title.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;
        fs::path from = file;
        fs::path assets = files_.assets.empty() ? ExeDir() / "assets" : files_.assets;
        fs::path to = assets / from.filename();
        std::error_code ec;
        if (fs::equivalent(from.parent_path(), assets, ec)) {
            Note(T("That image is already in the assets folder"));
            rescan_ = true;
            return;
        }
        import_.Start(from, to);
        rescan_ = true;
    }

    void LoadArt() {
        if (!background_.view) background_ = LoadArtwork(ExeDir() / L"branding" / L"launcher-background.png");
        if (!crest_.view) crest_ = LoadArtwork(ExeDir() / L"branding" / L"noble-icon.png");
        if (!crest_.view) crest_ = LoadCrest();
    }

    void UpdateMusic() {
        bool wanted = settings_.launcher_music && !music_suspended_;
        if (wanted == music_playing_) return;
        if (wanted) {
            fs::path theme = ExeDir() / L"branding" / L"launcher-theme.wav";
            // Missing/invalid audio is silent; never substitute Windows' default alert sound.
            music_playing_ = PlaySoundW(theme.c_str(), nullptr,
                SND_ASYNC | SND_LOOP | SND_FILENAME | SND_NODEFAULT) != FALSE;
            if (!music_playing_) music_suspended_ = true;
        } else {
            PlaySoundW(nullptr, nullptr, 0);
            music_playing_ = false;
        }
    }

    Settings settings_;
    Files files_;
    std::vector<Gpu> gpus_;
    Gpu auto_gpu_;
    Picture background_, crest_;
    bool music_playing_ = false, music_suspended_ = false;
    Import import_;
    std::vector<Res> modes_;
    std::string status_, saved_, shown_language_;
    double status_time_ = -100;
    bool rescan_ = false;
    int page_ = 0;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC, for original launcher artwork
    float scale = float(GetDpiForSystem()) / 96.0f;
    {
        Settings settings;  // the language, for the window's title and an error before the launcher is up
        settings.Load();
        words::Set(settings.language);
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"CastlevaniaLauncher";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, LONG(1060 * scale), LONG(690 * scale)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
    int w = r.right - r.left, h = r.bottom - r.top;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, Wide(T("Noble Archimedes - Launcher")).c_str(), style,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // Windows 11 title bar follows the original teal and brass identity.
    COLORREF caption = RGB(5, 21, 25), words = RGB(236, 206, 140), edge = RGB(118, 101, 66);
    DwmSetWindowAttribute(g_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(g_hwnd, 36 /* DWMWA_TEXT_COLOR */, &words, sizeof(words));
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof(edge));
    if (!CreateDevice(g_hwnd)) {
        MessageBoxW(nullptr, Wide(T("Direct3D 11 is not available.")).c_str(), L"Noble Archimedes", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    g_scale = scale;
    Style();
    Fonts();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Launcher launcher;
    // --page home|display|graphics|game|controls|files opens on that page
    static const wchar_t* kPageNames[] = {L"home", L"display", L"graphics", L"game", L"controls", L"files"};
    for (int i = 0; i < 6; i++)
        if (std::wcsstr(GetCommandLineW(), (std::wstring(L"--page ") + kPageNames[i]).c_str())) launcher.ShowPage(i);
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (IsIconic(g_hwnd)) {
            Sleep(50);
            continue;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (!launcher.Frame()) running = false;
        ImGui::Render();
        ImVec4 bg = V(kBg);
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    launcher.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    g_swap->Release();
    g_ctx->Release();
    g_dev->Release();
    DestroyWindow(g_hwnd);
    return 0;
}
