// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
//
// Each release has the function at its own address: CV_HOOK and CV_ORIGINAL name it by the one this executable was
// made from (common/release.h); the European executable compiles this file too (game_eu/hooks.cpp). The addresses in
// the comments are the USA release's.
#include <algorithm>
#include <cstring>

#include "app/app.h"
#include "common/release.h"
#include "cpu/x86_ops.h"

extern bool g_lament_save;  // menu.cpp
extern bool g_ps2_stick;    // menu.cpp
bool SprintOn();            // keyboard.cpp
float SprintFactor();       // keyboard.cpp

namespace {


void Scale(uint8_t* base, uint32_t va, float k) {
    float v;
    std::memcpy(&v, base + va, 4);
    v *= k;
    std::memcpy(base + va, &v, 4);
}

}  // namespace

// The camera's matrices, made again when the camera changed (a PS2 style libvu0 camera: sceVu0ViewScreenMatrix and
// clip matrices). Widescreen draws more at the sides (widescreen.cpp), so the clip data objects are culled against
// is made wider by the same 4:3 to 16:9 (or 16:10); the matrices the scene is drawn with are left alone.
void ViewMake(const X86Context& ctx, const uint8_t* base);  // game/view.cpp: CV_VIEW_TRACE

X86_EXTERN_FUNC(CV_ORIGINAL(CV_CAMERA_MATRICES));
X86_FUNC(CV_HOOK(CV_CAMERA_MATRICES)) {
    ViewMake(ctx, base);
    CV_ORIGINAL(CV_CAMERA_MATRICES)(ctx, base);
    if (!app::Widescreen()) return;
    const float kWiden = (4.0f / 3.0f) / app::WideAspect();
    Scale(base, cv::kClipScreenX, kWiden);
    Scale(base, cv::kViewClipX, kWiden);
    Scale(base, cv::kViewClipXv, kWiden);
    Scale(base, cv::kProjectionXv, kWiden);
}

// The sticks as the PS2 game read them. The Xbox release reads XInput through a pad class of its own (XInputGetState
// in sub_00391EA0, each state into the class in sub_00391990), which puts every thumbstick axis through this filter
// by itself: under 11000 of 32767, a third of its throw, the axis is made 0. The game's own code then makes each stick
// a direction and a distance (sub_00041D60: atan2 of the two axes scaled to the PS2's 128, and the distance with a
// dead zone of its own, (d - 16) / 79 held to 0..1), as the PS2 game did with its 8 bit sticks. Filtered by axis
// first, a stick within 20 degrees of up, down, left or right at full tilt (42 at half tilt) loses its other axis, so
// Hector walks only straight along those, and a walk starts at a third of his speed. With ps2_stick on, every axis
// goes through and the game's own round dead zone is the one that counts, as on the PS2.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_STICK_AXIS));
X86_FUNC(CV_HOOK(CV_STICK_AXIS)) {
    uint16_t axis;
    std::memcpy(&axis, base + ctx.esp + 4, 2);  // (int16 axis), ret 4: read before the original pops it
    CV_ORIGINAL(CV_STICK_AXIS)(ctx, base);
    if (g_ps2_stick) ctx.eax = (ctx.eax & 0xFFFF0000u) | axis;
}

// The PS2's two motors. A PS2 controller has a small motor, on or off (actuator 0, given 0 or 1), and a large one of 256
// speeds (actuator 1), and this is the game's call that sets one of them (port, actuator, value, frames). Its
// vibrations ask for both every frame while they last (sub_003294A0: the large one's speed, then the small one's 1,
// each for a frame). The Xbox release made both of the controller's motors the one value / 255 here, then set them
// (sub_00391DF0: port, left, right, frames, a motor below 0 left as it is), so the small motor's 1 that follows the
// large one's speed leaves 1 / 255 on both, too weak to feel. On the console the large motor's speed went out first
// and the small one's waited behind it (XInputSetState answers ERROR_IO_PENDING, and the pad class skips a port with a
// request in flight, sub_003914A0); here a request is done at once and the last one stays. Each actuator sets its own
// motor here, as on the PS2: the large one the left, heavy motor at its speed, the small one the right at full.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_PS2_MOTOR));
X86_FUNC(CV_HOOK(CV_PS2_MOTOR)) {
    int32_t actuator, value;
    std::memcpy(&actuator, base + ctx.esp + 8, 4);
    std::memcpy(&value, base + ctx.esp + 12, 4);
    if (actuator != 0 && actuator != 1) {
        CV_ORIGINAL(CV_PS2_MOTOR)(ctx, base);
        return;
    }
    float speed = actuator == 1 ? std::clamp(value, 0, 255) / 255.0f : value > 0 ? 1.0f : 0.0f, kept = -1.0f;
    float left = actuator == 1 ? speed : kept, right = actuator == 0 ? speed : kept;
    std::memcpy(base + ctx.esp + 8, &left, 4);  // the arguments made over, as the original makes them
    std::memcpy(base + ctx.esp + 12, &right, 4);
    x86_icall(ctx, base, cv::kPadRumble);  // and its jump there
}

// Hector's motions keep step with his sprint. His handle's object has his motion controller at +0x104, whose three
// slots (0x4C each from its +0x28; his own when their +0x0C is his object's +0x80) play the motions he blends between:
// +0x2C a motion's clock in 1/256 of its frames, +0x30 how far it goes a frame (256, the motion's own pace). While he
// sprint-walks, the slot playing (its clock moved since his last walking frame) goes at the sprint's pace; it is put
// back at once when he walks without sprinting, and by SprintFrame on a frame he does not walk at all, so no other
// motion of his (an attack, the dodge) plays faster
namespace {

struct SprintPace {
    uint32_t at = 0, was = 0, set = 0;  // a slot's pace changed: where, its value before, the one set
};
SprintPace g_paces[3];
uint32_t g_clocks[3] = {};
bool g_walked = false;  // Hector walked (a walk handler ran for him) since the last SprintFrame

void PaceBack(uint8_t* base, SprintPace& p) {
    if (!p.at) return;
    uint32_t now;
    std::memcpy(&now, base + p.at, 4);
    if (now == p.set) std::memcpy(base + p.at, &p.was, 4);  // else the game has set it since
    p = {};
}

void SprintMotions(uint8_t* base, uint32_t handle, bool sprint) {
    g_walked = true;
    uint32_t ctrl;
    std::memcpy(&ctrl, base + handle + 0x104, 4);
    for (int i = 0; i < 3; i++) {
        SprintPace& p = g_paces[i];
        uint32_t slot = ctrl + 0x28 + uint32_t(i) * 0x4C, owner = 0, clock = 0, pace = 0;
        if (ctrl) std::memcpy(&owner, base + slot + 0x0C, 4);
        if (!ctrl || owner != handle + 0x80 || !sprint) {
            PaceBack(base, p);
            if (ctrl && owner == handle + 0x80) std::memcpy(&g_clocks[i], base + slot + 0x2C, 4);
            continue;
        }
        std::memcpy(&clock, base + slot + 0x2C, 4);
        std::memcpy(&pace, base + slot + 0x30, 4);
        bool playing = clock != g_clocks[i];
        g_clocks[i] = clock;
        if (p.at && pace != p.set) p = {};  // the game set the slot's pace again (a motion started): taken as its own
        if (!playing) continue;
        uint32_t want = uint32_t(float(p.at ? p.was : pace) * SprintFactor() + 0.5f);
        if (p.at && p.set == want) continue;
        if (!p.at) p = {slot + 0x30, pace, 0};
        p.set = want;
        std::memcpy(base + p.at, &want, 4);
    }
}

}  // namespace

// once a frame (warp.cpp's camera update): Hector's motions at their own pace again on a frame he did not walk
void SprintFrame(uint8_t* base) {
    if (!g_walked)
        for (SprintPace& p : g_paces) PaceBack(base, p);
    g_walked = false;
}

// Hector's sprint (keyboard.cpp: sprint, L3 or Left Shift). His walk is one of the game's actor commands: its handler
// takes the command's data (its second argument), whose float at +0x14 is the speed, made a speed a frame through three
// constants (18.519 at full tilt), and sets it as the actor's (the record's +0x4C, sub_0032DD50) through the move that
// goes by the game's collision (sub_0032E770). Two handlers walk: by the stick, and on a course set. Called for
// Hector's handle (the actor's +0x2C, cv::kPlayer's) while he sprints, they take that speed times the sprint's, the
// data put back after, and his walk's motion goes at that pace too (SprintMotions). His guarded walk, the dodge, jumps
// and attacks have handlers and moves of their own, and every other actor's walk is its own: none of them changes, nor
// does the game's clock
struct SprintWalk {
    uint8_t* base;
    uint32_t at = 0;
    float was = 0;
    SprintWalk(const X86Context& ctx, uint8_t* b) : base(b) {
        uint32_t actor, data, handle, player;
        std::memcpy(&actor, base + ctx.esp + 4, 4);
        std::memcpy(&data, base + ctx.esp + 8, 4);
        std::memcpy(&player, base + cv::kPlayer, 4);
        if (!actor || !data || !player) return;
        std::memcpy(&handle, base + actor + 0x2C, 4);
        if (handle != player) return;
        bool sprint = SprintOn();
        SprintMotions(base, handle, sprint);
        if (!sprint) return;
        at = data + 0x14;
        std::memcpy(&was, base + at, 4);
        float faster = was * SprintFactor();
        std::memcpy(base + at, &faster, 4);
    }
    ~SprintWalk() {
        if (at) std::memcpy(base + at, &was, 4);
    }
};

X86_EXTERN_FUNC(CV_ORIGINAL(CV_WALK_STICK));
X86_FUNC(CV_HOOK(CV_WALK_STICK)) {
    SprintWalk sprint(ctx, base);
    CV_ORIGINAL(CV_WALK_STICK)(ctx, base);
}

X86_EXTERN_FUNC(CV_ORIGINAL(CV_WALK));
X86_FUNC(CV_HOOK(CV_WALK)) {
    SprintWalk sprint(ctx, base);
    CV_ORIGINAL(CV_WALK)(ctx, base);
}

// The Lament of Innocence bonus. The PS2 game looked on the memory card for a Lament of Innocence save (BASLUS-20733,
// its name still in the image, sub_0033D590) and, finding one, set flag 0x4002, which a game copies into its own flag
// 0x30008006 (sub_00321860). This is that flag's one reader: a room event (sub_001425E0) that puts a Moai at the far
// end of 5.0, left of the potion there. The Xbox has no PS2 memory card, so the setting answers for it.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_LAMENT_FLAG));
X86_FUNC(CV_HOOK(CV_LAMENT_FLAG)) {
    CV_ORIGINAL(CV_LAMENT_FLAG)(ctx, base);
    if (g_lament_save) ctx.eax = 1;
}

// The name codes: a new game named @CRAZY plays Crazy mode, one named @TREVOR plays as Trevor. The game takes them
// only once it has been beaten: this looks the new game's name up in three {name, flag} pairs at 0x548D9C (none,
// @CRAZY with flag 0x30008004, @TREVOR with 0x30008003) and returns the index of the one it matches whose flag is set,
// else 0, which sub_003219A0 hands to sub_003215E0 to start the game in that mode. Here they work from the start. The
// flags are left alone, so the save records no beaten game.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_NAME_CODE));
X86_FUNC(CV_HOOK(CV_NAME_CODE)) {
    uint32_t name;
    std::memcpy(&name, base + ctx.esp + 4, 4);
    CV_ORIGINAL(CV_NAME_CODE)(ctx, base);
    if (ctx.eax || !name) return;
    for (uint32_t i = 1; i < 3; i++) {
        uint32_t code;
        std::memcpy(&code, base + cv::kNameCodes + i * 8, 4);
        if (code && !std::strcmp(reinterpret_cast<const char*>(base + code), reinterpret_cast<const char*>(base + name))) {
            ctx.eax = i;
            return;
        }
    }
}
