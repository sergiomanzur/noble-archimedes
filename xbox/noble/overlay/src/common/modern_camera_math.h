// Pure camera math shared by the host camera adapter and its regression test.
// The imported title stores its view matrix in column-major form: the world
// position p maps to view space as row dot products with the strided triplets
// below, and the translation column is -R * cameraPosition.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace modern {
namespace math {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

inline float Length(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

inline void Normalize(float v[3]) {
    float length = Length(v);
    if (!(length > 1e-6f) || !std::isfinite(length)) return;
    v[0] /= length, v[1] /= length, v[2] /= length;
}

inline void Cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Camera basis from an absolute view heading. Positive yaw looks toward world
// +x and positive pitch looks down, matching the title's y-down world and
// player-yaw convention.
inline void BasisFromAngles(float yaw, float pitch, float right[3], float up[3], float forward[3]) {
    float cp = std::cos(pitch), sp = std::sin(pitch);
    float sy = std::sin(yaw), cy = std::cos(yaw);
    forward[0] = sy * cp;
    forward[1] = sp;
    forward[2] = cy * cp;
    right[0] = cy;
    right[1] = 0.0f;
    right[2] = -sy;
    Cross(forward, right, up);
}

inline void OrbitPosition(const float target[3], const float forward[3], const float right[3],
                          float distance, float shoulder, float out[3]) {
    float safe_shoulder = distance * 0.25f;
    if (shoulder < 0.0f) shoulder = 0.0f;
    if (shoulder > safe_shoulder) shoulder = safe_shoulder;
    out[0] = target[0] - forward[0] * distance + right[0] * shoulder;
    out[1] = target[1] - forward[1] * distance + right[1] * shoulder;
    out[2] = target[2] - forward[2] * distance + right[2] * shoulder;
}

inline constexpr float kHeadClearance = 120.0f;

inline float EnemyEyeLimit(const float player[3], const float enemy[3], float yaw, float distance) {
    const float dx = enemy[0] - player[0], dz = enemy[2] - player[2];
    const float s = std::sin(yaw), c = std::cos(yaw);
    const float ahead = dx * s + dz * c;
    const float side = dx * c - dz * s;
    if (!std::isfinite(enemy[1]) || std::fabs(enemy[1] - player[1]) > 320.0f ||
        ahead < 0.0f || std::fabs(side) > 145.0f)
        return distance;
    // Leave space for the enemy model and the guest camera's near plane.
    return std::min(distance, std::max(-105.0f, ahead - 245.0f));
}

inline void FirstPersonEye(const float player[3], float yaw, float distance, float out[3]) {
    constexpr float eye_height = 330.0f;
    out[0] = player[0] + std::sin(yaw) * distance;
    // As the eye backs into Hector's silhouette, lift it above his head.
    out[1] = player[1] - eye_height - std::max(0.0f, 50.0f - distance) * 0.75f;
    out[2] = player[2] + std::cos(yaw) * distance;
}

inline void FirstPersonPosition(const float player[3], float yaw, float forward_offset, float out[3]) {
    FirstPersonEye(player, yaw, kHeadClearance + forward_offset, out);
}

inline void ApplyStickDeadzone(int16_t& x, int16_t& y, int percent) {
    constexpr float full = 32767.0f;
    const float deadzone = full * (std::clamp(percent, 0, 50) / 100.0f);
    if (deadzone == 0.0f) return;
    const float magnitude = std::hypot(float(x), float(y));
    if (magnitude <= deadzone) {
        x = y = 0;
    } else if (magnitude < full) {
        // Ramp the remaining travel from zero without changing direction.
        const float gain = (1.0f - deadzone / magnitude) / (1.0f - deadzone / full);
        x = static_cast<int16_t>(std::lround(x * gain));
        y = static_cast<int16_t>(std::lround(y * gain));
    }
}

inline int16_t InvertStick(int16_t value) {
    return static_cast<int16_t>(-std::max<int>(value, -32767));
}

inline int16_t ClassicAxis(int16_t stick, float mouse, bool invert_stick, bool invert_mouse) {
    const int16_t native_stick = invert_stick ? InvertStick(stick) : stick;
    if (mouse == 0.0f) return native_stick;
    // The title's orbit takes a negative input for the normal positive motion.
    const float native_mouse = mouse * (invert_mouse ? 32767.0f : -32767.0f);
    return static_cast<int16_t>(std::clamp(native_stick + native_mouse, -32767.0f, 32767.0f));
}

// Express movement in the title's follow-camera coordinates. Rotate both axes
// together and scale only if the result cannot fit in the guest stick range.
inline void RebaseMovement(int16_t& x, int16_t& y, float view_yaw, float follow_yaw) {
    const float turn = view_yaw - follow_yaw;
    const float c = std::cos(turn), s = std::sin(turn);
    const float right = x * c + y * s;
    const float forward = y * c - x * s;
    const float peak = std::max({32767.0f, std::fabs(right), std::fabs(forward)});
    const float scale = 32767.0f / peak;
    x = static_cast<int16_t>(std::lround(right * scale));
    y = static_cast<int16_t>(std::lround(forward * scale));
}

// Column-major memory layout used by the title: strided rows of the rotation
// followed by the translation column.
inline void ViewMatrix(const float right[3], const float up[3], const float forward[3],
                       const float position[3], float out[16]) {
    out[0] = right[0]; out[1] = up[0]; out[2] = forward[0]; out[3] = 0.0f;
    out[4] = right[1]; out[5] = up[1]; out[6] = forward[1]; out[7] = 0.0f;
    out[8] = right[2]; out[9] = up[2]; out[10] = forward[2]; out[11] = 0.0f;
    out[12] = -(out[0] * position[0] + out[4] * position[1] + out[8] * position[2]);
    out[13] = -(out[1] * position[0] + out[5] * position[1] + out[9] * position[2]);
    out[14] = -(out[2] * position[0] + out[6] * position[1] + out[10] * position[2]);
    out[15] = 1.0f;
}

}  // namespace math
}  // namespace modern
