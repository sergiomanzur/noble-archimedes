// Game state for tests: the frame, map and room, where Hector is and which way he faces, his HP, MP, EXP and gold,
// where the camera is and looks, and whether a menu is up, as one line of JSON. Every read checks its pages first,
// so a pointer that is not there leaves its fields out instead of faulting.
//
// Addresses in the USA default.xbe (common/release.h has each release's), found by Neguchads (Castlevania-COD-Recomp: camera.cpp, minimap.cpp, hooks.cpp,
// game_cheat_rules.h) and checked in play: stick up with the camera heading 0 moves Hector along +z with yaw 0,
// the pause menu shows the same HP, EXP and gold:
//   map, room            int8 0xB234F4, 0xB234F5 (the game's getter at 0x2F6970 returns 0xB234F4)
//   player               handle u32 0xB41394; in play when int16 [h+0x144] is an index i under 128, int16
//                        [h+0x146+i*2] is 2 and u8 [h+0xEE] has both bits of 0x18
//   position, facing     record 0xB4129C + u32 [h] * 0x1210: float3 at +0x10 and the yaw in radians at +0x2C, what
//                        the game's getters sub_00334640 and sub_00334510 return
//   Hector               int32 attributes at 0xB2CCC0 + n * 4: 3 EXP, 5 max HP, 6 HP, 31 max MP, 32 MP; gold 0xB35204
//   camera               view u32 0xC36B74: angles float3 +0x20, target +0x30, position +0x40. The title, the map
//                        screen and debug views run callback 0x345020 or 0x4E7D0, at [u32 0xC36B70] + 0x1D0
//   menus                u32 0xB25518 is not 0 while the pause or inventory menu is up; a modal box sets bit 16 of the
//                        word at u32 [0x548C10]
//   scene                u32 0xB23410 (found here, by comparing memory in play and in a story scene)
//   game                 0xB23700, what sub_002F7180 returns: the mode u32 at +0 (0 Hector, 1 Crazy, 2 Trevor) and the
//                        save's name at +0x10, which sub_002F7240 returns
//
// CV_TELEMETRY=<file.jsonl> appends the state every CV_TELEMETRY_EVERY frames (30 by default) while the game reads
// the pad, with "pad": the buttons held as the game got them; a button pressed or let go also writes the state right
// then, with "pressed" or "released". Recorded play becomes a route this way (tools/cvtest). Turn by turn play
// (keyboard.cpp) writes the state beside each done file.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/release.h"
#include "core/input.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

namespace {

template <class T>
bool Read(uint32_t va, T& out) {
    if (va < 0x10000 || va > 0xFFFFFFFFu - sizeof(T)) return false;
    if (!mem::IsCommitted(va) || !mem::IsCommitted(va + uint32_t(sizeof(T)) - 1)) return false;
    std::memcpy(&out, mem::Base() + va, sizeof(T));
    return true;
}

struct Vec3 {
    float x, y, z;
};

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// names of the maps seen in play. A new game starts on map 5, the Abandoned Castle; the names Neguchads read from the
// teleporter table (0x4E91F0) call map 5 the Boss Rush, so they wait to be seen before they go here
const char* AreaName(int map) {
    switch (map) {
        case 5: return "Abandoned Castle";
        default: return nullptr;
    }
}

class Json {
public:
    Json() { s_ = "{"; }
    void Key(const char* k) {
        if (s_.size() > 1 && s_.back() != '{' && s_.back() != '[') s_ += ',';
        s_ += '"';
        s_ += k;
        s_ += "\":";
    }
    void Int(const char* k, long long v) {
        Key(k);
        s_ += std::to_string(v);
    }
    void Num(const char* k, float v) {
        Key(k);
        char b[32];
        snprintf(b, sizeof(b), "%.3f", v);
        s_ += b;
    }
    void Bool(const char* k, bool v) {
        Key(k);
        s_ += v ? "true" : "false";
    }
    void Str(const char* k, const char* v) {
        Key(k);
        s_ += '"';
        s_ += v;
        s_ += '"';
    }
    void Vec(const char* k, const Vec3& v) {
        Key(k);
        char b[96];
        snprintf(b, sizeof(b), "[%.3f,%.3f,%.3f]", v.x, v.y, v.z);
        s_ += b;
    }
    void Open(const char* k) {
        Key(k);
        s_ += '{';
    }
    void Close() { s_ += '}'; }
    std::string Done() { return s_ + '}'; }

private:
    std::string s_;
};

void Player(Json& j) {
    uint32_t h = 0;
    int16_t index = -1, state = 0;
    uint8_t flags = 0;
    bool active = Read(cv::kPlayer, h) && h && Read(h + 0x144, index) && index >= 0 && index < 128 &&
                  Read(h + 0x146 + uint32_t(index) * 2, state) && state == 2 && Read(h + 0xEE, flags) &&
                  (flags & 0x18) == 0x18;
    j.Open("player");
    j.Bool("active", active);
    uint32_t record = 0;
    Vec3 pos;
    float yaw = 0;
    if (h && Read(h, record) && record < 128 && Read(cv::kRecords + record * 0x1210 + 0x10, pos) && Finite(pos) &&
        Read(cv::kRecords + record * 0x1210 + 0x2C, yaw) && std::isfinite(yaw)) {
        j.Vec("pos", pos);
        j.Num("yaw", yaw);
    }
    struct Stat {
        const char* name;
        uint32_t va;
    };
    constexpr uint32_t a = cv::kAttributes;
    static const Stat kStats[] = {{"hp", a + 6 * 4},     {"hp_max", a + 5 * 4}, {"mp", a + 32 * 4},
                                  {"mp_max", a + 31 * 4}, {"exp", a + 3 * 4},   {"gold", cv::kGold}};
    for (const Stat& s : kStats) {
        int32_t v;
        if (Read(s.va, v)) j.Int(s.name, v);
    }
    j.Close();
}

void Camera(Json& j) {
    uint32_t view = 0, descriptor = 0, callback = 0;
    Vec3 angles, target, pos;
    if (!Read(cv::kCameraView, view) || !view || !Read(view + 0x20, angles) || !Read(view + 0x30, target) ||
        !Read(view + 0x40, pos) || !Finite(angles) || !Finite(target) || !Finite(pos))
        return;
    j.Open("camera");
    j.Vec("pos", pos);
    j.Vec("target", target);
    j.Vec("angles", angles);
    // the direction the camera looks across the floor, as Hector's yaw: atan2 of x over z
    j.Num("heading", std::atan2(target.x - pos.x, target.z - pos.z));
    if (Read(cv::kCameraDescriptor, descriptor) && descriptor && Read(descriptor + 0x1D0, callback))
        j.Bool("special", callback == cv::kTitleCallback || callback == cv::kDebugCallback);  // title, map screen or a debug view
    j.Close();
}

}  // namespace

struct EnemyInfo {  // game/enemies.cpp
    uint32_t actor;
    uint16_t type;
    float hp;
    float pos[3];
};
std::vector<EnemyInfo> Enemies();
std::string BotJson();  // bot.cpp

// for the bot (bot.cpp), each frame: Hector's position and facing, the camera's heading (the direction stick up
// walks), the scene word and the pause menu
bool PlayerPose(float pos[3], float& yaw) {
    uint32_t h = 0, record = 0;
    Vec3 p;
    if (!Read(cv::kPlayer, h) || !h || !Read(h, record) || record >= 128) return false;
    uint32_t at = cv::kRecords + record * 0x1210;
    if (!Read(at + 0x10, p) || !Finite(p) || !Read(at + 0x2C, yaw) || !std::isfinite(yaw)) return false;
    pos[0] = p.x, pos[1] = p.y, pos[2] = p.z;
    return true;
}

bool CameraHeading(float& heading) {
    uint32_t view = 0;
    Vec3 target, pos;
    if (!Read(cv::kCameraView, view) || !view || !Read(view + 0x30, target) || !Read(view + 0x40, pos) || !Finite(target) ||
        !Finite(pos))
        return false;
    heading = std::atan2(target.x - pos.x, target.z - pos.z);
    return true;
}

uint32_t SceneWord() {
    uint32_t v = 0;
    return Read(cv::kScene, v) ? v : 0;
}

// a test's "heal": Hector's HP set to his max HP (attributes 6 and 5)
void HealPlayer() {
    int32_t max = 0;
    constexpr uint32_t hp_max = cv::kAttributes + 5 * 4, hp = cv::kAttributes + 6 * 4;
    if (Read(hp_max, max) && max > 0 && mem::IsCommitted(hp)) std::memcpy(mem::Base() + hp, &max, 4);
}

bool PauseMenuUp() {
    uint32_t v = 0;
    return Read(cv::kMenu, v) && v;
}

// a message box is up: the save point's, an item's, a hint
bool ModalUp() {
    uint32_t at = 0, flags = 0;
    return Read(cv::kModalFlags, at) && at && Read(at, flags) && ((flags >> 16) & 1);
}

// map * 256 + room, -1 before the game has one
int RoomId() {
    int8_t map = 0, room = 0;
    if (!Read(cv::kMapRoom, map) || !Read(cv::kMapRoom + 1, room)) return -1;
    return map * 256 + room;
}

// the state now, one JSON object on one line
std::string GameStateJson() {
    Json j;
    j.Int("frame", d3d::Swaps());
    int8_t map = 0, room = 0;
    if (Read(cv::kMapRoom, map) && Read(cv::kMapRoom + 1, room)) {
        j.Int("map", map);
        j.Int("room", room);
        if (const char* name = AreaName(map)) j.Str("area", name);
    }
    Player(j);
    Camera(j);
    // u32 0xB23410, seen in play: 0x20000 for a story scene (the room 5.11 tombstone, from its start until Start skips
    // it), 0x8000 while a room change loads, 0 in play, menus and the title
    uint32_t scene = 0;
    if (Read(cv::kScene, scene)) {
        j.Int("scene", scene);
        j.Bool("cutscene", scene & 0x20000);
        j.Bool("loading", scene & 0x8000);
    }
    // the game's mode and the save's name (hooks.cpp: the name codes)
    uint32_t mode = 0;
    std::array<char, 16> raw{};
    if (Read(cv::kGameInfo, mode)) j.Int("mode", mode);
    if (Read(cv::kGameInfo + 0x10, raw)) {
        std::string name(raw.data(), strnlen(raw.data(), raw.size()));
        for (char& c : name)
            if (c < ' ' || c > '~' || c == '"' || c == '\\') c = '?';
        j.Str("name", name.c_str());
    }
    uint32_t menu = 0, flags_at = 0, flags = 0;
    if (Read(cv::kMenu, menu)) j.Bool("menu", menu != 0);
    if (Read(cv::kModalFlags, flags_at) && flags_at && Read(flags_at, flags)) j.Bool("modal", (flags >> 16) & 1);
    // the enemies with HP left in this room, nearest first: actor, type, HP, position, distance across the floor
    float me[3], yaw;
    bool posed = PlayerPose(me, yaw);
    std::vector<EnemyInfo> list = Enemies();
    auto dist = [&](const EnemyInfo& e) { return posed ? std::hypot(e.pos[0] - me[0], e.pos[2] - me[2]) : 0.0f; };
    std::sort(list.begin(), list.end(), [&](const EnemyInfo& a, const EnemyInfo& b) { return dist(a) < dist(b); });
    std::string enemies = "[";
    for (size_t i = 0; i < list.size() && i < 16; i++) {
        char b[160];
        snprintf(b, sizeof(b), "%s{\"actor\":%u,\"type\":%u,\"hp\":%.1f,\"pos\":[%.1f,%.1f,%.1f],\"dist\":%.1f}",
                 i ? "," : "", list[i].actor, list[i].type, list[i].hp, list[i].pos[0], list[i].pos[1], list[i].pos[2],
                 dist(list[i]));
        enemies += b;
    }
    std::string out = j.Done();
    out.pop_back();  // the closing brace: the lists go before it
    out += ",\"enemies\":" + enemies + "]";
    std::string bot = BotJson();
    if (!bot.empty()) out += ",\"bot\":" + bot;
    return out + "}";
}

// every map's rooms from the table the map screen draws from (header 0x55BA00, after Neguchads' minimap.cpp): flags
// +4 (bit 0: absolute pointers, else from the header), group count int16 +10, groups +12; a group is int8 map, int8
// room count, pointer +4 to 24 byte room records, the room ID at +8. [{"map": m, "rooms": [r, ...]}, ...], or "" when
// the table is not there yet
std::string RoomListJson() {
    constexpr uint32_t kHeader = cv::kRoomTable;
    uint32_t flags = 0, groups = 0;
    int16_t count = 0;
    if (!Read(kHeader + 4, flags) || !Read(kHeader + 10, count) || count < 1 || count > 32 || !Read(kHeader + 12, groups))
        return "";
    auto pointer = [&](uint32_t v) { return flags & 1 ? v : kHeader + v; };
    groups = pointer(groups);
    std::string out = "[";
    for (int i = 0; i < count; i++) {
        int8_t map = 0, n = 0;
        uint32_t records = 0;
        if (!Read(groups + i * 8, map) || !Read(groups + i * 8 + 1, n) || n < 1 || !Read(groups + i * 8 + 4, records))
            continue;
        records = pointer(records);
        if (out.size() > 1) out += ',';
        out += "{\"map\":" + std::to_string(map) + ",\"rooms\":[";
        for (int j = 0; j < n; j++) {
            uint8_t room = 0;
            if (!Read(records + j * 24 + 8, room)) continue;
            out += (out.back() == '[' ? "" : ",") + std::to_string(room);
        }
        out += "]}";
    }
    return out.size() > 1 ? out + "]" : "";
}

namespace {

// the pad as a bit per button: the analog slots A B X Y black white LT RT pressed past half, the digital buttons,
// then the left stick past half each way (menus are moved through with it)
uint32_t PadBits(const input::Pad& p) {
    uint32_t bits = 0;
    for (int i = 0; i < 8; i++)
        if (p.analog[i] > 0x80) bits |= 1u << i;
    bits |= uint32_t(p.buttons) << 8;
    if (p.ly > 16384) bits |= 1u << 16;
    if (p.ly < -16384) bits |= 1u << 17;
    if (p.lx < -16384) bits |= 1u << 18;
    if (p.lx > 16384) bits |= 1u << 19;
    return bits;
}

// "a+x", in keyboard.cpp's names
std::string PadNames(uint32_t bits) {
    static const char* names[] = {"a",  "b",    "x",    "y",     "black", "white", "lt", "rt",  "up",  "down",
                                  "left", "right", "start", "back", "ls",   "rs",    "lsu", "lsd", "lsl", "lsr"};
    std::string out;
    for (int i = 0; i < 20; i++)
        if (bits >> i & 1) out += (out.empty() ? "" : "+") + std::string(names[i]);
    return out;
}

}  // namespace

// CV_TELEMETRY: at a pad read, the state once every CV_TELEMETRY_EVERY frames, and when a button goes down or up
void TelemetryFrame(const input::Pad& p) {
    static const std::string path = [] {
        const char* v = std::getenv("CV_TELEMETRY");
        return std::string(v ? v : "");
    }();
    if (path.empty()) return;
    static const uint32_t every = [] {
        const char* v = std::getenv("CV_TELEMETRY_EVERY");
        long n = v ? std::atol(v) : 30;
        return uint32_t(n > 0 ? n : 30);
    }();
    static std::mutex m;
    static uint32_t last = ~0u, held = 0;
    uint32_t frame = d3d::Swaps(), bits = PadBits(p);
    std::lock_guard lock(m);  // the game reads the pad on more than one thread
    uint32_t down = bits & ~held, up = held & ~bits;
    held = bits;
    if (!down && !up && last != ~0u && frame - last < every) return;
    last = frame;
    std::string out = GameStateJson();
    out.pop_back();
    char sticks[64];
    snprintf(sticks, sizeof(sticks), ",\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d}", p.lx, p.ly, p.rx, p.ry);
    out += ",\"pad\":{\"held\":\"" + PadNames(bits) + "\"" + sticks;
    if (down) out += ",\"pressed\":\"" + PadNames(down) + "\"";
    if (up) out += ",\"released\":\"" + PadNames(up) + "\"";
    std::ofstream(path, std::ios::app) << out << "}\n";
}
