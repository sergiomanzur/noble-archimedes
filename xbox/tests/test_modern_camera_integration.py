"""Wiring checks for the primary build's modern camera overlay."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / "xbox" / "noble" / "overlay" / "src"


def read(relative):
    return (OVERLAY / relative).read_text(encoding="utf-8")


class ModernCameraIntegrationTest(unittest.TestCase):
    def test_camera_settings_are_wired_into_the_game_info_callbacks(self):
        game_info = read("common/game_info.cpp")
        self.assertIn(".load = modern::LoadSettings", game_info)
        self.assertIn(".save = modern::SaveSettings", game_info)
        self.assertIn('#include "common/modern_controls.h"', game_info)

    def test_pad_polling_runs_mouse_capture_and_modern_bindings(self):
        keyboard = read("common/keyboard.cpp")
        self.assertIn("modern::Frame(Focused(), MenuOpen(), scripted, d3d::Swaps());", keyboard)
        self.assertIn("if (!scripted) modern::ApplyPad(p);", keyboard)

    def test_camera_update_hook_runs_after_the_title_camera(self):
        warp = read("game/warp.cpp")
        original = "CV_ORIGINAL(CV_CAMERA_UPDATE)(ctx, base);"
        modern = "modern::AfterCameraUpdate(base);"
        self.assertIn(original, warp)
        self.assertIn(modern, warp)
        self.assertLess(warp.index(original), warp.index(modern))

    def test_camera_tab_is_registered(self):
        menu = read("common/menu.cpp")
        self.assertIn('ImGui::BeginTabItem("Camera"', menu)
        self.assertIn("modern::DrawSettings()", menu)
        self.assertIn("modern::SetMenuOpen(g_open);", menu)

    def test_settings_and_defaults_are_consistent(self):
        controls = read("common/modern_controls.cpp")
        for key in (
            "modern_camera_mode",
            "modern_mouse_sensitivity",
            "modern_invert_x",
            "modern_invert_y",
            "modern_shoulder_offset",
            "modern_camera_distance",
            "modern_first_person_offset",
            "modern_mouse_capture",
            "modern_invert_stick_x",
            "modern_invert_stick_y",
        ):
            self.assertIn(f'"{key}"', controls)
        self.assertIn("g_mode = kOverShoulder;", controls)
        self.assertIn("g_capture = true;", controls)
        self.assertIn("g_stick_invert_x = true;", controls)
        self.assertIn("g_stick_invert_y = true;", controls)

    def test_game_window_owns_cursor_and_draws_reticle_when_captured(self):
        menu = read("common/menu.cpp")
        self.assertIn("ImGuiConfigFlags_NoMouseCursorChange", menu)
        self.assertIn("msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT", menu)
        self.assertIn("SetCursor(nullptr);", menu)
        self.assertIn("modern::MouseCaptured() && GetForegroundWindow()", menu)

    def test_view_matrix_translation_uses_column_major_rows(self):
        math = read("common/modern_camera_math.h")
        self.assertIn("out[0] * position[0] + out[4] * position[1] + out[8] * position[2]", math)
        self.assertIn("out[1] * position[0] + out[5] * position[1] + out[9] * position[2]", math)
        self.assertIn("out[2] * position[0] + out[6] * position[1] + out[10] * position[2]", math)

    def test_mouse_look_is_direct_and_not_a_stick_budget(self):
        controls = read("common/modern_controls.cpp")
        self.assertIn("g_camera_yaw", controls)
        self.assertIn("g_camera_pitch", controls)
        self.assertIn("FaceHeading", controls)
        self.assertNotIn("g_stick_x", controls)
        self.assertNotIn("g_look_x", controls)
        math = read("common/modern_camera_math.h")
        self.assertIn("BasisFromAngles", math)
        self.assertIn("OrbitPosition", math)

    def test_scripted_runs_are_not_driven_by_physical_input(self):
        controls = read("common/modern_controls.cpp")
        self.assertIn("!scripted", controls)
        self.assertIn("ReleaseCapture();", controls)

    def test_mouse_capture_is_released_at_exit(self):
        controls = read("common/modern_controls.cpp")
        self.assertIn("std::atexit(Shutdown);", controls)
        self.assertNotIn("ShowCursor(", controls)


if __name__ == "__main__":
    unittest.main()
