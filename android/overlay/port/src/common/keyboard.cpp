#include "common/modern_controls.h"
#ifdef __ANDROID__
#include "noble_android.h"
#endif
// Keyboard play on port 1, and a scripted pad for unattended test runs.
//
// The keyboard is read only while the game window is in front, and adds to a real controller on the same port
// rather than replacing it. Port 1 always reports a controller, so the game can be played with the keyboard alone.
//
//   WASD left stick         arrows right stick (camera)    numpad 8 2 4 6 d-pad
//   K or Space  A (jump)    J  X (attack)                  L  B         I  Y
//   Q  left trigger         E  right trigger               Z  white     C  black
//   Enter  Start            Backspace  Back                Left Shift  sprint (L3)    Tab  speed-up (R3)
//
// CV_PAD_SCRIPT="<ms>:<button>[+<button>...][:<hold ms>],..." presses buttons at times measured from the game's
// first read of the pad (default hold 150 ms). A time ending in f counts the game's frames instead, its hold too
// ("120f:a:10f", default hold 9 frames), so a run does not drift when frames take longer. Buttons: up down left
// right start back a b x y black white lt rt l3 r3 (the sticks clicked in: l3+r3 opens the settings menu, as on a
// controller) lsu lsd lsl lsr (left stick) rsu rsd rsl rsr (right stick). "@file"
// reads the same entries from a file, one per line or comma separated.
//
// CV_PAD_LIVE=<folder> plays turn by turn: the game waits at its pad read for <folder>\cmd-<n>.txt (n from 1, written
// whole and ended by a newline: the game reads it until it has that), presses its entries as CV_PAD_SCRIPT does,
// times from the command's start ("<ms>:wait" only waits), then writes state-<n>.json (telemetry.cpp) and
// done-<n>.txt and waits for the next, and rooms.json (every map's rooms) once the game has its room table. The window
// need not be in front. Live entries can also be
//   0:warp=<map>.<room>[.<entry>]     Hector sent to that room at the command's start (game/warp.cpp)
//   0:heal                            his HP back to full at the command's start
//   0:capture                         the frame after the command's start into XBE_CAPTURE_DIR, as F3 captures one:
//                                     each draw's state and textures (the SDK's gpu/d3d12.cpp)
//   0:unplug, 0:plug                  the virtual controller unplugged, plugged in again
//   0:click=<control>                 the settings menu's control of that name clicked (CV_MENU_STATE, menu.cpp)
// With XBE_VIRTUAL_PAD=1 the presses go to the SDK's virtual controller (core/input.cpp), which the game reads through
// SDL as it reads a real one: SDL's mapping, the PS2 layout and the stick response take them, a frame later, and the
// rumble the game sends reaches it (telemetry "vpad").
//   0:walk=<x>/<z>[/<r>]:<hold>       the bot (bot.cpp) walks him there; fight[=<r>] fights the enemies near
//                                     him. The command ends once its goals are done, or at the hold at most
//   0:guard=<r>                       on those walks, the bot fights an enemy that comes within r first
//   0:set=<setting>=<value>           a setting changed at the command's start, as the in-game menu changes it
//                                     (display_mode, width, height, render_percent, anisotropic_filtering,
//                                     widescreen, widescreen_aspect, vsync, fps_cap, hdr, hdr_peak_nits,
//                                     hdr_scene_nits, or a game option such as lament_save): the renderer takes it
//                                     after the frame it presents; hdr_dump=1 has the next HDR frame written to
//                                     XBE_HDR_DUMP's folder (gpu/d3d12.cpp). One
//                                     read only at the start (texture_pack_size, button_layout) is an error: give
//                                     it with --set instead. texture_pack is live: every texture is asked again
//   <t>:ls=<x>/<y>, rs=<x>/<y>        a stick part way, -1 to 1
// <va>:<bytes>[,...] in <folder>\dump.txt (or CV_PAD_LIVE_DUMP) also saves those guest memory ranges at each
// command's end, as mem-<n>-<va>.bin.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/memory.h"
#include "audio/audio.h"
#include "xdk/d3d8.h"

extern std::atomic<bool> g_closing;  // below: the window is closing
std::string GameStateJson();         // telemetry.cpp
std::string RoomListJson();          // telemetry.cpp
void TelemetryFrame(const input::Pad& p);        // telemetry.cpp
void RequestWarp(int map, int room, int entry);  // game/warp.cpp
void HealPlayer();                               // telemetry.cpp
void MenuClick(const std::string& name);         // menu.cpp
struct BotGoal {                                 // bot.cpp
    bool fight;
    float x, z, radius;
};
void BotStart(const std::vector<BotGoal>& goals, float guard);
bool BotBusy();
void BotStop();
void BotPad(input::Pad& p);

// Hector's sprint speed, in percent of his walk: 0 off, 125, 150, 200 (the settings file's sprint; game/hooks.cpp)
uint32_t g_sprint = 0;
// the speed-up's speed, times the game's own: 0 off, 2, 4 (the settings file's speed_up). R3 or Tab turns it on and off
uint32_t g_speed_up = 2;

namespace {

HWND g_hwnd = nullptr;

enum : uint16_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kStart = 0x10, kBack = 0x20, kL3 = 0x40, kR3 = 0x80 };
enum { kA, kB, kX, kY, kBlack, kWhite, kLT, kRT };

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool Focused() {
    HWND fg = GetForegroundWindow();
    if (!fg || !g_hwnd) return false;
    DWORD a = 0, b = 0;
    GetWindowThreadProcessId(fg, &a);
    GetWindowThreadProcessId(g_hwnd, &b);
    return a == b;
}

// a stick axis the other way: -32768 has no positive, so it goes to 32767
int16_t Flip(int16_t v) { return int16_t(-std::max<int>(v, -32767)); }

// one stick axis from two keys, full deflection
int16_t Axis(int neg, int pos) {
    int v = (Down(pos) ? 32767 : 0) - (Down(neg) ? 32767 : 0);
    return int16_t(v);
}

void Keyboard(input::Pad& p) {
    if (!Focused()) return;
    if (Down(VK_NUMPAD8)) p.buttons |= kUp;
    if (Down(VK_NUMPAD2)) p.buttons |= kDown;
    if (Down(VK_NUMPAD4)) p.buttons |= kLeft;
    if (Down(VK_NUMPAD6)) p.buttons |= kRight;
    if (Down(VK_RETURN)) p.buttons |= kStart;
    if (Down(VK_BACK)) p.buttons |= kBack;
    auto press = [&](int slot, bool on) { if (on) p.analog[slot] = 0xFF; };
    press(kA, Down('K') || Down(VK_SPACE));
    press(kX, Down('J'));
    press(kB, Down('L'));
    press(kY, Down('I'));
    press(kWhite, Down('Z'));
    press(kBlack, Down('C'));
    press(kLT, Down('Q'));
    press(kRT, Down('E'));
    // the keyboard wins over a centred stick, a moved stick keeps its value
    int16_t lx = Axis('A', 'D'), ly = Axis('S', 'W'), rx = Axis(VK_LEFT, VK_RIGHT), ry = Axis(VK_DOWN, VK_UP);
    if (lx) p.lx = lx;
    if (ly) p.ly = ly;
    if (rx) p.rx = rx;
    if (ry) p.ry = ry;
}

struct Step {
    int64_t at, until;  // ms, or frames
    bool frames;
    uint16_t buttons;
    uint32_t analog;  // bit per analog slot
    int16_t lx, ly, rx, ry;
    int map = -1, room = 0, entry = 0;  // warp=<map>.<room>[.<entry>]: live commands send Hector there at their start
    bool goal = false, heal = false;    // walk=, fight (bot.cpp) and heal, live commands too
    bool capture = false;               // capture, a live command's too
    int plug = -1;                      // unplug 0, plug 1: the virtual controller
    std::string click;                  // click=<control>: the settings menu's
    BotGoal bot{};
    float guard = 0;  // guard=<r>, for the bot's walks
    std::string set;  // set=<setting>=<value>
};

// "-0.5/1" as a stick: x and y from -1 to 1
void Stick(const std::string& v, int16_t& x, int16_t& y) {
    float fx = 0, fy = 0;
    std::sscanf(v.c_str(), "%f/%f", &fx, &fy);
    auto axis = [](float f) { return int16_t(std::lround(32767 * (std::max)(-1.0f, (std::min)(1.0f, f)))); };
    x = axis(fx);
    y = axis(fy);
}

std::vector<Step> ParseScript(std::string text) {
    std::vector<Step> steps;
    for (char& c : text)
        if (c == '\n' || c == '\r') c = ',';
    std::stringstream all(text);
    std::string entry;
    while (std::getline(all, entry, ',')) {
        if (entry.empty() || entry[0] == '#') continue;
        std::stringstream e(entry);
        std::string at, names, hold;
        std::getline(e, at, ':');
        std::getline(e, names, ':');
        std::getline(e, hold, ':');
        if (at.empty() || names.empty()) continue;
        Step s{std::atoll(at.c_str()), 0, at.back() == 'f', 0, 0, 0, 0, 0, 0};
        s.until = s.at + (hold.empty() ? (s.frames ? 9 : 150) : std::atoll(hold.c_str()));  // atoll stops at the f
        std::stringstream n(names);
        std::string b;
        while (std::getline(n, b, '+')) {
            if (b == "up") s.buttons |= kUp;
            else if (b == "down") s.buttons |= kDown;
            else if (b == "left") s.buttons |= kLeft;
            else if (b == "right") s.buttons |= kRight;
            else if (b == "start") s.buttons |= kStart;
            else if (b == "back") s.buttons |= kBack;
            else if (b == "l3") s.buttons |= kL3;  // the sticks clicked in
            else if (b == "r3") s.buttons |= kR3;
            else if (b == "a") s.analog |= 1u << kA;
            else if (b == "b") s.analog |= 1u << kB;
            else if (b == "x") s.analog |= 1u << kX;
            else if (b == "y") s.analog |= 1u << kY;
            else if (b == "black") s.analog |= 1u << kBlack;
            else if (b == "white") s.analog |= 1u << kWhite;
            else if (b == "lt") s.analog |= 1u << kLT;
            else if (b == "rt") s.analog |= 1u << kRT;
            else if (b == "lsu") s.ly = 32767;
            else if (b == "lsd") s.ly = -32767;
            else if (b == "lsl") s.lx = -32767;
            else if (b == "lsr") s.lx = 32767;
            else if (b == "rsu") s.ry = 32767;
            else if (b == "rsd") s.ry = -32767;
            else if (b == "rsl") s.rx = -32767;
            else if (b == "rsr") s.rx = 32767;
            else if (b.rfind("warp=", 0) == 0) std::sscanf(b.c_str() + 5, "%d.%d.%d", &s.map, &s.room, &s.entry);
            else if (b.rfind("ls=", 0) == 0) Stick(b.substr(3), s.lx, s.ly);
            else if (b.rfind("rs=", 0) == 0) Stick(b.substr(3), s.rx, s.ry);
            else if (b == "heal") s.heal = true;
            else if (b == "capture") s.capture = true;
            else if (b.rfind("click=", 0) == 0) s.click = b.substr(6);
            else if (b == "unplug") s.plug = 0;
            else if (b == "plug") s.plug = 1;
            else if (b.rfind("guard=", 0) == 0) s.guard = float(std::atof(b.c_str() + 6));
            else if (b.rfind("set=", 0) == 0) s.set = b.substr(4);
            else if (b.rfind("walk=", 0) == 0) {
                s.goal = true;
                s.bot = {false, 0, 0, 150};
                std::sscanf(b.c_str() + 5, "%f/%f/%f", &s.bot.x, &s.bot.z, &s.bot.radius);
            } else if (b == "fight" || b.rfind("fight=", 0) == 0) {
                s.goal = true;
                s.bot = {true, 0, 0, 2500};
                if (b.size() > 6) std::sscanf(b.c_str() + 6, "%f", &s.bot.radius);
            }
        }
        steps.push_back(s);
    }
    return steps;
}

// set=<setting>=<value>: the setting's field changed as menu.cpp changes it, nothing saved
void ApplySetting(const std::string& entry) {
    size_t eq = entry.find('=');
    if (eq == std::string::npos) return;
    std::string key = entry.substr(0, eq), value = entry.substr(eq + 1);
    // read once at the start (the size the pack's pictures load at): changed here it would only seem to be, so a test
    // asking for it fails (cvtest takes an error line as the step's)
    if (key == "texture_pack_size") {
        LOGE("live: %s applies from the next start, not here", key.c_str());
        return;
    }
    uint32_t n = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    bool on = value == "true" || value == "1";
    app::Settings& st = app::Get();
    if (key == "display_mode") {
        static const char* const kModes[] = {"fullscreen", "borderless", "windowed"};
        for (uint32_t i = 0; i < 3; i++)
            if (value == kModes[i]) st.display_mode = app::DisplayMode(i);
        app::ApplyDisplay();
    } else if (key == "width" || key == "height") {
        (key == "width" ? st.width : st.height) = n;
        app::ApplyDisplay();
    } else if (key == "render_percent") st.render_percent = n;
    else if (key == "anisotropic_filtering") st.aniso = n;
    else if (key == "widescreen") st.widescreen = on;
    else if (key == "widescreen_aspect") st.widescreen_aspect = n == 1610 ? 1610 : 169;
    else if (key == "vsync") st.vsync = on;
    else if (key == "fps_cap") st.fps_cap = n;
    else if (key == "hdr") st.hdr = on;
    else if (key == "hdr_peak_nits") st.hdr_peak_nits = n;
    else if (key == "hdr_scene_nits") st.hdr_scene_nits = n;
    else if (key == "hdr_dump") {  // tests: the next HDR frame written to XBE_HDR_DUMP's folder (gpu/d3d12.cpp)
        if (const char* dir = std::getenv("XBE_HDR_DUMP")) std::ofstream(std::filesystem::path(dir) / "dump") << "1";
    } else {
        auto it = std::find_if(app::kGame.options.begin(), app::kGame.options.end(),
                               [&](const app::Option& o) { return key == o.key; });
        if (it == app::kGame.options.end()) {
            LOGR("live: no setting %s", key.c_str());
            return;
        }
        if (it->flag) *it->flag = on;
        else *it->value = n;
    }
    LOGR("live: %s = %s", key.c_str(), value.c_str());
}

const std::vector<Step>& Script() {
    static const std::vector<Step> steps = [] {
        const char* v = std::getenv("CV_PAD_SCRIPT");
        if (!v || !*v) return std::vector<Step>{};
        std::string text = v;
        if (text[0] == '@') {
            std::ifstream f(text.substr(1));
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
        return ParseScript(text);
    }();
    return steps;
}

// ms and frames since the script or command started
void Press(input::Pad& p, const std::vector<Step>& steps, int64_t ms, int64_t frames) {
    for (const Step& s : steps) {
        int64_t now = s.frames ? frames : ms;
        if (now < s.at || now >= s.until) continue;
        p.buttons |= s.buttons;
        for (int i = 0; i < 8; i++)
            if (s.analog >> i & 1) p.analog[i] = 0xFF;
        if (s.lx) p.lx = s.lx;
        if (s.ly) p.ly = s.ly;
        if (s.rx) p.rx = s.rx;
        if (s.ry) p.ry = s.ry;
    }
}

using Clock = std::chrono::steady_clock;

int64_t MsSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

void Scripted(input::Pad& p) {
    const auto& steps = Script();
    if (steps.empty()) return;
    static const Clock::time_point start = Clock::now();
    static const uint32_t start_frame = d3d::Swaps();
    Press(p, steps, MsSince(start), int64_t(d3d::Swaps() - start_frame));
}

// CV_PAD_LIVE: the command running, its entries and when it is over
struct Live {
    std::filesystem::path dir;
    int n = 0;
    bool running = false;
    std::vector<Step> steps;
    Clock::time_point start;
    uint32_t start_frame = 0;
    int64_t end_ms = 0, end_frames = 0;  // over when both are reached, or once the bot's goals are done
    bool goals = false;
};

Live& LiveState() {
    static Live live = [] {
        Live l;
        if (const char* v = std::getenv("CV_PAD_LIVE"); v && *v) l.dir = v;
        return l;
    }();
    return live;
}

bool LiveOn() { return !LiveState().dir.empty(); }

// the ranges in <folder>\dump.txt (read at each command's end, so they can change while the game runs) or
// CV_PAD_LIVE_DUMP, written beside the done file
void LiveDump(const Live& l) {
    std::string spec;
    if (std::ifstream f(l.dir / "dump.txt"); f) std::getline(f, spec);
    else if (const char* v = std::getenv("CV_PAD_LIVE_DUMP")) spec = v;
    std::stringstream all(spec);
    std::string e;
    while (std::getline(all, e, ',')) {
        size_t colon = e.find(':');
        if (colon == std::string::npos) continue;
        uint32_t va = uint32_t(std::stoul(e.substr(0, colon), nullptr, 0));
        uint32_t bytes = uint32_t(std::stoul(e.substr(colon + 1), nullptr, 0));
        if (!mem::IsCommitted(va)) continue;
        char name[48];
        snprintf(name, sizeof(name), "mem-%d-%08X.bin", l.n, va);
        std::ofstream(l.dir / name, std::ios::binary).write(reinterpret_cast<const char*>(mem::Base() + va), bytes);
    }
}

// the game reads the pad on more than one thread: one at a time takes a command, the others wait with it
std::mutex g_live_m;

// at each pad read: the command over, its done file; no command, wait here for the next
void LiveWait() {
    std::lock_guard lock(g_live_m);
    Live& l = LiveState();
    bool timed_out = MsSince(l.start) >= l.end_ms && int64_t(d3d::Swaps() - l.start_frame) >= l.end_frames;
    if (l.running && (timed_out || (l.goals && !BotBusy()))) {
        if (l.goals) BotStop();  // what is left timed out
        LiveDump(l);
        std::ofstream(l.dir / ("state-" + std::to_string(l.n) + ".json")) << GameStateJson() << "\n";
        static bool rooms = false;  // the map screen's room table, once it is there
        if (!rooms) {
            std::string list = RoomListJson();
            if (!list.empty()) std::ofstream(l.dir / "rooms.json") << list << "\n";
            rooms = !list.empty();
        }
        std::ofstream(l.dir / ("done-" + std::to_string(l.n) + ".txt")) << l.n << "\n";
        l.running = false;
    }
    while (!l.running && !g_closing && !(g_hwnd && !IsWindow(g_hwnd))) {
        std::filesystem::path cmd = l.dir / ("cmd-" + std::to_string(l.n + 1) + ".txt");
        std::error_code ec;
        if (std::filesystem::exists(cmd, ec)) {
            std::string text;
            if (std::ifstream f(cmd); f) {
                std::stringstream ss;
                ss << f.rdbuf();
                text = ss.str();
            }
            // cvtest renames the command in whole, ended by a newline; a read that got less (on Windows the file can
            // be held a moment after its rename, by a scanner) is tried again rather than taken as a command of
            // nothing, which would be done at once with none of its entries
            if (text.empty() || text.back() != '\n') {
                Sleep(20);
                continue;
            }
            l.steps = ParseScript(text);
            l.end_ms = l.end_frames = 0;
            for (const Step& s : l.steps) {
                int64_t& end = s.frames ? l.end_frames : l.end_ms;
                end = (std::max)(end, s.until);  // windows.h defines max
            }
            std::vector<BotGoal> goals;
            float guard = 0;
            for (const Step& s : l.steps) {
                if (s.map >= 0) RequestWarp(s.map, s.room, s.entry);
                if (s.heal) HealPlayer();
                if (s.capture) gpu::g_capture = true;
                if (s.plug >= 0) input::VirtualPlug(s.plug == 1);
                if (!s.click.empty()) MenuClick(s.click);
                if (!s.set.empty()) ApplySetting(s.set);
                if (s.goal) goals.push_back(s.bot);
                guard = (std::max)(guard, s.guard);
            }
            l.goals = !goals.empty();
            if (l.goals) BotStart(goals, guard);
            l.n++;
            l.start = Clock::now();
            l.start_frame = d3d::Swaps();
            l.running = true;
            break;
        }
        Sleep(20);
    }
}

}  // namespace

// the settings file changed by another program, the launcher kept open beside the game: what the in-game menu
// changes at once (HDR and its levels, render scale, filtering, widescreen, vsync, the game's options) changes here
// too, looked for about twice a second; a change is taken once the file has stood a look unchanged, not mid-write.
// The rest (the pack, the button sheet, the window's mode and size, the graphics card) waits for the next start
void SettingsWatch() {
    static const std::filesystem::path file = app::ExeDir() / app::kGame.settings;
    static std::filesystem::file_time_type seen{}, pending{};
    static bool started = false;
    static std::map<std::string, std::string> values;
    static uint32_t frames = 0;
    if (frames++ % 30) return;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(file, ec);
    if (ec || (started && t == seen)) return;
    if (started && t != pending) {
        pending = t;
        return;
    }
    std::map<std::string, std::string> now;
    std::ifstream f(file);
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    for (std::string line; std::getline(f, line);) {
        size_t eq = line.find('=');
        if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == '[') continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
        now[k] = v;
    }
    if (now.empty()) return;
    seen = pending = t;
    if (started) {
        static const char* kLive[] = {"hdr", "hdr_peak_nits", "hdr_scene_nits", "render_percent",
                                      "anisotropic_filtering", "widescreen", "widescreen_aspect", "vsync"};
        for (const auto& [k, v] : now) {
            auto it = values.find(k);
            if (it != values.end() && it->second == v) continue;
            bool live = std::find_if(std::begin(kLive), std::end(kLive), [&](const char* s) { return k == s; }) !=
                        std::end(kLive);
            bool option = !live && k != "texture_pack" && k != "texture_pack_size" &&
                          std::find_if(app::kGame.options.begin(), app::kGame.options.end(),
                                       [&](const app::Option& o) { return k == o.key; }) != app::kGame.options.end();
            if (live || option) ApplySetting(k + "=" + v);
        }
    }
    values = std::move(now);
    started = true;
}

bool MenuOpen();                     // menu.cpp
void MenuToggle();                   // menu.cpp
void MenuWindow();                   // menu.cpp
extern bool g_pause_on_focus_loss;   // menu.cpp
extern bool g_ps2_stick;             // menu.cpp
extern bool g_invert_camera_x;       // menu.cpp
extern bool g_invert_camera_y;       // menu.cpp
extern uint32_t g_button_layout;     // texpack.cpp
std::atomic<bool> g_closing{false};  // the window is closing: a paused game lets go

// Hector's sprint (game/hooks.cpp): a click of L3, the left stick pressed in, or of Left Shift starts it; the move
// stick let go for a moment (10 frames, a sixth of a second, so a turn through the middle keeps it) ends it, as do the
// settings menu and the sprint set off. L3 with R3 is the settings menu's, not a sprint's. The game has no use of its own for
// either stick's click, in either button layout
std::atomic<bool> g_sprinting{false};

// the move stick pushed past the game's own dead zone (16 of the PS2's 128)
bool Moving(const input::Pad& p) {
    constexpr float kMove = 4500;
    return float(p.lx) * float(p.lx) + float(p.ly) * float(p.ly) > kMove * kMove;
}

void Sprint(const input::Pad& p, bool menu) {
    static std::atomic<bool> l3_was{false}, shift_was{false};
    static std::atomic<int64_t> idle_since{-1};  // the frame the stick has been at rest since, -1 moving
    bool l3 = (p.buttons & kL3) != 0, r3 = (p.buttons & kR3) != 0;
    bool shift = Focused() && Down(VK_LSHIFT);
    bool clicked = (!l3_was.exchange(l3) && l3 && !r3) | (!shift_was.exchange(shift) && shift);
    if (!g_sprint || menu || (l3 && r3)) {
        g_sprinting = false;
        return;
    }
    int64_t now = int64_t(d3d::Swaps());
    if (clicked) {
        g_sprinting = true;
        idle_since = -1;
    }
    if (Moving(p)) {
        idle_since = -1;
    } else {
        int64_t was = -1;
        idle_since.compare_exchange_strong(was, now);
        if (was >= 0 && now - was > 10) g_sprinting = false;
    }
}

// The speed-up: a click of R3, the right stick pressed in, or of Tab turns it on at the setting's speed and off again.
// The whole game then runs that many frames a second (the SDK's app::SetSpeed: its pacing, and one frame of each so
// many shown). A click is R3 let go with L3 not held at any time while it was down (L3 with R3 is the settings
// menu's) and the settings menu closed. The setting changed while it is on takes it along, set off ends it; every
// start begins at the game's own speed. The game has no use of its own for R3, in either button layout
std::atomic<bool> g_sped{false};

void SpeedUp(const input::Pad& p, bool menu) {
    static std::atomic<bool> r3_was{false}, spoiled{false}, tab_was{false};
    bool l3 = (p.buttons & kL3) != 0, r3 = (p.buttons & kR3) != 0;
    bool tab = Focused() && Down(VK_TAB);
    bool was = r3_was.exchange(r3), clicked = false;
    if (!was && r3) spoiled = l3 || menu;
    else if (r3 && (l3 || menu)) spoiled = true;
    else if (was && !r3) clicked = !spoiled.exchange(false) && !menu;
    clicked |= !tab_was.exchange(tab) && tab && !menu;
    if (g_speed_up < 2) g_sped = false;
    else if (clicked) g_sped = !g_sped;
    uint32_t want = g_sped ? std::min<uint32_t>(g_speed_up, 4) : 1;
    if (app::Speed() != want) {
        app::SetSpeed(want);
        LOGR("speed: %ux", want);
    }
}

// the game's rumble, a frame at a time (KeyboardRumble)
struct Motors {
    uint16_t want[2] = {}, sent[2] = {};  // as the game asks for them, as the controller has them
    std::chrono::steady_clock::time_point read;  // the game's last read of the pad
};
std::mutex g_rumble_m;
Motors g_motors[4];
thread_local bool t_rumble_own = false;  // a rumble of RumbleFrame's going out
const bool kRumbleTrace = std::getenv("CV_RUMBLE_TRACE") != nullptr;

// at the game's read of a pad: the motors as the game left them in the frame before, or stopped
void RumbleFrame(int port, bool stop) {
    if (port < 0 || port >= 4) return;
    uint16_t left, right;
    {
        std::lock_guard lock(g_rumble_m);
        Motors& m = g_motors[port];
        m.read = std::chrono::steady_clock::now();
        if (stop) m.want[0] = m.want[1] = 0;
        if (m.want[0] == m.sent[0] && m.want[1] == m.sent[1]) return;
        left = m.want[0];
        right = m.want[1];
    }
    static uint32_t n = 0;
    if (kRumbleTrace && n++ < 400) LOGR("rumble sent %u: port %d, %u / %u", n, port + 1, left, right);
    t_rumble_own = true;
    input::Rumble(port, left, right);  // through KeyboardRumble, which takes it as it is
    t_rumble_own = false;
}

void KeyboardWindow(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    MenuWindow();
}

void KeyboardPad(int port, input::Pad& p) {
    RumbleFrame(port, false);
    if (port != 0) return;
    const bool noble_scripted = !Script().empty() || LiveOn();
    modern::Frame(Focused(), MenuOpen(), noble_scripted, d3d::Swaps());
    // pause on focus loss: the game stops at its pad read until the window is in front again; not in scripted runs,
    // which go on behind other windows
    if (g_pause_on_focus_loss && Script().empty() && !LiveOn() && !Focused()) {
        RumbleFrame(port, true);  // the motors run until set again: they stop with the game
        // and the sound: the mixer goes on by itself, so a looped sound (the courtyard's fountain) played on
        audio::g_paused = true;
        while (!Focused() && !g_closing && IsWindow(g_hwnd)) Sleep(50);
        audio::g_paused = false;
    }
    if (LiveOn()) LiveWait();
    p.connected = true;
#ifdef __ANDROID__
    // the touch overlay is player 1's pad alongside a physical controller, merged here so it goes through the same
    // dead zones, camera modes and inversion as the controller's sticks (ApplyStickDeadzones and ApplyPad below)
    noble_android::MergeTouchPad(0, p);
#endif
    // the PS2 layout: a controller's shoulders do what the Xbox's triggers do and its triggers what white and black
    // do, as on the PS2 (L1 resets the camera, R1 guards) and as its icons say (texpack.cpp); the keyboard keeps its
    // own keys
    if (g_button_layout == 1) {
        std::swap(p.analog[kWhite], p.analog[kLT]);
        std::swap(p.analog[kBlack], p.analog[kRT]);
    }
    if (!noble_scripted) modern::ApplyStickDeadzones(p);
    int16_t camera_x = p.rx, camera_y = p.ry;  // a controller's or the touch pad's right stick, the camera's under the
                                               // settings menu
    Keyboard(p);
    // a test's presses: into the pad, or with the virtual controller into it, which SDL reads back the frame after
    input::Pad scripted{};
    input::Pad& to = input::VirtualOn() ? scripted : p;
    Scripted(to);
    if (LiveOn()) {
        std::lock_guard lock(g_live_m);
        const Live& l = LiveState();
        if (l.running) Press(to, l.steps, MsSince(l.start), int64_t(d3d::Swaps() - l.start_frame));
        if (l.running && l.goals && !MenuOpen()) BotPad(to);
    }
    if (input::VirtualOn()) input::VirtualSet(scripted);
    Sprint(p, MenuOpen());
    SpeedUp(p, MenuOpen());
#ifndef __ANDROID__
    // both sticks clicked in together: the menu, from a controller or a test's script; once a press, of the threads the
    // game reads the pad on (each sees it, one turns it). Android keeps L3 + R3 for showing and hiding the touch pad
    // (NobleActivity), and opens this menu with Start + Back or the overlay's MENU button instead.
    static std::atomic<bool> sticks{false};
    bool both = (p.buttons & (kL3 | kR3)) == (kL3 | kR3);
    if (!sticks.exchange(both) && both) MenuToggle();
#endif
    // the menu just closed from the controller (B, Start, L3 + R3): those buttons are not the game's, so it gets
    // nothing until they are let go
    static std::atomic<bool> menu_was{false}, hold{false};
    bool menu = MenuOpen();
    if (menu_was.exchange(menu) && !menu) hold = true;
    if (hold) {
        bool any = (p.buttons & 0xFF) != 0;
        for (uint8_t a : p.analog) any |= a > 0x40;
        hold = any;
    }
    if (menu || hold) {
        // the menu has the keyboard and controller, but for a controller's right stick: the camera, to look around
        // at what a setting changes (the field of view, the distances, the anti-aliasing). The arrow keys move
        // through the menu, so they do not turn it
        p = input::Pad{};
        p.connected = true;
        p.rx = g_invert_camera_x ? Flip(camera_x) : camera_x;
        p.ry = g_invert_camera_y ? Flip(camera_y) : camera_y;
        TelemetryFrame(p);
        return;
    }
    // the camera turned the other way for a push of the right stick: a controller's, the keyboard's arrows and a test's
    if (!noble_scripted) modern::ApplyPad(p);
    TelemetryFrame(p);  // the pad as the game gets it, for recorded routes
}

// A rumble from the game (XInputSetState) before it goes to the controller. The game's pad class stops a vibration
// whose frames have run out at its read of the pad each frame (sub_003914A0), before the game's vibrations, which last
// a frame, ask for the motors again (sub_003294A0), and those set one motor and then the other (game/hooks.cpp): a
// frame went stop, left, right. A console's request was in flight for longer than that, and the controller got one
// change a frame. Here the motors as the game leaves them each frame go to the controller at its next read of the pad
// (RumbleFrame), and at once when it has not read one for a while. CV_RUMBLE_TRACE logs the game's requests.
void KeyboardRumble(int port, uint16_t& left, uint16_t& right) {
    if (port < 0 || port >= 4) return;
    std::lock_guard lock(g_rumble_m);
    Motors& m = g_motors[port];
    if (kRumbleTrace && !t_rumble_own && (m.want[0] != left || m.want[1] != right)) {
        static uint32_t n = 0;
        if (n++ < 400) LOGR("rumble %u: port %d, %u / %u at frame %llu", n, port + 1, left, right,
                            static_cast<unsigned long long>(d3d::Swaps()));
    }
    m.want[0] = left;
    m.want[1] = right;
    if (t_rumble_own || std::chrono::steady_clock::now() - m.read > std::chrono::milliseconds(50)) {
        m.sent[0] = left;
        m.sent[1] = right;
        return;
    }
    left = m.sent[0];  // the controller's as it is: an unchanged rumble is not sent again
    right = m.sent[1];
}

// Hector sprinting now (game/hooks.cpp, telemetry.cpp), and how much faster than his walk
bool SprintOn() { return g_sprint && g_sprinting.load(std::memory_order_relaxed); }
float SprintFactor() { return float(std::min<uint32_t>(g_sprint, 300)) / 100.0f; }
