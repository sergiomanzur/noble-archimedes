// The view: field of view, draw distance and fog, settings of the in-game menu and the launcher (menu.cpp).
//
// The camera is a PS2 libvu0 one. Its parameters are ints at 0x012FC214 and on, each with a setter that also marks
// the camera's matrices to be made again (the byte at 0x012FC7B2), which sub_00012CD0 then makes from them: the view
// screen matrix (sceVu0ViewScreenMatrix, sub_00386B80), the view clip matrix objects are culled against, and the fog's.
//   screen distance  0x012FC214, sub_00012B30. Every frame of play the camera update (sub_00361410) hands the active
//                    camera's own to it (sub_003623F0: the int at camera + 0x154; 615 in the first courtyard, 700 in
//                    the room after it). With the y scale of 448 / 384, 224 / (d * 448 / 384) is the tan of half the
//                    vertical field of view: 35 and 31 degrees there. A shorter distance is a wider view, for the clip
//                    matrix too, so what comes into view at the sides is drawn rather than culled.
//   far z            0x012FC22C, sub_00012B90: 200000 at the start; a room sets its own as it loads (sub_00361710 and
//                    sub_00361730: 24000, then 19600 in the first courtyard, 8200 in the room after it), where its fog
//                    has hidden everything. Past it nothing is drawn.
//   fog              sub_00012A90(near, far), as a room loads (sub_002FBF38: 10000 to 19600 in the first courtyard),
//                    kept at 0x012FC1F0 and 0x012FC1F4, its coefficients at 0x012FC1F8 (near / (far - near) * -128) and
//                    0x012FC1FC (far * near / (far - near) * 128): 128 * near * (far / z - 1) / (far - near) in the
//                    game's programs (c91), none at near and all of it at far. Both distances times m push the same
//                    fog m times as far out; past the far plane it is gone.
// The game's own values are kept: a setting changed in the menu is applied again from them by the camera update
// (ViewFrame, game/warp.cpp), on the game's thread. CV_VIEW_TRACE logs the setters as they run. The addresses here are
// the USA release's; common/release.h has both (game_eu/view.cpp compiles this for the European one).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/release.h"
#include "core/log.h"
#include "cpu/x86_ops.h"

extern uint32_t g_fov;            // menu.cpp: percent of the game's field of view (of the tan of its half angle)
extern uint32_t g_draw_distance;  // menu.cpp: percent of the game's far plane
extern uint32_t g_fog_distance;   // menu.cpp: percent of the game's fog distance, 0 for no fog

namespace {

using cv::kFarZ, cv::kFogA, cv::kFogB, cv::kMakeAgain, cv::kScreenDistance, cv::kYScale;
using cv::kScene;  // bit 17 while a story scene plays, bit 15 while a room loads (telemetry.cpp)
// no fog: both distances out past any far plane (24000 times 4 at most)
constexpr int32_t kNoFogNear = 0x20000000, kNoFogFar = 0x3FFFFFFF;
constexpr double kPi = 3.14159265358979;

template <typename T>
T Get(const uint8_t* base, uint32_t va) {
    T v;
    std::memcpy(&v, base + va, sizeof(v));
    return v;
}
template <typename T>
void Put(uint8_t* base, uint32_t va, T v) {
    std::memcpy(base + va, &v, sizeof(v));
}

int32_t Times(int32_t v, uint32_t percent) {
    return int32_t(std::clamp<long long>(std::llround(double(v) * percent / 100.0), 1, 0x3FFFFFFF));
}

struct View {
    uint8_t* base = nullptr;
    int32_t screen = 0, far_z = 0;  // the game's own, as it last set them
    int32_t fog_near = 0, fog_far = 0;
    bool fog_set = false;
    uint32_t far_made = 100, fog_made = 100;  // the settings they were last applied with
} g_view;

// for the renderer's thread (ViewBackdrop): the game's own fog coefficients and far plane, those drawn, and a count
// of the times a room set them
struct Shared {
    std::atomic<float> game_a{0}, game_b{0}, drawn_a{0}, drawn_b{0}, far_z{0};
    std::atomic<uint32_t> room{0};
} g_shared;

void Coefficients(int32_t n, int32_t f, float& a, float& b) {
    float span = float(f - n), inv = span != 0.0f ? 1.0f / span : 1.0f;
    a = float(n) * inv * -128.0f;
    b = float(f) * float(n) * inv * 128.0f;
}

void Share(const uint8_t* base) {
    float a, b;
    Coefficients(g_view.fog_near, g_view.fog_far, a, b);
    g_shared.game_a = a;
    g_shared.game_b = b;
    g_shared.drawn_a = Get<float>(base, kFogA);
    g_shared.drawn_b = Get<float>(base, kFogB);
    g_shared.far_z = float(g_view.far_z);
}

// the fog's coefficients as sub_00012A90 makes them, from the distances pushed out
void PutFog(uint8_t* base) {
    int32_t n = g_view.fog_near, f = g_view.fog_far;
    if (g_fog_distance == 0) {
        n = kNoFogNear;
        f = kNoFogFar;
    } else if (g_fog_distance != 100) {
        n = Times(n, g_fog_distance);
        f = Times(f, g_fog_distance);
    }
    float a, b;
    Coefficients(n, f, a, b);
    Put<float>(base, kFogA, a);
    Put<float>(base, kFogB, b);
}

void PutFar(uint8_t* base) {
    Put<int32_t>(base, kFarZ, g_draw_distance == 100 ? g_view.far_z : Times(g_view.far_z, g_draw_distance));
}

const bool kTrace = std::getenv("CV_VIEW_TRACE") != nullptr;
bool Traced(uint32_t& n) { return kTrace && (n++ < 8 || n % 300 == 0); }
uint32_t Caller(const X86Context& ctx, const uint8_t* base) { return Get<uint32_t>(base, ctx.esp); }

}  // namespace

// each frame of play, from the camera update: settings changed since the game last set its values
void ViewFrame(uint8_t* base) {
    g_view.base = base;
    bool make = false;
    if (g_view.far_z && g_view.far_made != g_draw_distance) {
        g_view.far_made = g_draw_distance;
        PutFar(base);
        make = true;
    }
    if (g_view.fog_set && g_view.fog_made != g_fog_distance) {
        g_view.fog_made = g_fog_distance;
        PutFog(base);
        make = true;
    }
    if (make) Put<uint8_t>(base, kMakeAgain, 1);
    if (make) Share(base);
}

// for the renderer extension (widescreen.cpp): the fog coefficients the game set for the room, those it is drawn with
// and its far plane; false while the room is drawn as the game draws it
bool ViewBackdrop(float game[2], float drawn[2], float& far_z, uint32_t& room) {
    if (g_fog_distance == 100 && g_draw_distance == 100) return false;
    game[0] = g_shared.game_a;
    game[1] = g_shared.game_b;
    drawn[0] = g_shared.drawn_a;
    drawn[1] = g_shared.drawn_b;
    far_z = g_shared.far_z;
    room = g_shared.room;
    return far_z > 0 && game[1] != 0;
}

// the camera's matrices about to be made (hooks.cpp, sub_00012CD0), traced
void ViewMake(const X86Context& ctx, const uint8_t* base) {
    static uint32_t n = 0;
    if (Traced(n))
        LOGR("view %u: matrices made for %08x: screen distance %d far %d fog %g %g", n, Caller(ctx, base),
             Get<int32_t>(base, kScreenDistance), Get<int32_t>(base, kFarZ), Get<float>(base, kFogA),
             Get<float>(base, kFogB));
}

// for the in-game menu: the vertical field of view, in degrees, of the game's camera now and as it is drawn
bool ViewFov(double& game, double& drawn) {
    const uint8_t* base = g_view.base;
    if (!base || g_view.screen <= 0) return false;
    double y = Get<int32_t>(base, kYScale) / 384.0, d = Get<int32_t>(base, kScreenDistance);
    if (y <= 0 || d <= 0) return false;
    game = 2 * std::atan(224 / (g_view.screen * y)) * 180 / kPi;
    drawn = 2 * std::atan(224 / (d * y)) * 180 / kPi;
    return true;
}

// for tests (telemetry.cpp: "view"), as the game set them and as they are drawn: the vertical field of view in degrees,
// the far plane, the fog's coefficient b (128 * near * far / (far - near), which the fog's distances scale)
std::string ViewJson() {
    const uint8_t* base = g_view.base;
    if (!base) return "";
    double fov_game = 0, fov_drawn = 0;
    ViewFov(fov_game, fov_drawn);
    float a, b;
    Coefficients(g_view.fog_near, g_view.fog_far, a, b);
    char out[256];
    std::snprintf(out, sizeof(out), "{\"fov\":[%.2f,%.2f],\"far\":[%d,%d],\"fog\":[%.6g,%.6g]}", fov_game,
                  fov_drawn, g_view.far_z, Get<int32_t>(base, kFarZ), g_view.fog_set ? b : 0.0f, Get<float>(base, kFogB));
    return out;
}

// the screen distance, each frame of play from the active camera: shorter for a wider field of view, but in story
// scenes, which keep the shots they were framed with. A room change's loading (bit 15) is no exception: the bit is
// still set while the new room fades in, which showed the game's field of view until it went.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_SCREEN_DISTANCE));
X86_FUNC(CV_HOOK(CV_SCREEN_DISTANCE)) {
    uint32_t at = ctx.esp + 4;
    int32_t d = Get<int32_t>(base, at);
    g_view.screen = d;
    uint32_t fov = std::clamp<uint32_t>(g_fov, 50, 300);
    uint32_t word = Get<uint32_t>(base, kScene);
    bool scene = (word & 0x20000) != 0;
    int32_t used = fov == 100 || scene || d <= 0 ? d : std::max<int32_t>(1, int32_t(std::lround(d * 100.0 / fov)));
    static uint32_t n = 0;
    static int32_t traced = -1;
    if (kTrace && used != traced && n++ < 400) {
        traced = used;
        LOGR("view %u: screen distance %d, drawn with %d, scene %x, from %08x", n, d, used, word, Caller(ctx, base));
    }
    Put<int32_t>(base, at, used);
    CV_ORIGINAL(CV_SCREEN_DISTANCE)(ctx, base);
    Put<int32_t>(base, at, d);  // the caller's argument as it was
}

// the far plane
X86_EXTERN_FUNC(CV_ORIGINAL(CV_FAR_Z));
X86_FUNC(CV_HOOK(CV_FAR_Z)) {
    int32_t z = Get<int32_t>(base, ctx.esp + 4);  // read before the original's ret moves esp
    static uint32_t t = 0;
    if (Traced(t)) LOGR("view %u: far %d from %08x", t, z, Caller(ctx, base));
    CV_ORIGINAL(CV_FAR_Z)(ctx, base);
    g_view.far_z = z;
    g_view.far_made = g_draw_distance;
    if (g_draw_distance != 100) PutFar(base);
    g_shared.room++;
    Share(base);
}

// the fog: the game's distances stay where it keeps them (sub_00012B10 sets the fog again from those), the
// coefficients its programs take are the pushed out ones
X86_EXTERN_FUNC(CV_ORIGINAL(CV_FOG));
X86_FUNC(CV_HOOK(CV_FOG)) {
    int32_t n = Get<int32_t>(base, ctx.esp + 4), f = Get<int32_t>(base, ctx.esp + 8);
    static uint32_t t = 0;
    if (Traced(t)) LOGR("view %u: fog %d to %d from %08x", t, n, f, Caller(ctx, base));
    CV_ORIGINAL(CV_FOG)(ctx, base);
    g_view.fog_near = n;
    g_view.fog_far = f;
    g_view.fog_set = true;
    g_view.fog_made = g_fog_distance;
    if (g_fog_distance != 100) PutFog(base);
    g_shared.room++;
    Share(base);
}
