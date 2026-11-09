// Regression test for the imported title's column-major view matrix and the
// direct-look camera placements built on top of it.
#include <cmath>
#include <cstdio>

#include "common/modern_camera_math.h"

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }

void Apply(const float matrix[16], const float point[3], float out[3]) {
    out[0] = matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12];
    out[1] = matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13];
    out[2] = matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14];
}

void TestViewMatrix() {
    const float position[3] = {5, -2, 7};
    const float right[3] = {0, 0, 1};
    const float up[3] = {0, 1, 0};
    const float forward[3] = {1, 0, 0};
    float matrix[16];
    modern::math::ViewMatrix(right, up, forward, position, matrix);

    float at_camera[3], at_target[3];
    const float target[3] = {10, -2, 7};
    Apply(matrix, position, at_camera);
    Apply(matrix, target, at_target);
    Check(Near(at_camera[0], 0) && Near(at_camera[1], 0) && Near(at_camera[2], 0),
          "camera position maps to the view origin");
    Check(Near(at_target[0], 0) && Near(at_target[1], 0) && Near(at_target[2], 5),
          "the target lies five units down the view's forward axis");
}

void TestBasisFromAngles() {
    float right[3], up[3], forward[3];
    modern::math::BasisFromAngles(0, 0, right, up, forward);
    Check(Near(forward[2], 1) && Near(right[0], 1) && Near(up[1], 1),
          "zero angles give the title's +z forward basis");

    modern::math::BasisFromAngles(modern::math::kPi / 2.0f, 0, right, up, forward);
    Check(Near(forward[0], 1) && Near(right[2], -1) && Near(up[1], 1),
          "positive yaw turns toward world +x");

    modern::math::BasisFromAngles(0, modern::math::kPi / 4.0f, right, up, forward);
    Check(Near(forward[1], 0.70710678f) && Near(forward[2], 0.70710678f),
          "positive pitch looks down in the title's y-down world");
}

void TestOrbitPosition() {
    const float target[3] = {0, 0, 0};
    const float forward[3] = {0, 0, 1};
    const float right[3] = {1, 0, 0};
    float out[3];
    modern::math::OrbitPosition(target, forward, right, 10, 2, out);
    Check(Near(out[0], 2) && Near(out[1], 0) && Near(out[2], -10),
          "orbit keeps the requested distance and shoulder");
    modern::math::OrbitPosition(target, forward, right, 4, 2, out);
    Check(Near(out[0], 1) && Near(out[2], -4),
          "shoulder is capped to a quarter of the orbit distance");
    modern::math::OrbitPosition(target, forward, right, 1, 10, out);
    Check(Near(out[0], 0.25f) && Near(out[2], -1),
          "shoulder shrinks in tight camera distances");
}

void TestFirstPerson() {
    const float standing[3] = {200, 0, -1000};
    const float running[3] = {200, -25, -500};
    float eye[3], running_eye[3];
    modern::math::FirstPersonPosition(standing, 0, 0, eye);
    modern::math::FirstPersonPosition(running, 0, 0, running_eye);
    Check(eye[1] < standing[1] && eye[2] > standing[2], "default eye is above and ahead of Hector");
    for (int axis = 0; axis < 3; ++axis)
        Check(Near(running_eye[axis] - eye[axis], running[axis] - standing[axis]),
              "eye follows the current player pose without lag");
    modern::math::FirstPersonPosition(running, modern::math::kPi / 2, 50, running_eye);
    Check(Near(running_eye[1], running[1] + eye[1] - standing[1]) &&
              Near(running_eye[0] - running[0], eye[2] - standing[2] + 50),
          "turning rotates forward clearance and the extra offset without moving the eye line");
}

void TestFirstPersonMovement() {
    using modern::math::kPi;
    using modern::math::RebaseMovement;
    int16_t x = 0, y = 32767;
    RebaseMovement(x, y, kPi, 0);
    Check(x == 0 && y == -32767, "forward reverses immediately after a half turn");

    x = 0;
    y = 16000;
    RebaseMovement(x, y, kPi / 2, 0);
    Check(x == 16000 && y == 0, "a quarter turn preserves analog strength");

    x = 10000;
    y = 0;
    RebaseMovement(x, y, 0, kPi / 2);
    Check(x == 0 && y == 10000, "strafe compensates for a nonzero follow heading");

    x = y = 32767;
    RebaseMovement(x, y, kPi / 4, 0);
    Check(x == 32767 && y == 0, "full diagonal rotates without clipping its direction");
}

void TestEnemyClearanceAndStickLimits() {
    const float player[3] = {0, 0, 0};
    const float close[3] = {0, 0, 100};
    const float behind[3] = {0, 0, -100};
    const float beside[3] = {200, 0, 100};
    const float safe = modern::math::EnemyEyeLimit(player, close, 0, 120);
    float eye[3];
    modern::math::FirstPersonEye(player, 0, safe, eye);
    Check(eye[2] < 0 && close[2] - eye[2] > 64 && eye[1] < -330,
          "eye retracts and lifts when an enemy enters the forward camera space");
    Check(Near(modern::math::EnemyEyeLimit(player, behind, 0, 120), 120) &&
              Near(modern::math::EnemyEyeLimit(player, beside, 0, 120), 120),
          "enemies behind or to the side do not pull back the eye");
    Check(modern::math::InvertStick(-32768) == 32767 &&
              modern::math::InvertStick(32767) == -32767 &&
              modern::math::InvertStick(0) == 0,
          "inversion reverses both full-stick limits and preserves neutral");
    using modern::math::ClassicAxis;
    Check(ClassicAxis(0, 0.5f, false, false) < 0 && ClassicAxis(0, 0.5f, true, false) < 0 &&
              ClassicAxis(0, 0.5f, false, true) > 0 && ClassicAxis(0, 0.5f, true, true) > 0,
          "only the mouse inversion changes mouse direction");
    Check(ClassicAxis(-32768, 0, true, true) == 32767 &&
              ClassicAxis(-32768, 0, true, false) == 32767,
          "mouse inversion does not change full-stick input");
}

void TestStickDeadzone() {
    using modern::math::ApplyStickDeadzone;
    int16_t x = 2000, y = -2000;
    ApplyStickDeadzone(x, y, 10);
    Check(x == 0 && y == 0, "small diagonal drift is centered");
    x = 3000; y = 3000;
    ApplyStickDeadzone(x, y, 10);
    Check(x > 0 && x == y && x < 3000, "diagonal input keeps its direction and ramps from zero");
    x = 32767; y = 0;
    ApplyStickDeadzone(x, y, 50);
    Check(x == 32767 && y == 0, "full cardinal input still reaches full speed");
    x = -32768; y = 32767;
    ApplyStickDeadzone(x, y, 50);
    Check(x == -32768 && y == 32767, "full diagonal input keeps the native endpoints");
    x = -20; y = 100;
    ApplyStickDeadzone(x, y, 0);
    Check(x == -20 && y == 100, "zero dead zone preserves the original input");
}

}  // namespace

int main() {
    TestViewMatrix();
    TestBasisFromAngles();
    TestOrbitPosition();
    TestFirstPerson();
    TestFirstPersonMovement();
    TestEnemyClearanceAndStickLimits();
    TestStickDeadzone();
    if (failures) {
        std::fprintf(stderr, "modern camera math: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("modern camera math: ok\n");
    return 0;
}
