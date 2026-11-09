// Keyboard play on port 1, and a scripted pad for unattended test runs.
//
// The keyboard is read only while the game window is in front, and adds to a real controller on the same port
// rather than replacing it. Port 1 always reports a controller, so the game can be played with the keyboard alone.
//
//   WASD left stick         arrows right stick (camera)    numpad 8 2 4 6 d-pad
//   K or Space  A (jump)    J  X (attack)                  L  B         I  Y
//   Q  left trigger         E  right trigger               Z  white     C  black
//   Enter  Start            Backspace  Back
//
// CV_PAD_SCRIPT="<ms>:<button>[+<button>...][:<hold ms>],..." presses buttons at times measured from the game's
// first read of the pad (default hold 150 ms). A time ending in f counts the game's frames instead, its hold too
// ("120f:a:10f", default hold 9 frames), so a run does not drift when frames take longer. Buttons: up down left
// right start back a b x y black white lt rt lsu lsd lsl lsr (left stick) rsu rsd rsl rsr (right stick). "@file"
// reads the same entries from a file, one per line or comma separated.
//
// CV_PAD_LIVE=<folder> plays turn by turn: the game waits at its pad read for <folder>\cmd-<n>.txt (n from 1), presses
// its entries as CV_PAD_SCRIPT does, times from the command's start ("<ms>:wait" only waits), then writes
// state-<n>.json (telemetry.cpp) and done-<n>.txt and waits for the next, and rooms.json (every map's rooms) once the
// game has its room table. The window need not be in front. Live entries can also be
//   0:warp=<map>.<room>[.<entry>]     Hector sent to that room at the command's start (game/warp.cpp)
//   0:heal                            his HP back to full at the command's start
//   0:walk=<x>/<z>[/<r>]:<hold>       the bot (bot.cpp) walks him there; fight[=<r>] fights the enemies near
//                                     him. The command ends once its goals are done, or at the hold at most
//   0:guard=<r>                       on those walks, the bot fights an enemy that comes within r first
//   0:set=<setting>=<value>           a setting changed at the command's start, as the in-game menu changes it
//                                     (render_percent, anisotropic_filtering, widescreen, vsync, or a game option
//                                     such as lament_save): the renderer takes it after the frame it presents. One
//                                     read only at the start (texture_pack) is an error: give it with --set instead
//   <t>:ls=<x>/<y>, rs=<x>/<y>        a stick part way, -1 to 1
// <va>:<bytes>[,...] in <folder>\dump.txt (or CV_PAD_LIVE_DUMP) also saves those guest memory ranges at each
// command's end, as mem-<n>-<va>.bin.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/modern_controls.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

extern std::atomic<bool> g_closing;  // below: the window is closing
std::string GameStateJson();         // telemetry.cpp
std::string RoomListJson();          // telemetry.cpp
void TelemetryFrame(const input::Pad& p);        // telemetry.cpp
void RequestWarp(int map, int room, int entry);  // game/warp.cpp
void HealPlayer();                               // telemetry.cpp
struct BotGoal {                                 // bot.cpp
    bool fight;
    float x, z, radius;
};
void BotStart(const std::vector<BotGoal>& goals, float guard);
bool BotBusy();
void BotStop();
void BotPad(input::Pad& p);

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
            else if (b == "l3") s.buttons |= kL3;
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
    // read once at the start (the pack's index and its size, the button sheet): changed here they would only seem to
    // be, so a test asking for one fails (cvtest takes an error line as the step's)
    for (const char* start : {"texture_pack", "texture_pack_size"})
        if (key == start) {
            LOGE("live: %s applies from the next start, not here", key.c_str());
            return;
        }
    uint32_t n = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    bool on = value == "true" || value == "1";
    app::Settings& st = app::Get();
    if (key == "render_percent") st.render_percent = n;
    else if (key == "anisotropic_filtering") st.aniso = n;
    else if (key == "widescreen") st.widescreen = on;
    else if (key == "vsync") st.vsync = on;
    else {
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
            std::ifstream f(cmd);
            std::stringstream ss;
            ss << f.rdbuf();
            l.steps = ParseScript(ss.str());
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

bool MenuOpen();                     // menu.cpp
void MenuToggle();                   // menu.cpp
void MenuWindow();                   // menu.cpp
extern bool g_pause_on_focus_loss;   // menu.cpp
extern uint32_t g_button_layout;     // texpack.cpp
std::atomic<bool> g_closing{false};  // the window is closing: a paused game lets go

uint32_t g_sprint = 0;

namespace {
std::atomic<bool> g_sprinting{false};

// the move stick pushed past the game's own dead zone (16 of the PS2's 128)
bool Moving(const input::Pad& p) {
    constexpr float kMove = 4500;
    return float(p.lx) * float(p.lx) + float(p.ly) * float(p.ly) > kMove * kMove;
}

void Sprint(const input::Pad& p, bool menu, bool scripted) {
    static std::atomic<bool> l3_was{false}, shift_was{false};
    static std::atomic<int64_t> idle_since{-1};  // the frame the stick has been at rest since, -1 moving
    bool l3 = (p.buttons & kL3) != 0, r3 = (p.buttons & kR3) != 0;
    bool shift = !scripted && Focused() && Down(VK_LSHIFT);
    const bool l3_pressed = !l3_was.exchange(l3) && l3 && !r3;
    const bool shift_pressed = !shift_was.exchange(shift) && shift;
    if (!g_sprint || menu || (l3 && r3)) {
        g_sprinting = false;
        return;
    }
    int64_t now = int64_t(d3d::Swaps());
    if ((l3_pressed || shift_pressed) && Moving(p)) {
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

}  // namespace

bool SprintOn() { return g_sprint && g_sprinting.load(std::memory_order_relaxed); }
float SprintFactor() { return float(std::min<uint32_t>(g_sprint, 300)) / 100.0f; }

namespace {
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

}  // namespace

void KeyboardWindow(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    MenuWindow();
}

void KeyboardPad(int port, input::Pad& p) {
    RumbleFrame(port, MenuOpen());
    if (port != 0) return;
    const bool scripted = !Script().empty() || LiveOn();
    modern::Frame(Focused(), MenuOpen(), scripted, d3d::Swaps());
    // pause on focus loss: the game stops at its pad read until the window is in front again; not in scripted runs,
    // which go on behind other windows
    if (g_pause_on_focus_loss && !scripted && !Focused()) {
        RumbleFrame(port, true);
        while (!Focused() && !g_closing && IsWindow(g_hwnd)) Sleep(50);
    }
    if (LiveOn()) LiveWait();
    p.connected = true;
    // The menu chord is based on the physical controller. Scripted L3
    // presses still exercise the sprint logic below.
    static std::atomic<bool> sticks{false};
    bool both = (p.buttons & (kL3 | kR3)) == (kL3 | kR3);
    if (!sticks.exchange(both) && both) MenuToggle();
    if (MenuOpen()) {
        Sprint(p, true, scripted);
        // the menu has the keyboard and controller
        p = input::Pad{};
        p.connected = true;
        TelemetryFrame(p);
        return;
    }
    // the PS2 layout: a controller's shoulders do what the Xbox's triggers do and its triggers what white and black
    // do, as on the PS2 (L1 resets the camera, R1 guards) and as its icons say (texpack.cpp); the keyboard keeps its
    // own keys
    if (g_button_layout == 1) {
        std::swap(p.analog[kWhite], p.analog[kLT]);
        std::swap(p.analog[kBlack], p.analog[kRT]);
    }
    if (!scripted) modern::ApplyStickDeadzones(p);
    Keyboard(p);
    Scripted(p);
    if (LiveOn()) {
        std::lock_guard lock(g_live_m);
        const Live& l = LiveState();
        if (l.running) Press(p, l.steps, MsSince(l.start), int64_t(d3d::Swaps() - l.start_frame));
        if (l.running && l.goals) BotPad(p);
    }
    Sprint(p, false, scripted);
    if (!scripted) modern::ApplyPad(p);
    TelemetryFrame(p);  // the pad as the game gets it, for recorded routes
}

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
