// Android: the imported camera modes without the Windows mouse capture. The
// right stick (a controller's or the touch overlay's) drives the same
// Classic, over-the-shoulder and first-person cameras as the PC build.
#include "common/modern_controls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/modern_camera_math.h"
#include "common/release.h"
#include "core/input.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"

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
std::vector<EnemyInfo> Enemies();  // game/enemies.cpp

extern bool g_invert_camera_x;  // menu.cpp: the Camera section's Invert X
extern bool g_invert_camera_y;  // menu.cpp: the Camera section's Invert Y

namespace modern {
namespace {

constexpr uint32_t kSceneMask = 0x28000;
constexpr float kMinDistance = 120.0f;
constexpr float kMaxPitch = 1.10f;
constexpr float kStickRadiansPerFrame = 0.025f;

std::mutex g_input_mutex;
std::atomic<float> g_camera_yaw{0.0f};
std::atomic<float> g_camera_pitch{0.0f};
std::atomic<float> g_base_heading{0.0f};
std::atomic<float> g_move_amount{0.0f};
std::atomic<bool> g_camera_need_init{true};

std::atomic<int> g_mode{kOverShoulder};
std::atomic<float> g_sensitivity{0.15f};
constexpr int kDefaultStickDeadzone = 10;
std::atomic<int> g_move_deadzone{kDefaultStickDeadzone};
std::atomic<int> g_camera_deadzone{kDefaultStickDeadzone};
std::atomic<float> g_shoulder{120.0f};
std::atomic<float> g_distance{620.0f};
std::atomic<float> g_first_person{0.0f};
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
// position. Tying it to the absolute camera heading makes movement follow the
// view instead of continuing along the last character heading.
void FaceHeading(uint8_t* base, float yaw) {
    uint32_t player = 0, record = 0;
    if (!ReadU32(base, cv::kPlayer, player) || !player) return;
    if (!ReadU32(base, player, record) || record >= 128) return;
    uint32_t yaw_address = cv::kRecords + record * 0x1210 + 0x2C;
    if (!mem::IsCommitted(yaw_address) || !mem::IsCommitted(yaw_address + 3)) return;
    if (!std::isfinite(yaw)) return;
    std::memcpy(base + yaw_address, &yaw, sizeof yaw);
}

void ModeChanged() { g_camera_need_init = true; }

void WriteView(uint8_t* base, uint32_t view, const float* target, const float* position, const float* matrix) {
    if (!view || !mem::IsCommitted(view + 0xFF)) return;
    float* v = reinterpret_cast<float*>(base + view);
    std::memcpy(v + 0, target, sizeof(float) * 4);
    std::memcpy(v + 12, target, sizeof(float) * 4);
    std::memcpy(v + 16, position, sizeof(float) * 4);
    std::memcpy(v + 32, target, sizeof(float) * 4);
    std::memcpy(v + 36, matrix, sizeof(float) * 16);
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
    g_move_deadzone = std::clamp(t["move_stick_deadzone"].value_or(kDefaultStickDeadzone), 0, 50);
    g_camera_deadzone = std::clamp(t["camera_stick_deadzone"].value_or(kDefaultStickDeadzone), 0, 50);
    g_shoulder = Clamp(t["modern_shoulder_offset"].value_or(g_shoulder.load()), 0.0f, 350.0f);
    g_distance = Clamp(t["modern_camera_distance"].value_or(g_distance.load()), kMinDistance, 1600.0f);
    g_first_person = Clamp(t["modern_first_person_offset"].value_or(g_first_person.load()), 0.0f, 400.0f);
}

void SaveSettings(void* table) {
    if (!table) return;
    toml::table& t = *static_cast<toml::table*>(table);
    t.insert_or_assign("modern_camera_mode", int64_t(g_mode.load()));
    t.insert_or_assign("move_stick_deadzone", int64_t(g_move_deadzone.load()));
    t.insert_or_assign("camera_stick_deadzone", int64_t(g_camera_deadzone.load()));
    t.insert_or_assign("modern_shoulder_offset", double(g_shoulder.load()));
    t.insert_or_assign("modern_camera_distance", double(g_distance.load()));
    t.insert_or_assign("modern_first_person_offset", double(g_first_person.load()));
}

void Frame(bool focused, bool menu_open, bool scripted, uint32_t) {
    g_gameplay = focused && !menu_open && !scripted && GameplayCamera(reinterpret_cast<uint8_t*>(mem::Base()));
}

void ApplyPad(input::Pad& pad) {
    std::lock_guard lock(g_input_mutex);
    uint32_t frame = d3d::Swaps();
    static uint32_t stick_frame = ~0u;
    const int mode = g_mode.load();
    if (mode != kClassic && !g_gameplay.load()) return;
    g_move_amount = std::max(std::fabs(static_cast<float>(pad.lx)), std::fabs(static_cast<float>(pad.ly))) / 32767.0f;
    const bool invert_x = g_invert_camera_x, invert_y = g_invert_camera_y;
    if (mode == kClassic) {
        // The title's raw stick is inverted. Unchecked gives the usual
        // direction; the Camera section's Invert X/Y flips it back.
        pad.rx = math::ClassicAxis(pad.rx, 0.0f, !invert_x, false);
        pad.ry = math::ClassicAxis(pad.ry, 0.0f, !invert_y, false);
    } else {
        if (frame != stick_frame && (pad.rx != 0 || pad.ry != 0)) {
            float rx = (pad.rx / 32767.0f) * (invert_x ? -1.0f : 1.0f);
            float ry = (pad.ry / 32767.0f) * (invert_y ? -1.0f : 1.0f);
            float yaw = g_camera_yaw.load() + rx * kStickRadiansPerFrame;
            while (yaw > math::kPi) yaw -= 2.0f * math::kPi;
            while (yaw < -math::kPi) yaw += 2.0f * math::kPi;
            float pitch = g_camera_pitch.load() - ry * kStickRadiansPerFrame;
            g_camera_yaw = yaw;
            g_camera_pitch = Clamp(pitch, -kMaxPitch, kMaxPitch);
        }
        stick_frame = frame;

        // Steer the title camera toward the host heading. Movement is
        // relative to this camera, so it follows the view while keeping the
        // title's own movement rules.
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
}

void AfterCameraUpdate(uint8_t* base) {
    if (!base) return;
    const int mode = g_mode.load();
    if (mode == kClassic) return;

    if ((SceneWord() & kSceneMask) || CameraSpecial(base)) g_camera_need_init = true;
    float player_position[3], player_yaw = 0;
    if (!GameplayCamera(base, player_position, &player_yaw)) return;

    uint32_t view = 0;
    if (!ReadU32(base, cv::kCameraView, view) || !view) return;
    if (!mem::IsCommitted(view) || !mem::IsCommitted(view + 0xFF)) return;
    float* v = reinterpret_cast<float*>(base + view);

    float target[4], position[4], matrix[16];
    std::memcpy(target, v + 12, sizeof(target));
    std::memcpy(position, v + 16, sizeof(position));
    std::memcpy(matrix, v + 36, sizeof(matrix));
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
    if (g_move_amount.load() < 0.05f) FaceHeading(base, yaw);

    float next_position[4];
    if (mode == kFirstPerson) {
        float distance = math::kHeadClearance + g_first_person.load();
        for (const auto& enemy : Enemies())
            distance = math::EnemyEyeLimit(player_position, enemy.pos, yaw, distance);
        math::FirstPersonEye(player_position, yaw, distance, next_position);
    } else {
        float wanted = std::min(g_distance.load(), base_distance);
        float shoulder = g_shoulder.load();
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

void SetMenuOpen(bool) {}
bool MouseCaptured() { return false; }
void ToggleMouseCapture() {}
void Shutdown() {}

void ApplyStickDeadzones(input::Pad& pad) {
    math::ApplyStickDeadzone(pad.lx, pad.ly, g_move_deadzone.load());
    math::ApplyStickDeadzone(pad.rx, pad.ry, g_camera_deadzone.load());
}

void ResetDefaults() {
    g_mode = kOverShoulder;
    g_sensitivity = 0.15f;
    g_move_deadzone = kDefaultStickDeadzone;
    g_camera_deadzone = kDefaultStickDeadzone;
    g_shoulder = 120.0f;
    g_distance = 620.0f;
    g_first_person = 0.0f;
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
bool GetInvertX() { return g_invert_camera_x; }
void SetInvertX(bool value) { g_invert_camera_x = value; }
bool GetInvertY() { return g_invert_camera_y; }
void SetInvertY(bool value) { g_invert_camera_y = value; }
float GetShoulderOffset() { return g_shoulder.load(); }
void SetShoulderOffset(float value) { g_shoulder = Clamp(value, 0.0f, 350.0f); }
float GetCameraDistance() { return g_distance.load(); }
void SetCameraDistance(float value) { g_distance = Clamp(value, kMinDistance, 1600.0f); }
float GetFirstPersonOffset() { return g_first_person.load(); }
void SetFirstPersonOffset(float value) { g_first_person = Clamp(value, 0.0f, 400.0f); }
bool GetMouseCapture() { return false; }
void SetMouseCapture(bool) {}

bool DrawSettings() {
    bool changed = false;
    ImGui::SeparatorText("Camera system");
    int mode = g_mode.load();
    static const char* kModes[] = {"Classic (original camera)", "Over-the-shoulder (RE2 Remake)", "First person"};
    if (ImGui::BeginCombo("Mode", kModes[mode])) {
        for (int i = 0; i < 3; i++) {
            if (ImGui::Selectable(kModes[i], i == mode) && i != mode) {
                SetCameraMode(i);
                changed = true;
            }
            if (i == mode) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
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
    ImGui::TextWrapped("The right stick moves the selected camera. The Camera section below flips its X and Y "
                       "directions. Classic keeps the title's original follow camera; over-the-shoulder is the RE2 "
                       "Remake-style view; first person sits at Hector's eye line.");
    return changed;
}

void CycleCameraMode() {
    SetCameraMode((g_mode.load() + 1) % 3);
}

}  // namespace modern
