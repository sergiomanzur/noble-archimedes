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
#include <cstring>

#include "app/app.h"
#include "common/release.h"
#include "cpu/x86_ops.h"

extern bool g_lament_save;  // menu.cpp

namespace {

constexpr float kWiden = (4.0f / 3.0f) / (16.0f / 9.0f);

void Scale(uint8_t* base, uint32_t va, float k) {
    float v;
    std::memcpy(&v, base + va, 4);
    v *= k;
    std::memcpy(base + va, &v, 4);
}

}  // namespace

// The camera's matrices, made again when the camera changed (a PS2 style libvu0 camera: sceVu0ViewScreenMatrix and
// clip matrices). Widescreen draws more at the sides (widescreen.cpp), so the clip data objects are culled against
// is made wider by the same 4:3 to 16:9; the matrices the scene is drawn with are left alone.
X86_EXTERN_FUNC(CV_ORIGINAL(CV_CAMERA_MATRICES));
X86_FUNC(CV_HOOK(CV_CAMERA_MATRICES)) {
    CV_ORIGINAL(CV_CAMERA_MATRICES)(ctx, base);
    if (!app::Widescreen()) return;
    Scale(base, cv::kClipScreenX, kWiden);
    Scale(base, cv::kViewClipX, kWiden);
    Scale(base, cv::kViewClipXv, kWiden);
    Scale(base, cv::kProjectionXv, kWiden);
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
