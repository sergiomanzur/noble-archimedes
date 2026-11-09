// Modern PC controls for the imported Xbox build.
//
// Mouse look owns an absolute yaw/pitch heading. The title still supplies the
// player target, a collision-informed distance and scripted camera changes; it
// cannot drive the heading back into a feedback turn. The angle changes only
// when the mouse (or the controller's right stick) moves, so the camera stops
// immediately instead of coasting on a stick budget.
#include "common/modern_controls.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>
#include <imgui.h>

#include "app/app.h"
#include "common/modern_camera_math.h"
#include "common/release.h"
#include "core/input.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

// keyboard.cpp owns the pad read; menu.cpp reports when its UI is open.
bool PlayerPose(float pos[3], float& yaw);  // telemetry.cpp
bool PauseMenuUp();                         // telemetry.cpp
bool ModalUp();                             // telemetry.cpp
uint32_t SceneWord();                       // telemetry.cpp
struct EnemyInfo {
    uint32_t actor;
    uint16_t type;
    float hp;
    float pos[3];
};
std::vector<EnemyInfo> Enemies();            // game/enemies.cpp

namespace modern {
namespace {

constexpr uint32_t kSceneMask = 0x28000;  // story scene or room change (telemetry.cpp)
constexpr float kMinDistance = 120.0f;
constexpr float kMaxPitch = 1.10f;  // radians above/below horizontal
constexpr float kStickRadiansPerFrame = 0.025f;  // gamepad look, ~1.5 rad/s at 60 Hz

std::mutex g_input_mutex;
std::mutex g_hotkey_mutex;
std::atomic<bool> g_captured{false};
bool g_first_motion = true;
float g_classic_mouse_x = 0.0f, g_classic_mouse_y = 0.0f;
uint32_t g_last_frame = ~0u;
std::atomic<float> g_camera_yaw{0.0f};   // absolute view heading, 0 = +z
std::atomic<float> g_camera_pitch{0.0f}; // positive looks down
std::atomic<float> g_base_heading{0.0f}; // title camera heading used by movement
std::atomic<float> g_move_amount{0.0f};  // last left-stick magnitude, 0..1
std::atomic<bool> g_camera_need_init{true};
std::atomic<bool> g_aiming{false};

std::atomic<int> g_mode{kOverShoulder};
std::atomic<float> g_sensitivity{0.15f};  // degrees per mouse pixel
std::atomic<bool> g_invert_x{false};      // mouse only
std::atomic<bool> g_invert_y{false};      // mouse only
std::atomic<bool> g_stick_invert_x{true}; // correct the title's inverted camera stick
std::atomic<bool> g_stick_invert_y{true};
constexpr int kDefaultStickDeadzone = 10;
std::atomic<int> g_move_deadzone{kDefaultStickDeadzone};
std::atomic<int> g_camera_deadzone{kDefaultStickDeadzone};
std::atomic<float> g_shoulder{120.0f};    // guest units to the right
std::atomic<float> g_distance{620.0f};    // camera distance from Hector in OTS mode
std::atomic<float> g_first_person{0.0f};  // optional forward offset in FPS mode
std::atomic<bool> g_capture{true};
std::atomic<bool> g_gameplay{false};

float Clamp(float value, float lo, float hi) {
    return !std::isfinite(value) ? lo : std::max(lo, std::min(hi, value));
}

bool ReadU32(uint8_t* base, uint32_t va, uint32_t& out) {
    if (!mem::IsCommitted(va) || !mem::IsCommitted(va + 3)) return false;
    std::memcpy(&out, base + va, 4);
    return true;
}

bool CameraSpecial(uint8_t* base) {
    uint32_t descriptor = 0, callback = 0;
    if (!ReadU32(base, cv::kCameraDescriptor, descriptor) || !descriptor) return false;
    if (!ReadU32(base, descriptor + 0x1D0, callback)) return false;
    return callback == cv::kTitleCallback || callback == cv::kDebugCallback;
}

bool GameplayCamera(uint8_t* base, float* player_position = nullptr, float* player_yaw = nullptr) {
    float pos[3], yaw = 0;
    if (!PlayerPose(pos, yaw)) return false;
    if (SceneWord() & kSceneMask) return false;
    if (PauseMenuUp() || ModalUp()) return false;
    if (CameraSpecial(base)) return false;
    if (player_position) std::memcpy(player_position, pos, sizeof pos);
    if (player_yaw) *player_yaw = yaw;
    return true;
}

// The title keeps the player's facing in the same record as the telemetry
// position. Tying it to the absolute camera heading makes W follow where the
// mouse is pointing instead of continuing along the last character heading.
void FaceHeading(uint8_t* base, float yaw) {
    uint32_t player = 0, record = 0;
    if (!ReadU32(base, cv::kPlayer, player) || !player) return;
    if (!ReadU32(base, player, record) || record >= 128) return;
    uint32_t yaw_address = cv::kRecords + record * 0x1210 + 0x2C;
    if (!mem::IsCommitted(yaw_address) || !mem::IsCommitted(yaw_address + 3)) return;
    if (!std::isfinite(yaw)) return;
    std::memcpy(base + yaw_address, &yaw, sizeof yaw);
}

void CaptureWindow(HWND window) {
    if (!window || !IsWindow(window) || !IsWindowVisible(window)) return;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right <= 1 || client.bottom <= 1) return;
    POINT center{client.right / 2, client.bottom / 2};
    if (!ClientToScreen(window, &center) || !SetCursorPos(center.x, center.y)) return;
    g_first_motion = true;
    g_captured = true;
}

void ReleaseCapture() {
    g_classic_mouse_x = g_classic_mouse_y = 0.0f;
    if (g_captured) {
        g_captured = false;
        g_first_motion = true;
        g_aiming = false;
    }
}

void ReadRelativeMouse(HWND window, float& dx, float& dy) {
    dx = dy = 0.0f;
    if (!window || !IsWindow(window) || !g_captured) return;
    RECT client{};
    if (!GetClientRect(window, &client)) return;
    if (client.right <= 1 || client.bottom <= 1) return;
    POINT center{client.right / 2, client.bottom / 2};
    ClientToScreen(window, &center);
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    if (!g_first_motion) {
        dx = static_cast<float>(cursor.x - center.x);
        dy = static_cast<float>(cursor.y - center.y);
    }
    g_first_motion = false;
    if (cursor.x != center.x || cursor.y != center.y) SetCursorPos(center.x, center.y);
}

void ModeChanged() {
    // Re-seed the absolute heading from the title camera on the next frame.
    g_camera_need_init = true;
}

void WriteView(uint8_t* base, uint32_t view, const float* target, const float* position, const float* matrix) {
    if (!view || !mem::IsCommitted(view + 0xFF)) return;
    float* v = reinterpret_cast<float*>(base + view);
    // Telemetry reads +0x30 target and +0x40 position; the title uses +0x00
    // and +0x80 as additional copies in some camera paths.
    std::memcpy(v + 0, target, sizeof(float) * 4);
    std::memcpy(v + 12, target, sizeof(float) * 4);
    std::memcpy(v + 16, position, sizeof(float) * 4);
    std::memcpy(v + 32, target, sizeof(float) * 4);
    std::memcpy(v + 36, matrix, sizeof(float) * 16);
    // +0x70 is the negative forward vector in the retail view structure. The
    // forward row of the column-major rotation is (M2, M6, M10).
    v[0x70 / 4 + 0] = -matrix[2];
    v[0x70 / 4 + 1] = -matrix[6];
    v[0x70 / 4 + 2] = -matrix[10];
    v[0x70 / 4 + 3] = 0.0f;
}

}  // namespace

void LoadSettings(const void* table) {
    if (!table) return;
    const toml::table& t = *static_cast<const toml::table*>(table);
    int mode = t["modern_camera_mode"].value_or(g_mode.load());
    g_mode = mode >= kClassic && mode <= kFirstPerson ? mode : kOverShoulder;
    g_sensitivity = Clamp(t["modern_mouse_sensitivity"].value_or(g_sensitivity.load()), 0.01f, 1.0f);
    g_invert_x = t["modern_invert_x"].value_or(g_invert_x.load());
    g_invert_y = t["modern_invert_y"].value_or(g_invert_y.load());
    g_stick_invert_x = t["modern_invert_stick_x"].value_or(g_stick_invert_x.load());
    g_stick_invert_y = t["modern_invert_stick_y"].value_or(g_stick_invert_y.load());
    g_move_deadzone = std::clamp(t["move_stick_deadzone"].value_or(kDefaultStickDeadzone), 0, 50);
    g_camera_deadzone = std::clamp(t["camera_stick_deadzone"].value_or(kDefaultStickDeadzone), 0, 50);
    g_shoulder = Clamp(t["modern_shoulder_offset"].value_or(g_shoulder.load()), 0.0f, 350.0f);
    g_distance = Clamp(t["modern_camera_distance"].value_or(g_distance.load()), kMinDistance, 1600.0f);
    g_first_person = Clamp(t["modern_first_person_offset"].value_or(g_first_person.load()), 0.0f, 400.0f);
    g_capture = t["modern_mouse_capture"].value_or(g_capture.load());
}

void SaveSettings(void* table) {
    if (!table) return;
    toml::table& t = *static_cast<toml::table*>(table);
    t.insert_or_assign("modern_camera_mode", int64_t(g_mode.load()));
    t.insert_or_assign("modern_mouse_sensitivity", double(g_sensitivity.load()));
    t.insert_or_assign("modern_invert_x", g_invert_x.load());
    t.insert_or_assign("modern_invert_y", g_invert_y.load());
    t.insert_or_assign("modern_invert_stick_x", g_stick_invert_x.load());
    t.insert_or_assign("modern_invert_stick_y", g_stick_invert_y.load());
    t.insert_or_assign("move_stick_deadzone", int64_t(g_move_deadzone.load()));
    t.insert_or_assign("camera_stick_deadzone", int64_t(g_camera_deadzone.load()));
    t.insert_or_assign("modern_shoulder_offset", double(g_shoulder.load()));
    t.insert_or_assign("modern_camera_distance", double(g_distance.load()));
    t.insert_or_assign("modern_first_person_offset", double(g_first_person.load()));
    t.insert_or_assign("modern_mouse_capture", g_capture.load());
}

void Frame(bool focused, bool menu_open, bool scripted, uint32_t frame) {
    static const bool registered = [] {
        std::atexit(Shutdown);
        return true;
    }();
    (void)registered;
    static bool f5_was = false, f6_was = false;
    HWND window = static_cast<HWND>(app::Window());
    bool gameplay = focused && !menu_open && !scripted && GameplayCamera(reinterpret_cast<uint8_t*>(mem::Base()));
    g_gameplay = gameplay;

    bool f5 = focused && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    bool f6 = focused && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    {
        std::lock_guard hotkey_lock(g_hotkey_mutex);
        if (f5 && !f5_was) CycleCameraMode();
        if (f6 && !f6_was) ToggleMouseCapture();
        f5_was = f5;
        f6_was = f6;
    }

    std::lock_guard lock(g_input_mutex);
    bool want_capture = gameplay && g_capture.load();
    if (!want_capture) {
        ReleaseCapture();
        return;
    }
    if (!g_captured) CaptureWindow(window);
    if (!g_captured) return;
    if (frame == g_last_frame) return;
    g_last_frame = frame;

    float dx = 0.0f, dy = 0.0f;
    ReadRelativeMouse(window, dx, dy);
    dx = Clamp(dx, -4000.0f, 4000.0f);
    dy = Clamp(dy, -4000.0f, 4000.0f);
    // Direct look: mouse travel changes the camera angle exactly once. No
    // residual stick value keeps turning after the mouse stops.
    float angular = g_sensitivity.load() * math::kDegToRad;
    g_aiming = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (g_mode.load() == kClassic) {
        // Feed relative mouse motion through the game's own orbit controls.
        g_classic_mouse_x = Clamp(dx * angular / kStickRadiansPerFrame, -1.0f, 1.0f);
        g_classic_mouse_y = Clamp(-dy * angular / kStickRadiansPerFrame, -1.0f, 1.0f);
        return;
    }
    if (dx != 0.0f || dy != 0.0f) {
        float yaw = g_camera_yaw.load() + dx * angular * (g_invert_x.load() ? -1.0f : 1.0f);
        while (yaw > math::kPi) yaw -= 2.0f * math::kPi;
        while (yaw < -math::kPi) yaw += 2.0f * math::kPi;
        float pitch = g_camera_pitch.load() + dy * angular * (g_invert_y.load() ? -1.0f : 1.0f);
        g_camera_yaw = yaw;
        g_camera_pitch = Clamp(pitch, -kMaxPitch, kMaxPitch);
    }
}

void ApplyPad(input::Pad& pad) {
    std::lock_guard lock(g_input_mutex);
    // The title's right stick still works; fold it into the same absolute
    // heading instead of letting the title camera's own follow change it.
    uint32_t frame = d3d::Swaps();
    static uint32_t stick_frame = ~0u;
    const int mode = g_mode.load();
    if (mode != kClassic && !g_gameplay.load()) return;
    g_move_amount = std::max(std::fabs(static_cast<float>(pad.lx)), std::fabs(static_cast<float>(pad.ly))) /
                    32767.0f;
    if (mode == kClassic) {
        // The title's raw camera stick is inverted. These default-on flips
        // correct it without changing the original camera itself.
        const bool mouse = g_captured && g_gameplay.load();
        pad.rx = math::ClassicAxis(pad.rx, mouse ? g_classic_mouse_x : 0.0f, g_stick_invert_x, g_invert_x);
        pad.ry = math::ClassicAxis(pad.ry, mouse ? g_classic_mouse_y : 0.0f, g_stick_invert_y, g_invert_y);
    } else {
        if (frame != stick_frame && (pad.rx != 0 || pad.ry != 0)) {
            float rx = (pad.rx / 32767.0f) * (g_stick_invert_x.load() ? 1.0f : -1.0f);
            float ry = (pad.ry / 32767.0f) * (g_stick_invert_y.load() ? 1.0f : -1.0f);
            float yaw = g_camera_yaw.load() + rx * kStickRadiansPerFrame;
            while (yaw > math::kPi) yaw -= 2.0f * math::kPi;
            while (yaw < -math::kPi) yaw += 2.0f * math::kPi;
            float pitch = g_camera_pitch.load() - ry * kStickRadiansPerFrame;
            g_camera_yaw = yaw;
            g_camera_pitch = Clamp(pitch, -kMaxPitch, kMaxPitch);
        }
        stick_frame = frame;

        // Steer the title camera toward the host heading. Movement is
        // relative to this camera, so keeping it aligned makes W follow
        // the mouse without bypassing the title's own movement rules.
        float base = g_base_heading.load();
        float yaw = g_camera_yaw.load();
        if (mode == kFirstPerson && !g_camera_need_init.load()) {
            math::RebaseMovement(pad.lx, pad.ly, yaw, base);
        }
        float error = yaw - base;
        while (error > math::kPi) error -= 2.0f * math::kPi;
        while (error < -math::kPi) error += 2.0f * math::kPi;
        float command = Clamp(error * 2.0f, -1.0f, 1.0f);
        pad.rx = static_cast<int16_t>(Clamp(-command, -1.0f, 1.0f) * 32767.0f);
        pad.ry = 0;
    }
    if (!g_captured) return;
    // The guest's analog array is A, B, X, Y, black, white, LT, RT.
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) pad.analog[2] = 0xFF;   // X: attack
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) pad.analog[3] = 0xFF;   // Y: special
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) pad.analog[4] = 0xFF;  // black
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) pad.analog[5] = 0xFF;  // white
    if (g_aiming.load()) {
        pad.lx = static_cast<int16_t>(pad.lx * 0.55f);
        pad.ly = static_cast<int16_t>(pad.ly * 0.55f);
    }
}

void AfterCameraUpdate(uint8_t* base) {
    if (!base) return;
    const int mode = g_mode.load();
    if (mode == kClassic) return;

    // A story scene, room change or special view should reset the absolute
    // heading when normal play resumes. Menus and message boxes do not.
    if ((SceneWord() & kSceneMask) || CameraSpecial(base)) g_camera_need_init = true;
    float player_position[3], player_yaw = 0;
    if (!GameplayCamera(base, player_position, &player_yaw)) return;

    uint32_t view = 0;
    if (!ReadU32(base, cv::kCameraView, view) || !view) return;
    if (!mem::IsCommitted(view) || !mem::IsCommitted(view + 0xFF)) return;
    float* v = reinterpret_cast<float*>(base + view);

    float target[4], position[4], matrix[16];
    std::memcpy(target, v + 12, sizeof(target));      // +0x30
    std::memcpy(position, v + 16, sizeof(position));  // +0x40
    std::memcpy(matrix, v + 36, sizeof(matrix));      // +0x90
    for (float value : target)
        if (!std::isfinite(value)) return;
    for (float value : position)
        if (!std::isfinite(value)) return;
    for (float value : matrix)
        if (!std::isfinite(value)) return;

    float base_forward[3] = {matrix[2], matrix[6], matrix[10]};
    float base_delta[3] = {position[0] - target[0], position[1] - target[1], position[2] - target[2]};
    float base_distance = math::Length(base_delta);
    math::Normalize(base_forward);
    if (!(base_distance > 1.0f) || !std::isfinite(base_distance)) return;
    float base_heading = std::atan2(base_forward[0], base_forward[2]);
    while (base_heading > math::kPi) base_heading -= 2.0f * math::kPi;
    while (base_heading < -math::kPi) base_heading += 2.0f * math::kPi;
    g_base_heading = base_heading;

    // Seed the absolute heading from the title camera once per scene. After
    // that the saved angles own the heading, so the title camera's own follow
    // cannot drive the character and camera into a feedback turn.
    if (g_camera_need_init.load()) {
        while (player_yaw > math::kPi) player_yaw -= 2.0f * math::kPi;
        while (player_yaw < -math::kPi) player_yaw += 2.0f * math::kPi;
        float pitch = std::asin(Clamp(base_forward[1], -1.0f, 1.0f));
        g_camera_yaw = player_yaw;
        g_camera_pitch = Clamp(pitch, -kMaxPitch, kMaxPitch);
        g_camera_need_init = false;
    }

    const float yaw = g_camera_yaw.load();
    const float pitch = g_camera_pitch.load();
    float right[3], up[3], forward[3];
    math::BasisFromAngles(yaw, pitch, right, up, forward);
    // While moving, let the title turn the character toward the (already
    // camera-relative) stick direction. While idle, face the camera so the
    // model turns with the mouse.
    if (g_move_amount.load() < 0.05f) FaceHeading(base, yaw);

    float next_position[4];
    if (mode == kFirstPerson) {
        // The title's follow target trails Hector during a run. Anchor to his
        // live position and keep head clearance independent of look pitch.
        float distance = math::kHeadClearance + g_first_person.load();
        for (const auto& enemy : Enemies())
            distance = math::EnemyEyeLimit(player_position, enemy.pos, yaw, distance);
        math::FirstPersonEye(player_position, yaw, distance, next_position);
    } else {
        const bool aiming = g_aiming.load();
        float wanted = std::min(g_distance.load(), base_distance);
        if (aiming) wanted *= 0.72f;
        float shoulder = g_shoulder.load() * (aiming ? 1.2f : 1.0f);
        math::OrbitPosition(target, forward, right, wanted, shoulder, next_position);
    }
    next_position[3] = 1.0f;
    float next_target[4] = {next_position[0] + forward[0] * 1000.0f,
                            next_position[1] + forward[1] * 1000.0f,
                            next_position[2] + forward[2] * 1000.0f, 1.0f};
    float next_matrix[16];
    math::ViewMatrix(right, up, forward, next_position, next_matrix);
    WriteView(base, view, next_target, next_position, next_matrix);
}

void SetMenuOpen(bool open) {
    if (!open) return;
    std::lock_guard lock(g_input_mutex);
    ReleaseCapture();
}

bool MouseCaptured() { return g_captured.load(); }

void CycleCameraMode() {
    int next = (g_mode.load() + 1) % 3;
    g_mode = next;
    ModeChanged();
}

void ToggleMouseCapture() {
    g_capture = !g_capture.load();
    if (!g_capture.load()) {
        std::lock_guard lock(g_input_mutex);
        ReleaseCapture();
    }
}

void Shutdown() {
    std::lock_guard lock(g_input_mutex);
    ReleaseCapture();
}

void ApplyStickDeadzones(input::Pad& pad) {
    math::ApplyStickDeadzone(pad.lx, pad.ly, g_move_deadzone.load());
    math::ApplyStickDeadzone(pad.rx, pad.ry, g_camera_deadzone.load());
}

void ResetDefaults() {
    g_mode = kOverShoulder;
    g_sensitivity = 0.15f;
    g_invert_x = false;
    g_invert_y = false;
    g_stick_invert_x = true;
    g_stick_invert_y = true;
    g_move_deadzone = kDefaultStickDeadzone;
    g_camera_deadzone = kDefaultStickDeadzone;
    g_shoulder = 120.0f;
    g_distance = 620.0f;
    g_first_person = 0.0f;
    g_capture = true;
    ModeChanged();
}

int GetCameraMode() { return g_mode.load(); }
void SetCameraMode(int mode) {
    if (mode < kClassic || mode > kFirstPerson) return;
    g_mode = mode;
    ModeChanged();
}
float GetMouseSensitivity() { return g_sensitivity.load(); }
void SetMouseSensitivity(float value) { g_sensitivity = Clamp(value, 0.01f, 1.0f); }
bool GetInvertX() { return g_invert_x.load(); }
void SetInvertX(bool value) { g_invert_x = value; }
bool GetInvertY() { return g_invert_y.load(); }
void SetInvertY(bool value) { g_invert_y = value; }
float GetShoulderOffset() { return g_shoulder.load(); }
void SetShoulderOffset(float value) { g_shoulder = Clamp(value, 0.0f, 350.0f); }
float GetCameraDistance() { return g_distance.load(); }
void SetCameraDistance(float value) { g_distance = Clamp(value, 120.0f, 1600.0f); }
float GetFirstPersonOffset() { return g_first_person.load(); }
void SetFirstPersonOffset(float value) { g_first_person = Clamp(value, 0.0f, 400.0f); }
bool GetMouseCapture() { return g_capture.load(); }
void SetMouseCapture(bool value) { g_capture = value; }

bool DrawSettings() {
    bool changed = false;
    int move_deadzone = g_move_deadzone.load();
    if (ImGui::SliderInt("Move dead zone", &move_deadzone, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        g_move_deadzone = move_deadzone;
        changed = true;
    }
    int camera_deadzone = g_camera_deadzone.load();
    if (ImGui::SliderInt("Camera dead zone", &camera_deadzone, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        g_camera_deadzone = camera_deadzone;
        changed = true;
    }
    int mode = g_mode.load();
    static const char* kModes[] = {"Classic (original camera)", "Over-the-shoulder (RE2 Remake)",
                                   "First person"};
    if (ImGui::BeginCombo("Camera", kModes[mode])) {
        for (int i = 0; i < 3; i++) {
            if (ImGui::Selectable(kModes[i], i == mode) && i != mode) {
                SetCameraMode(i);
                changed = true;
            }
            if (i == mode) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    float sensitivity = g_sensitivity.load();
    if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.01f, 1.0f, "%.3f deg/px")) {
        SetMouseSensitivity(sensitivity);
        changed = true;
    }
    if (mode != kClassic) {
        float shoulder = g_shoulder.load();
        if (ImGui::SliderFloat("Shoulder offset", &shoulder, 0.0f, 350.0f, "%.0f")) {
            SetShoulderOffset(shoulder);
            changed = true;
        }
        if (mode == kOverShoulder) {
            float distance = g_distance.load();
            if (ImGui::SliderFloat("Camera distance", &distance, 120.0f, 1600.0f, "%.0f")) {
                SetCameraDistance(distance);
                changed = true;
            }
        } else {
            float offset = g_first_person.load();
            if (ImGui::SliderFloat("First-person offset", &offset, 0.0f, 400.0f, "%.0f")) {
                SetFirstPersonOffset(offset);
                changed = true;
            }
        }
    }
    bool invert_x = g_invert_x.load();
    if (ImGui::Checkbox("Invert mouse X", &invert_x)) {
        SetInvertX(invert_x);
        changed = true;
    }
    ImGui::SameLine();
    bool invert_y = g_invert_y.load();
    if (ImGui::Checkbox("Invert mouse Y", &invert_y)) {
        SetInvertY(invert_y);
        changed = true;
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Controller camera (the title's default is inverted):");
    bool stick_x = g_stick_invert_x.load();
    if (ImGui::Checkbox("Invert camera stick X", &stick_x)) {
        g_stick_invert_x = stick_x;
        changed = true;
    }
    ImGui::SameLine();
    bool stick_y = g_stick_invert_y.load();
    if (ImGui::Checkbox("Invert camera stick Y", &stick_y)) {
        g_stick_invert_y = stick_y;
        changed = true;
    }
    bool capture = g_capture.load();
    if (ImGui::Checkbox("Capture mouse during gameplay", &capture)) {
        SetMouseCapture(capture);
        changed = true;
    }
    ImGui::TextDisabled("F5 cycles the camera; F6 releases or recaptures the mouse.");
    ImGui::TextWrapped("Mouse look rotates the chosen camera directly and stops when the mouse stops. Classic "
                       "leaves the title camera alone. Over-the-shoulder orbits around Hector using the title "
                       "camera's current distance; first person sits at his eye line. The character faces the "
                       "camera heading, so W always moves where you are looking.");
    return changed;
}

}  // namespace modern
