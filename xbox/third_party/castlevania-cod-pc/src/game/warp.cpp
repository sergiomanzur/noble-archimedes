// Warps for tests: Hector sent to a map and room the way doors and teleporters send him (0x75875, 0x142FC4,
// 0x892DD), so a test can start anywhere. RequestWarp (keyboard.cpp: a live entry "warp=<map>.<room>[.<entry>]")
// leaves the destination; the camera update, which runs every frame on the game's thread, sends it once the game
// takes room changes:
//   sub_002F6AB0(6)                  1 when no scene change is locked or waiting ([0xB23528], [0xB2352C])
//   sub_002F6950(map, room, entry)   the destination, kept at 0xB234F8 until the change copies it to 0xB234F4
//   sub_002F62E0(0x100000A4, 8)      the flag doors set before a change (teleporters set it to 1)
//   sub_002F6AD0(6)                  the room change
// (the USA release's addresses; common/release.h has each release's)
#include <atomic>
#include <cstring>
#include <initializer_list>

#include "common/release.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(CV_ORIGINAL(CV_CAMERA_UPDATE));
X86_EXTERN_FUNC(CV_ORIGINAL(CV_SCENE_FREE));
X86_EXTERN_FUNC(CV_ORIGINAL(CV_SET_ROOM));
X86_EXTERN_FUNC(CV_ORIGINAL(CV_SET_FLAG));
X86_EXTERN_FUNC(CV_ORIGINAL(CV_CHANGE_ROOM));

namespace {

std::atomic<uint32_t> g_warp{0};  // bit 31 set: map in bits 16-23, room 8-15, entry 0-7

constexpr uint32_t kRoomChange = 6;
constexpr uint32_t kFlagBeforeChange = 0x100000A4;

// a cdecl game function, its arguments and a return address put below the caller's stack; its eax
uint32_t Call(X86Context& ctx, uint8_t* base, void (*f)(X86Context&, uint8_t*), std::initializer_list<uint32_t> args) {
    X86Context call = ctx;
    call.esp = ctx.esp - 256;
    uint32_t at = call.esp, zero = 0;
    std::memcpy(base + at, &zero, 4);
    for (uint32_t a : args) std::memcpy(base + (at += 4), &a, 4);
    f(call, base);
    return call.eax;
}

}  // namespace

void RequestWarp(int map, int room, int entry) {
    g_warp = 0x80000000u | uint32_t(map & 0xFF) << 16 | uint32_t(room & 0xFF) << 8 | uint32_t(entry & 0xFF);
    LOGR("warp: asked for map %d room %d entry %d", map, room, entry);
}

// the camera update, each frame of play: a warp asked for goes here
X86_FUNC(CV_HOOK(CV_CAMERA_UPDATE)) {
    uint32_t w = g_warp.load();
    if (w && ctx.esp >= 0x10000 && mem::IsCommitted(ctx.esp - 256) &&
        Call(ctx, base, CV_ORIGINAL(CV_SCENE_FREE), {kRoomChange})) {
        uint32_t map = w >> 16 & 0xFF, room = w >> 8 & 0xFF, entry = w & 0xFF;
        Call(ctx, base, CV_ORIGINAL(CV_SET_ROOM), {map, room, entry});
        Call(ctx, base, CV_ORIGINAL(CV_SET_FLAG), {kFlagBeforeChange, 8});
        Call(ctx, base, CV_ORIGINAL(CV_CHANGE_ROOM), {kRoomChange});
        g_warp = 0;
        LOGR("warp: sent to map %u room %u entry %u", map, room, entry);
    }
    CV_ORIGINAL(CV_CAMERA_UPDATE)(ctx, base);
}
