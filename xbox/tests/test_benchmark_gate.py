"""Regression tests for the bounded benchmark-validity gate.

Run with:
    python -m unittest discover -s xbox/tests -p test_benchmark_gate.py

These avoid launching the game.  They cover the two audit failures that
invalidated the previous reports (mixed presented/renderer denominators and
SetTexture-scoped renderer deltas) plus the gate's environment and decision
rules.
"""
from __future__ import annotations

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "xbox" / "bench"))
import gate  # noqa: E402


def write_framestats(path: Path, last_flip: int, us: int = 18500,
                     captures=()) -> None:
    captures = set(captures)
    with open(path, "w", newline="", encoding="ascii") as f:
        w = csv.writer(f)
        w.writerow(["flip", "us", "capture"])
        for flip in range(1, last_flip + 1):
            w.writerow([flip, us, 1 if flip in captures else 0])


def stash_of_legacy_bug(directory: Path):
    """Write the shape of output/rcap_audit_final2 without its large files."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "probe.json").write_text(json.dumps({
        "frames": 153, "start_flip": 2678, "end_flip": 2831,
        "render_capture": True,
    }), encoding="ascii")
    with open(directory / "render_capture.csv", "w", newline="",
              encoding="ascii") as f:
        w = csv.writer(f)
        w.writerow(["type", "count", "cpu_ms", "bytes_copied", "bytes_required"])
        for kind in ("copy", "readback", "present", "swap", "clear"):
            w.writerow([kind, 132, 91.86, 0, 0])
    (directory / "render_capture.csv.manifest.csv").write_text(
        "data,norm_addr,resource_addr,binds,total,valid,gpu_target\n"
        "unique_total,,,2678784\n"
        "texture_binds,,,,11574\n"
        "renderer_events,,,,11574\n"
        "texture_frame_range,,,,2900..3032\n",
        encoding="ascii")


def make_valid_run(root: Path) -> Path:
    """Build a synthetic timing run that should pass every gate check."""
    arm = root / "arms" / "baseline"
    run = arm / "runs" / "run1"
    run.mkdir(parents=True)
    (arm / "identity.json").write_text(json.dumps({
        "exe_sha256": "A" * 64, "dll_sha256": "B" * 64,
    }), encoding="ascii")
    (run / "run.json").write_text(json.dumps({
        "arm": "baseline", "exe_sha256": "A" * 64, "dll_sha256": "B" * 64,
        "loaded_module": str(arm / "d3d8gl.dll"), "mode": "timing",
        "gate_start": gate.GATE_START, "gate_end": gate.GATE_END,
    }), encoding="ascii")
    (run / "env.json").write_text(json.dumps({
        "recomp": gate.expected_env(run, False), "cwd": str(ROOT),
    }), encoding="ascii")
    (run / "pad_script.txt").write_bytes(gate.FROZEN_PAD.read_bytes())
    (run / "run_status.json").write_text(json.dumps({
        "required_flip": gate.GATE_END, "max_flip": gate.GATE_END + 1,
        "complete": True, "timed_out": False, "wall_seconds": 60.0,
    }), encoding="ascii")
    write_framestats(run / "framestats.csv", gate.GATE_END + 1)
    lines = []
    for i, ms in enumerate(range(54500, 60000, 1000)):
        lines.append(
            "[PACE] 58 flips/s fps=60 movie=0 resets=0 period=17.20ms "
            "min=17.00 max=17.50 wait=0.00ms flag=00000000 "
            "frame_p50=18.50 p95=19.00 p99=19.20 max=19.30 frames=58")
        lines.append(
            "[STALL] calls=58 requested=580.00ms actual=580.01ms dsp10=58")
        lines.append(f"[BENCH] ms={ms} flips={3000 + i * 58}")
        lines.append(f"[TICK] guest_ms={ms} vblank={ms // 16} flips={i}")
    (run / "xbox_stderr.log").write_text("\n".join(lines) + "\n",
                                         encoding="ascii")
    return run


class FrozenInputTest(unittest.TestCase):
    def test_scene_flips_are_pinned_and_derivable(self):
        frozen = gate.require_frozen_inputs()
        self.assertEqual(frozen["calibration_sha256"], gate.FROZEN_CAL_SHA)
        self.assertEqual(frozen["frame_pad_sha256"], gate.FROZEN_PAD_SHA)
        self.assertEqual(tuple(tuple(x) for x in frozen["scene_flips"]),
                         gate.SCENE_FLIPS)

    def test_gate_window_is_inside_courtyard_light(self):
        scene = dict((name, (a, b)) for name, a, b in gate.SCENE_FLIPS)
        a, b = scene[gate.GATE_SCENE]
        self.assertLessEqual(a, gate.GATE_START)
        self.assertLessEqual(gate.GATE_END, b)
        for flip in gate.CAPTURE_FLIPS:
            self.assertGreaterEqual(flip, gate.GATE_START)
            self.assertLess(flip, gate.GATE_END)


class MetricsTest(unittest.TestCase):
    def test_percentile_matches_qualified_stats(self):
        values = [1.0, 2.0, 3.0, 4.0]
        self.assertEqual(gate.percentile(values, 50), 2.5)
        self.assertEqual(gate.percentile(values, 0), 1.0)
        self.assertEqual(gate.percentile(values, 100), 4.0)

    def test_window_requires_every_presented_flip(self):
        rows = [{"flip": f, "us": 18500, "capture": 0}
                for f in range(1, gate.GATE_END + 1)]
        metrics = gate.window_metrics(rows, gate.GATE_START, gate.GATE_END)
        self.assertEqual(metrics["frames"], gate.GATE_END - gate.GATE_START)
        self.assertEqual(metrics["problems"], [])
        self.assertAlmostEqual(metrics["p50_ms"], 18.5, places=3)

        holed = [r for r in rows if r["flip"] != gate.GATE_START + 10]
        metrics = gate.window_metrics(holed, gate.GATE_START, gate.GATE_END)
        self.assertTrue(metrics["problems"])
        self.assertIn("not contiguous", metrics["problems"][0])

    def test_framestats_rejects_non_increasing_flips(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "framestats.csv"
            path.write_text("flip,us,capture\n5,18000,0\n4,18000,0\n",
                            encoding="ascii")
            with self.assertRaises(gate.GateError):
                gate.parse_framestats(path)


class EnvTest(unittest.TestCase):
    def test_unique_and_gpu_stats_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            env = gate.expected_env(run, False)
            problems = gate.env_problems(env, run, False)
            self.assertEqual(problems, [])
            polluted = dict(env)
            polluted["RECOMP_BENCH_UNIQUE"] = "1"
            polluted["RECOMP_GPU_STATS"] = "1"
            problems = gate.env_problems(polluted, run, False)
            self.assertTrue(any("diagnostic env present" in p for p in problems))

    def test_missing_frame_recorder_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            env = gate.expected_env(run, False)
            del env["RECOMP_FRAME_STATS"]
            problems = gate.env_problems(env, run, False)
            self.assertTrue(any("required env missing" in p for p in problems))

    def test_capture_env_is_a_distinct_mode(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            env = gate.expected_env(run, True)
            self.assertIn("RECOMP_CAPTURE_FLIPS", env)
            problems = gate.env_problems(env, run, False)
            self.assertTrue(any("diagnostic env present" in p for p in problems))


class GuardEventTest(unittest.TestCase):
    def test_guard_events_are_counted_and_refusal_cap_is_explicit(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "xbox_stderr.log"
            path.write_text("\n".join([
                "[FB] refusing to present guest surface 0xFF1003E0 pitch 2560",
                "[FB] refusing to present guest surface 0xFF1003E0 pitch 2560",
                "[FB] refusing to present guest surface 0xFF1003E0 pitch 2560",
                "[FB] refusing to present guest surface 0xFF1003E0 pitch 2560",
                "[FB] not reading guest surface 0xFF1003E0 pitch 2560",
                "[PB] bad target #1 0x00000000 from word 0xDEADBEEF",
                "[ICALL] Failed to resolve VA 0x003AA4DE",
                "[CRASH] Access violation",
                "[GPU] REFUSING to write surface 0x00000000..0x00001000",
            ]) + "\n", encoding="ascii")
            result = gate.parse_guard_events(path)
        self.assertEqual(result["fb_refusing"], 4)
        self.assertTrue(result["fb_refusing_capped"])
        self.assertEqual(result["fb_not_reading"], 1)
        self.assertEqual(result["pb_bad_target"], 1)
        self.assertEqual(result["icall_failed"], 1)
        self.assertEqual(result["crash"], 1)
        self.assertEqual(result["gpu_surface_refused"], 1)


class CaptureAuditTest(unittest.TestCase):
    def test_mixed_denominators_and_settexture_deltas_are_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d) / "rcap"
            stash_of_legacy_bug(directory)
            result = gate.audit_render_capture(directory)
        self.assertEqual(result["renderer_frames"], 132)
        self.assertEqual(result["probe_presented_frames"], 153)
        self.assertEqual(result["recommended_denominator"], 132)
        joined = " | ".join(result["findings"])
        self.assertIn("denominator mismatch", joined)
        self.assertIn("SetTexture", joined)
        self.assertIn("no replay", joined)


class CaptureCompareTest(unittest.TestCase):
    def _write(self, directory: Path, color, flips=gate.CAPTURE_FLIPS,
               one_dark=False):
        from PIL import Image
        directory.mkdir(parents=True, exist_ok=True)
        img = Image.new("RGB", (4, 4), color)
        if one_dark:
            img.putpixel((0, 0), (0, 0, 0))
        for flip in flips:
            img.save(directory / f"present_{flip}.bmp")

    def test_candidate_inside_the_same_binary_null_is_equivalent(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._write(root / "b1", (255, 255, 255))
            self._write(root / "b2", (255, 255, 255), one_dark=True)
            self._write(root / "c", (255, 255, 255))
            result = gate.compare_captures(
                [root / "b1", root / "b2"], root / "c")
        self.assertEqual(result["verdict"], "equivalent_within_null")
        self.assertLessEqual(result["candidate_max_fraction"],
                             result["null_max_fraction"] + result["margin"])

    def test_large_capture_divergence_is_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._write(root / "b1", (255, 255, 255))
            self._write(root / "b2", (255, 255, 255), one_dark=True)
            self._write(root / "c", (0, 0, 0))
            result = gate.compare_captures(
                [root / "b1", root / "b2"], root / "c")
        self.assertEqual(result["verdict"], "divergence")
        self.assertGreater(result["candidate_max_fraction"],
                           result["null_max_fraction"])


class HostPreflightTest(unittest.TestCase):
    def _snapshot(self, **over):
        snap = {
            "cpu_percent": 3.0,
            "load_percentage": 4.0,
            "free_memory_mb": 8192.0,
            "power_plan": "Ultimate Performance",
            "gpu": {"name": "test", "utilization": 5.0, "temperature": 60.0},
            "process_names": ["chrome", "opencode-cli"],
        }
        snap.update(over)
        return snap

    def test_idle_host_passes(self):
        verdict = gate.preflight_reasons(self._snapshot())
        self.assertTrue(verdict["ok"])
        self.assertEqual(verdict["reasons"], [])

    def test_busy_host_and_forbidden_build_fail(self):
        verdict = gate.preflight_reasons(self._snapshot(
            cpu_percent=43.0, load_percentage=28.0, free_memory_mb=1024.0,
            gpu={"name": "test", "utilization": 45.0, "temperature": 85.0},
            process_names=["chrome", "msbuild"]))
        self.assertFalse(verdict["ok"])
        joined = " | ".join(verdict["reasons"])
        self.assertIn("CPU busy", joined)
        self.assertIn("free memory", joined)
        self.assertIn("GPU util", joined)
        self.assertIn("GPU temp", joined)
        self.assertIn("msbuild", joined)

    def test_power_saver_fails_and_balanced_warns(self):
        saver = gate.preflight_reasons(self._snapshot(power_plan="Power saver"))
        self.assertFalse(saver["ok"])
        balanced = gate.preflight_reasons(self._snapshot(power_plan="Balanced"))
        self.assertTrue(balanced["ok"])
        self.assertTrue(balanced["warnings"])

    def test_thresholds_are_not_weakened_by_default(self):
        verdict = gate.preflight_reasons(self._snapshot(cpu_percent=11.0))
        self.assertFalse(verdict["ok"])


class VerdictTest(unittest.TestCase):
    def test_overlapping_arms_are_inconclusive(self):
        result = gate.decide_verdict([18.30, 18.50, 18.40],
                                     [18.20, 18.35, 18.45])
        self.assertEqual(result["verdict"], "inconclusive")

    def test_separated_large_effect_is_a_win(self):
        result = gate.decide_verdict([18.60, 18.70, 18.65],
                                     [17.50, 17.60, 17.55])
        self.assertEqual(result["verdict"], "candidate_faster")
        self.assertTrue(result["non_overlap"])
        self.assertGreater(result["effect_ms"], 0.5)

    def test_separated_but_small_effect_is_not_a_win(self):
        result = gate.decide_verdict([18.60, 18.70, 18.65],
                                     [18.30, 18.20, 18.25],
                                     min_effect_ms=0.5)
        self.assertEqual(result["verdict"], "inconclusive")

    def test_host_drift_wide_arms_are_invalid(self):
        result = gate.decide_verdict([17.20, 18.60, 19.00],
                                     [17.50, 17.60, 17.55])
        self.assertEqual(result["verdict"], "invalid")
        self.assertTrue(result["problems"])


class StagingTest(unittest.TestCase):
    def test_staged_arm_has_one_dll_and_matching_hashes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            exe = root / "candidate.exe"
            dll = root / "d3d8gl.dll"
            exe.write_bytes(b"exe-bytes")
            dll.write_bytes(b"dll-bytes")
            with patch.object(gate, "ARMS", root / "arms"):
                dest = gate.stage_arm("candidate", exe, dll)
            self.assertEqual((dest / "game.exe").read_bytes(), b"exe-bytes")
            self.assertEqual((dest / "d3d8gl.dll").read_bytes(), b"dll-bytes")
            self.assertEqual([p.name.lower() for p in dest.glob("*.dll")],
                             ["d3d8gl.dll"])
            identity = json.loads((dest / "identity.json").read_text())
            self.assertEqual(identity["exe_sha256"],
                             gate.sha256_file(exe))
            self.assertEqual(identity["dll_sha256"],
                             gate.sha256_file(dll))


@unittest.skipUnless(gate.FROZEN_RUN.exists(),
                     "qualified checkpoint runs are not present")
class FrozenBaselineTest(unittest.TestCase):
    def test_gate_window_reproduces_the_frozen_p50_range(self):
        values = []
        for run in (1, 2, 3):
            result = gate.validate_run(gate.FROZEN_RUN / f"run{run}",
                                       mode="timing", legacy=True)
            metrics = result["metrics"]
            self.assertEqual(metrics["frames"], 180)
            self.assertFalse(metrics["problems"])
            values.append(metrics["p50_ms"])
        self.assertTrue(all(17.5 <= v <= 20.5 for v in values), values)
        self.assertLessEqual(max(values) - min(values), 1.5)


class ValidRunTest(unittest.TestCase):
    def test_clean_timing_run_has_no_errors(self):
        with tempfile.TemporaryDirectory() as d:
            run = make_valid_run(Path(d))
            result = gate.validate_run(run, mode="timing")
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["metrics"]["frames"], 180)
        self.assertGreaterEqual(result["metrics"]["dsp_buckets"], 4)

    def test_capture_flips_must_match_the_gate(self):
        with tempfile.TemporaryDirectory() as d:
            run = make_valid_run(Path(d))
            write_framestats(run / "framestats.csv", gate.GATE_END + 1,
                             captures=(gate.CAPTURE_FLIPS[0],))
            result = gate.validate_run(run, mode="capture")
        self.assertTrue(any("capture flips" in e for e in result["errors"]))

    def test_timing_run_with_bmp_captures_is_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            run = make_valid_run(Path(d))
            (run / "present_3069.bmp").write_bytes(b"bmp")
            result = gate.validate_run(run, mode="timing")
        self.assertTrue(any("capture images" in w for w in result["warnings"]))


if __name__ == "__main__":
    unittest.main()
