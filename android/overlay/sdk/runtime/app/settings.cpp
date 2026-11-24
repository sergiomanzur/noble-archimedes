// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.widescreen = t["widescreen"].value_or(s.widescreen);
    s.widescreen_aspect = t["widescreen_aspect"].value_or(s.widescreen_aspect);
    // 1 = match full screen aspect ratio (e.g. 20:9 / 19.5:9 on mobile), 1610 = 16:10, 169 = 16:9.
    if (s.widescreen_aspect != 1610 && s.widescreen_aspect != 1) s.widescreen_aspect = 169;
    s.hdr = t["hdr"].value_or(s.hdr);
    s.hdr_peak_nits = t["hdr_peak_nits"].value_or(s.hdr_peak_nits);
    s.hdr_scene_nits = t["hdr_scene_nits"].value_or(s.hdr_scene_nits);
    s.frame_generation = std::min(t["frame_generation"].value_or(s.frame_generation), 2u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.gpu = t["gpu"].value_or(s.gpu);
    s.release = t["release"].value_or(s.release);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"custom_textures", s.custom_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"widescreen", s.widescreen},
                  {"widescreen_aspect", int64_t(s.widescreen_aspect)},
                  {"hdr", s.hdr},
                  {"hdr_peak_nits", int64_t(s.hdr_peak_nits)},
                  {"hdr_scene_nits", int64_t(s.hdr_scene_nits)},
                  {"frame_generation", int64_t(s.frame_generation)},
                  {"join_ip", s.join_ip},
                  {"gpu", s.gpu},
                  {"release", s.release}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// the keys this executable knows written over the file's, the others kept (a launcher's own settings)
void Save() {
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
    }
    for (auto&& [key, value] : Table()) t.insert_or_assign(key, value);
    std::ofstream f(File());
    f << t << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

#ifndef _WIN32
// Linux / Android: the display the game window is on, else the primary one, at its desktop mode
namespace {
SDL_DisplayID WindowDisplay() {
    SDL_DisplayID d = Window() ? SDL_GetDisplayForWindow(static_cast<SDL_Window*>(Window())) : 0;
    return d ? d : SDL_GetPrimaryDisplay();
}
}  // namespace

std::vector<Mode> DisplayModes() {
    std::vector<Mode> list;
    int n = 0;
    if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(WindowDisplay(), &n)) {
        for (int i = 0; i < n; i++) list.push_back({uint32_t(modes[i]->w), uint32_t(modes[i]->h)});
        SDL_free(modes);
    }
    return list;
}

Mode Desktop() {
#ifdef __ANDROID__
    // Many Android phones (e.g. Samsung Galaxy A14 5G 1080x2408) report a portrait native panel mode
    // before or even after sensorLandscape rotation. Prefer the live SurfaceView pixel size once the
    // window is up, and always normalize to landscape (width >= height) so Widescreen() is never stuck
    // false and RenderScale() uses the short edge (1080) instead of the long edge (2408).
    Mode wp = WindowPixels();
    uint32_t w = wp.width, h = wp.height;
    if (w <= 1 || h <= 1) {
        const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(WindowDisplay());
        if (m && m->w > 0 && m->h > 0) {
            w = uint32_t(m->w);
            h = uint32_t(m->h);
        } else {
            return {1280, 720};
        }
    }
    if (w < h) std::swap(w, h);
    return {w, h};
#else
    const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(WindowDisplay());
    if (m && m->w > 0 && m->h > 0) return {uint32_t(m->w), uint32_t(m->h)};
    return {1280, 720};
#endif
}
#else
Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}
#endif

Mode Output() {
#ifdef __ANDROID__
    // Android always presents fullscreen at the SurfaceView size; ignore desktop resolution overrides.
    return Desktop();
#else
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
#endif
}

bool Widescreen() {
#ifdef __ANDROID__
    // Compute dynamically from the landscape-normalized Android output so a portrait boot mode can
    // never permanently latch Widescreen() to false.
    const Settings& s = Get();
    if (!s.widescreen) return false;
    Mode m = Output();
    return uint64_t(m.width) * 2 >= uint64_t(m.height) * 3;  // 3:2 or wider
#else
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    uint64_t k = uint64_t(s.width) << 32 | s.height;
    if (key.exchange(k) != k) {
        Mode m = Output();
        wide = m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide && s.widescreen;
#endif
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
#ifdef __ANDROID__
    return Desktop();
#else
    struct Key {
        uint32_t width;
        uint32_t height;
#ifdef _WIN32
        HMONITOR monitor;
#else
        SDL_DisplayID monitor;
#endif
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
#ifdef _WIN32
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
#else
    Key now{s.width, s.height, WindowDisplay()};
#endif
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
#endif
}

float WideAspect() {
    uint32_t a = Get().widescreen_aspect;
    if (a == 1) {
        Mode m = CachedOutput();
        if (m.width && m.height) {
            return std::clamp(float(m.width) / float(m.height), 4.0f / 3.0f, 21.0f / 9.0f);
        }
        return 16.0f / 9.0f;
    }
    return a == 1610 ? 16.0f / 10.0f : 16.0f / 9.0f;
}

namespace {
std::mutex g_hdr_lock;
HdrState g_hdr;
}  // namespace

HdrState Hdr() {
    std::lock_guard<std::mutex> guard(g_hdr_lock);
    return g_hdr;
}

void SetHdr(const HdrState& h) {
    std::lock_guard<std::mutex> guard(g_hdr_lock);
    g_hdr = h;
}

namespace {
FrameGenState g_framegen;  // under g_hdr_lock
}  // namespace

FrameGenState FrameGen() {
    std::lock_guard<std::mutex> guard(g_hdr_lock);
    return g_framegen;
}

void SetFrameGen(const FrameGenState& s) {
    std::lock_guard<std::mutex> guard(g_hdr_lock);
    g_framegen = s;
}

namespace {
std::atomic<uint32_t> g_speed{1};
}  // namespace

void SetSpeed(uint32_t times) { g_speed.store(std::clamp(times, 1u, 8u), std::memory_order_relaxed); }
uint32_t Speed() { return g_speed.load(std::memory_order_relaxed); }

uint32_t HdrPeak() {
    if (uint32_t want = Get().hdr_peak_nits) return std::clamp(want, 400u, 10000u);
    uint32_t display = Hdr().peak_nits;
    return display >= 400 ? display : 1000;
}

float RenderScale(uint32_t guest_height, bool dlss) {
    const uint32_t output_height = CachedOutput().height;
    float h = output_height * (Get().render_percent / 100.0f);
    if (dlss && h > std::max(output_height, 2160u)) {
        h = float(std::max(output_height, 2160u));
        LOG_FIRST(1, "render scale: limiting DLSS supersampling to %u lines", uint32_t(h));
    }
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
