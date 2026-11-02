// The test bot: walks Hector to a point and fights the enemies near him, steering every frame from the game state
// (telemetry.cpp, game/enemies.cpp), so the fixed cameras cutting do not throw it off. Turn by turn commands give it
// goals (keyboard.cpp), one after another:
//   walk=<x>/<z>[/<radius>]    to that point across the floor, reached within radius (default 150)
//   fight[=<radius>]           the enemies within radius of Hector (default 2500): to the nearest one, attacking from
//                              close by, cleared once none has been there for half a second
//   guard=<radius>             on this command's walks, an enemy within radius is fought first
// A goal also ends when its command's time is up, and all of them when the room changes ("room") or a message box
// comes up ("modal"). Stick up walks the way the camera looks: with heading h, forward is (sin h, cos h) in x and z
// and right is (cos h, -sin h). Getting no closer, and in a fight landing no hit, for 3/4 of a second, it steps aside
// for half a second, the other way each time, and gives up after 8 tries (20 in a fight). While a story scene plays,
// a room loads or the pause menu is up it leaves the pad alone.
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "core/input.h"
#include "xdk/d3d8.h"

bool PlayerPose(float pos[3], float& yaw);  // telemetry.cpp
bool CameraHeading(float& heading);         // telemetry.cpp
uint32_t SceneWord();                       // telemetry.cpp
bool PauseMenuUp();                         // telemetry.cpp
bool ModalUp();                             // telemetry.cpp
int RoomId();                               // telemetry.cpp
struct EnemyInfo {                          // game/enemies.cpp
    uint32_t actor;
    uint16_t type;
    float hp;
    float pos[3];
};
std::vector<EnemyInfo> Enemies();

struct BotGoal {
    bool fight;
    float x, z, radius;
};

namespace {

constexpr int kB = 1, kX = 2;          // the attack buttons' analog slots: X hits for 15 HP, B for 25
constexpr float kAttackRange = 450;    // close enough to swing: his attacks lunge, an enemy of room 5.2 keeps ~400
                                       // away and the armoured knights stop him ~250 away
constexpr uint32_t kScenes = 0x28000;  // story scene, room change (telemetry.cpp)
constexpr uint32_t kStuckFrames = 45, kAsideFrames = 30, kMaxAside = 8, kClearFrames = 30;
constexpr uint32_t kMaxAsideFighting = 20;  // an enemy that keeps backing off is chased longer
constexpr int kNoRoom = -2;            // RoomId() gives -1 before the game has a room

struct Bot {
    std::vector<BotGoal> goals;
    size_t at = 0;
    std::vector<std::string> results;  // per goal: reached, cleared, stuck, timeout
    float guard = 0;
    float dist = 0, best = 1e30f, enemy_hp = 1e30f;
    uint32_t best_frame = 0, aside_until = 0, aside = 0, empty_since = 0;
    float side = 1;
    bool fighting = false;
    int start_enemies = -1;
    int room = kNoRoom;  // RoomId() at the first frame
    // what the last frame pressed, for the game's second pad read in the same frame
    uint32_t frame = ~0u;
    bool press = false, attack = false;
    int button = kX;
    int16_t lx = 0, ly = 0;
};
std::mutex g_m;
Bot g_bot;

void Finish(Bot& b, const char* result) {
    b.results.push_back(result);
    b.at++;
    b.best = 1e30f;
    b.enemy_hp = 1e30f;
    b.aside = 0;
    b.aside_until = 0;
    b.empty_since = 0;
}

// this frame's stick and buttons for the current goal
void Steer(Bot& b, uint32_t frame) {
    b.press = b.attack = false;
    if (b.at >= b.goals.size()) return;
    int room = RoomId();
    if (b.room == kNoRoom) {
        b.room = room;
    } else if (room != b.room) {  // what is left ends: its points are in the room left behind
        while (b.at < b.goals.size()) Finish(b, "room");
        return;
    }
    if (ModalUp()) {  // a message box (a save point's, say) ends them too: whoever gave them answers it
        while (b.at < b.goals.size()) Finish(b, "modal");
        return;
    }
    float pos[3], yaw, h;
    if ((SceneWord() & kScenes) || PauseMenuUp() || !PlayerPose(pos, yaw) || !CameraHeading(h)) {
        b.best_frame = frame;  // waiting is not being stuck
        return;
    }
    BotGoal& g = b.goals[b.at];
    // the enemies to fight: those within a fight's radius, or on a walk those within the guard radius
    float reach = g.fight ? g.radius : b.guard;
    std::vector<EnemyInfo> list;
    if (reach > 0) list = Enemies();
    const EnemyInfo* near = nullptr;
    float nd = 0, hp = 0;
    for (const EnemyInfo& e : list) {
        float d = std::hypot(e.pos[0] - pos[0], e.pos[2] - pos[2]);
        if (d > reach) continue;
        hp += e.hp;
        if (!near || d < nd) near = &e, nd = d;
    }
    if (g.fight && b.start_enemies < 0) b.start_enemies = int(list.size());
    if (g.fight && !near) {
        if (!b.empty_since) b.empty_since = frame;
        if (frame - b.empty_since >= kClearFrames) Finish(b, "cleared");
        return;
    }
    b.empty_since = 0;
    bool fighting = near != nullptr;
    if (fighting != b.fighting) {  // to an enemy or back to the walk: distances start over
        b.fighting = fighting;
        b.best = 1e30f;
        b.best_frame = frame;
        b.enemy_hp = 1e30f;
    }
    float tx = g.x, tz = g.z;
    if (fighting) {
        if (hp < b.enemy_hp && b.enemy_hp < 1e30f) {  // a hit landed: progress, like getting closer
            b.best_frame = frame;
            b.aside = 0;
        }
        b.enemy_hp = hp;
        tx = near->pos[0], tz = near->pos[2];
    }
    float dx = tx - pos[0], dz = tz - pos[2], d = std::hypot(dx, dz);
    b.dist = d;
    if (!fighting && d <= g.radius) {
        Finish(b, "reached");
        return;
    }
    float amount = 1;
    if (fighting && d <= kAttackRange) {
        int beat = (frame / 4) % 6;       // 4 frames pressed, 4 let go: X, X, B, as recorded play fights
        b.attack = beat % 2 == 0;
        b.button = beat == 4 ? kB : kX;
        amount = 0.35f;                   // turned to face it, hardly walking
        b.best = d;                       // fighting is not being stuck
        b.best_frame = frame;
    } else if (d < b.best - 20) {
        b.best = d;
        b.best_frame = frame;
    } else if (frame - b.best_frame > kStuckFrames && frame >= b.aside_until) {
        if (++b.aside > (fighting ? kMaxAsideFighting : kMaxAside)) {
            Finish(b, "stuck");
            return;
        }
        b.side = -b.side;
        b.aside_until = frame + kAsideFrames;
        b.best_frame = frame + kAsideFrames;
    }
    float ux = dx / d, uz = dz / d;
    if (frame < b.aside_until) {  // a quarter turn aside
        float t = ux;
        ux = -uz * b.side;
        uz = t * b.side;
    }
    float fx = std::sin(h), fz = std::cos(h), rx = std::cos(h), rz = -std::sin(h);
    b.ly = int16_t(32767 * amount * (ux * fx + uz * fz));
    b.lx = int16_t(32767 * amount * (ux * rx + uz * rz));
    b.press = true;
}

}  // namespace

// a command's goals, replacing any left from the last one, and the guard radius for its walks (0: none)
void BotStart(const std::vector<BotGoal>& goals, float guard) {
    std::lock_guard lock(g_m);
    g_bot = Bot{};
    g_bot.goals = goals;
    g_bot.guard = guard;
}

// goals left
bool BotBusy() {
    std::lock_guard lock(g_m);
    return g_bot.at < g_bot.goals.size();
}

// the command's time is up: what is left timed out
void BotStop() {
    std::lock_guard lock(g_m);
    while (g_bot.at < g_bot.goals.size()) Finish(g_bot, "timeout");
}

// at each pad read
void BotPad(input::Pad& p) {
    std::lock_guard lock(g_m);
    Bot& b = g_bot;
    if (b.goals.empty()) return;
    uint32_t frame = d3d::Swaps();
    if (frame != b.frame) {
        b.frame = frame;
        Steer(b, frame);
    }
    if (!b.press) return;
    p.lx = b.lx;
    p.ly = b.ly;
    if (b.attack) p.analog[b.button] = 0xFF;
}

// for the state: the goals, how each ended, the distance to the current one
std::string BotJson() {
    std::lock_guard lock(g_m);
    const Bot& b = g_bot;
    if (b.goals.empty()) return "";
    std::string r = "[";
    for (const std::string& s : b.results) r += (r.size() > 1 ? ",\"" : "\"") + s + "\"";
    char out[256];
    snprintf(out, sizeof(out), "{\"goals\":%zu,\"done\":%zu,\"results\":%s],\"dist\":%.1f,\"aside\":%u,\"enemies_at_start\":%d}",
             b.goals.size(), b.at, r.c_str(), b.dist, b.aside, b.start_enemies);
    return out;
}
