// What the port's code finds in default.xbe, by the release this executable was recompiled from: the USA one
// (Castlevania), or with CV_EU the European one (CastlevaniaEU), the same game built later for five languages, its
// code and data a little shifted. The European addresses were found by matching that xbe to the USA one
// (tools/release_map.py): functions by their bytes with addresses and call offsets masked, data by what the
// instructions of matched functions name. The few only reached through pointers (the attributes, gold, the camera's
// view) were checked in play: the attributes with HP 100 of 100 at a new game, gold and both against the USA
// release's memory at the same moment, byte for byte, the view by the camera's position. A new hook needs both
// addresses: release_map.py find gives the European one.
#pragma once

#include <cstdint>

#ifdef CV_EU
#define CV_PICK(usa, eu) eu
#else
#define CV_PICK(usa, eu) usa
#endif

// a recompiled function in a hook: CV_HOOK(f) the hook's name, CV_ORIGINAL(f) the original it calls
#define CV_HOOK(f) CV_PASTE(sub_, f)
#define CV_ORIGINAL(f) CV_PASTE(__imp__sub_, f)
#define CV_PASTE(a, b) CV_PASTE_(a, b)
#define CV_PASTE_(a, b) a##b

// functions, by their 8 hex digits
#define CV_CAMERA_MATRICES CV_PICK(00012CD0, 00012D70)  // the camera's matrices made again (hooks.cpp)
#define CV_LAMENT_FLAG CV_PICK(0033E1D0, 0033FD60)      // flag 0x30008006's reader (hooks.cpp)
#define CV_NAME_CODE CV_PICK(002F7030, 002F87E0)        // a new game's name looked up in the name codes (hooks.cpp)
#define CV_MAX_HP CV_PICK(00060010, 000601E0)           // an actor's max HP (enemies.cpp)
#define CV_CAMERA_UPDATE CV_PICK(003623B0, 00364330)    // the camera update each frame, where a warp is sent (warp.cpp)
#define CV_SCENE_FREE CV_PICK(002F6AB0, 002F8130)       // no scene change locked or waiting
#define CV_SET_ROOM CV_PICK(002F6950, 002F7FD0)         // the room a change goes to
#define CV_SET_FLAG CV_PICK(002F62E0, 002F7960)         // a game flag
#define CV_CHANGE_ROOM CV_PICK(002F6AD0, 002F8150)      // the room change

namespace cv {

inline constexpr bool kEurope = CV_PICK(false, true);

// the camera's clip matrices (hooks.cpp)
inline constexpr uint32_t kClipScreenX = CV_PICK(0x012FC500, 0x01303E80);  // clip screen matrix, x scale
inline constexpr uint32_t kViewClipX = CV_PICK(0x012FC5C0, 0x01303F40);    // view clip matrix, x scale
inline constexpr uint32_t kViewClipXv = CV_PICK(0x012FC640, 0x01303FC0);   // its x scale as a vector
inline constexpr uint32_t kProjectionXv = CV_PICK(0x012FC660, 0x01303FE0); // the projection's x scale as a vector
// the name codes, three {name, flag} pairs (hooks.cpp)
inline constexpr uint32_t kNameCodes = CV_PICK(0x548D9C, 0x54C8D4);
// int8 map, int8 room; where a change goes; the scene change locks (telemetry.cpp, warp.cpp, enemies.cpp)
inline constexpr uint32_t kMapRoom = CV_PICK(0xB234F4, 0xB2AE4C);
inline constexpr uint32_t kNextRoom = CV_PICK(0xB234F8, 0xB2AE50);
inline constexpr uint32_t kChangeLock = CV_PICK(0xB23528, 0xB2AE80);
inline constexpr uint32_t kChangeWait = CV_PICK(0xB2352C, 0xB2AE84);
// Hector: his handle, the records it indexes, his attributes (int32 each: 3 EXP, 5 max HP, 6 HP, 31 max MP, 32 MP),
// his gold
inline constexpr uint32_t kPlayer = CV_PICK(0xB41394, 0xB48CF4);
inline constexpr uint32_t kRecords = CV_PICK(0xB4129C, 0xB48BFC);
inline constexpr uint32_t kAttributes = CV_PICK(0xB2CCC0, 0xB34620);
inline constexpr uint32_t kGold = CV_PICK(0xB35204, 0xB3CB64);
// the camera: its view, the descriptor whose callback tells a title or map screen, and those callbacks
inline constexpr uint32_t kCameraView = CV_PICK(0xC36B74, 0xC3E4E4);
inline constexpr uint32_t kCameraDescriptor = CV_PICK(0xC36B70, 0xC3E4E0);
inline constexpr uint32_t kTitleCallback = CV_PICK(0x345020, 0x346C70);
inline constexpr uint32_t kDebugCallback = CV_PICK(0x4E7D0, 0x4E7F0);
// a story scene or a room change under way; the pause or inventory menu; a message box's flags, through a pointer
inline constexpr uint32_t kScene = CV_PICK(0xB23410, 0xB2AD68);
inline constexpr uint32_t kMenu = CV_PICK(0xB25518, 0xB2CE78);
inline constexpr uint32_t kModalFlags = CV_PICK(0x548C10, 0x54C748);
// the game: its mode (0 Hector, 1 Crazy, 2 Trevor) and the save's name 0x10 on, what the getter there returns
inline constexpr uint32_t kGameInfo = CV_PICK(0xB23700, 0xB2B060);
// the room table the game writes (telemetry.cpp)
inline constexpr uint32_t kRoomTable = CV_PICK(0x55BA00, 0x55F840);

}  // namespace cv
