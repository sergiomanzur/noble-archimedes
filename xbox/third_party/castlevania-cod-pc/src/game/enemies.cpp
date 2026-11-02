// The enemies in the room, for tests and the bot (common/bot.cpp). The game keeps no list of them anyone has found,
// but every actor passes through sub_00060010, its max HP (cdecl, the actor at [esp+4]), when it spawns and when
// its AI checks its HP; the actors seen there are kept, each with the room it was seen in.
//
// Actor layout, after Neguchads (Castlevania-COD-Recomp: enemy_edits.cpp, indicators.cpp): u16 +0 the character
// kind, 2 for an enemy; u16 +2 the enemy type; float +0x1C HP. The world position, float3 at +0xD0, was found by
// dumping the knights of room 5.12 (two actors 0x420 apart, each with its own position there).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/release.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(CV_ORIGINAL(CV_MAX_HP));

namespace {

constexpr uint32_t kActorBytes = 0x140;
constexpr uint16_t kEnemy = 2;

struct Seen {
    uint32_t actor;
    int map, room;
};
std::mutex g_m;
std::vector<Seen> g_seen;

bool Readable(uint32_t va, uint32_t bytes) {
    return va >= 0x10000 && va <= 0xFFFFFFFFu - bytes && mem::IsCommitted(va) && mem::IsCommitted(va + bytes - 1);
}

void MapRoom(int& map, int& room) {
    int8_t m = -1, r = -1;
    if (Readable(cv::kMapRoom, 2)) {
        std::memcpy(&m, mem::Base() + cv::kMapRoom, 1);
        std::memcpy(&r, mem::Base() + cv::kMapRoom + 1, 1);
    }
    map = m;
    room = r;
}

}  // namespace

struct EnemyInfo {
    uint32_t actor;
    uint16_t type;
    float hp;
    float pos[3];
};

// the enemies seen in this room that are still enemies with HP left
std::vector<EnemyInfo> Enemies() {
    int map, room;
    MapRoom(map, room);
    std::lock_guard lock(g_m);
    std::vector<EnemyInfo> out;
    std::erase_if(g_seen, [&](const Seen& s) { return s.map != map || s.room != room; });
    for (const Seen& s : g_seen) {
        if (!Readable(s.actor, kActorBytes)) continue;
        const uint8_t* a = mem::Base() + s.actor;
        EnemyInfo e{s.actor};
        uint16_t kind;
        std::memcpy(&kind, a, 2);
        std::memcpy(&e.type, a + 2, 2);
        std::memcpy(&e.hp, a + 0x1C, 4);
        std::memcpy(e.pos, a + 0xD0, 12);
        if (kind != kEnemy || !std::isfinite(e.hp) || e.hp <= 0 || !std::isfinite(e.pos[0]) || !std::isfinite(e.pos[2]))
            continue;
        out.push_back(e);
    }
    return out;
}

X86_FUNC(CV_HOOK(CV_MAX_HP)) {
    uint32_t actor = 0;
    if (Readable(ctx.esp, 8)) std::memcpy(&actor, base + ctx.esp + 4, 4);
    CV_ORIGINAL(CV_MAX_HP)(ctx, base);
    if (!actor || !Readable(actor, kActorBytes)) return;
    int map, room;
    MapRoom(map, room);
    std::lock_guard lock(g_m);
    for (Seen& s : g_seen)
        if (s.actor == actor) {
            s.map = map;
            s.room = room;
            return;
        }
    if (g_seen.size() < 256) g_seen.push_back({actor, map, room});
}
