import importlib.util
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
BENCH_PATH = ROOT / "xbox" / "tools" / "bench.py"
spec = importlib.util.spec_from_file_location("xbox_bench", BENCH_PATH)
bench = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = bench
spec.loader.exec_module(bench)
with patch.dict(sys.modules, {"bench": bench}):
    frame_spec = importlib.util.spec_from_file_location(
        "xbox_frame_bench", ROOT / "xbox" / "tools" / "frame_bench.py")
    frame_bench = importlib.util.module_from_spec(frame_spec)
    frame_spec.loader.exec_module(frame_bench)


def calibration_log():
    return "\n".join(
        f"[BENCH] ms={ms} flips={flip}\n"
        "[PACE] 60 flips/s fps=60 movie=0 resets=0 period=16.67ms "
        "min=16.60 max=17.10 wait=0.10ms"
        for ms, flip in ((1000, 60), (110000, 6600)))


class BenchHarnessTest(unittest.TestCase):
    def test_parses_pace_perf_tick_and_unique(self):
        text = "\n".join([
            "[BENCH] ms=1000",
            "[GPU-PERF] invisible/s=1 flip=60 flips/s=60 draws/s=100 pixels/s=1 px/frame=1 "
            "cpu=1 draw=1.00 p8=2.00 xform=3.00 raster=4.00 share=5.00 wait=0.50 setup=0.00 ms/frame",
            "[TICK] guest_ms=1000 vblank=60 flips=60",
            "[UNIQUE] presents=60 unique=59",
            "[PACE] 60 flips/s fps=60 movie=0 resets=0 period=16.67ms min=16.60 max=17.10 wait=0.10ms flag=00000000",
        ])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "xbox_stderr.log"
            p.write_text(text)
            samples = bench.parse_log(p)
        self.assertEqual(len(samples), 1)
        s = samples[0]
        self.assertEqual(s.ms, 1000.0)
        self.assertEqual(s.pace["flips"], 60)
        self.assertEqual(s.perf["draw"], 1.0)
        self.assertEqual(s.tick["vblank"], 60.0)
        self.assertEqual(s.unique["unique"], 59.0)

    def test_summarize_scene(self):
        samples = [
            bench.Sample(500.0, {"flips": 30, "movie": 0}, None, None, None),
            bench.Sample(1200.0, {"flips": 60, "movie": 0}, None, None, None),
            bench.Sample(2200.0, {"flips": 58, "movie": 0}, None, None, None),
        ]
        r = bench.summarize("x", 1000, 3000, samples)
        self.assertEqual(r.median_fps(), 59.0)
        self.assertEqual(r.low1(), 58.0)

    def test_scene_definitions_are_ordered(self):
        for name, a, b in bench.MAIN_SCENES:
            self.assertTrue(name)
            self.assertLess(a, b)
        self.assertGreaterEqual(bench.MAIN_DURATION, bench.MAIN_SCENES[-1][2] / 1000)

    def test_backend_opt_in_clears_inherited_renderer_switches(self):
        for backend, expected in (
                ("software", {}), ("d3d8gl", {"RECOMP_D3D8GL": "1"}),
                ("d3d11", {"RECOMP_GPU_RASTER": "1"}),
                ("d3d12", {"RECOMP_GPU_D3D12": "1"})):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as d:
                process = Mock()
                process.poll.return_value = 0
                with patch.object(bench, "ROOT", Path(d)), patch.dict(os.environ, {
                        "RECOMP_D3D8GL": "1", "RECOMP_GPU_RASTER": "1",
                        "RECOMP_GPU_D3D12": "1", "RECOMP_GPU_PRESENT": "1"}), \
                        patch.object(bench.subprocess, "Popen", return_value=process) as launch:
                    bench.run_one(Path("unused.exe"), Path(d) / "out", "", 0,
                                  backend, False)
                env = launch.call_args.kwargs["env"]
                actual = {k: env[k] for k in (
                    "RECOMP_D3D8GL", "RECOMP_GPU_RASTER", "RECOMP_GPU_D3D12",
                    "RECOMP_GPU_PRESENT") if k in env}
                self.assertEqual(actual, expected)

    def test_gpu_present_requires_d3d11_before_launch(self):
        with tempfile.TemporaryDirectory() as d, \
                patch.object(bench, "ROOT", Path(d)), \
                patch.object(bench.subprocess, "Popen") as launch:
            with self.assertRaisesRegex(ValueError, "d3d11"):
                bench.run_one(Path("unused.exe"), Path(d), "", 0,
                              "d3d8gl", False, gpu_present=True)
            launch.assert_not_called()

    def test_missing_baseline_is_not_created_without_update(self):
        with tempfile.TemporaryDirectory() as d:
            from PIL import Image
            shot = Path(d) / "capture_title.bmp"
            Image.new("RGB", (2, 2)).save(shot)
            base = Path(d) / "baselines"
            with patch.object(bench, "BASE", base):
                notes = bench.compare_baseline([shot], False)
            self.assertFalse((base / "title.bmp").exists())
            self.assertIn("missing", notes[0])

    def test_bench_cli_selects_d3d8gl_and_forwards_gpu_present(self):
        for backend, present in (("d3d8gl", False), ("d3d11", True)):
            with self.subTest(backend=backend), tempfile.TemporaryDirectory() as d:
                exe = Path(d) / "game.exe"
                exe.touch()
                argv = ["bench", "--exe", str(exe), "--backend", backend]
                if present:
                    argv.append("--gpu-present")
                with patch.object(sys, "argv", argv), \
                        patch.object(bench, "OUT", Path(d) / "out"), \
                        patch.object(bench, "BASE", Path(d) / "base"), \
                        patch.object(bench, "host_info", return_value="test"), \
                        patch.object(bench, "run_one", return_value=[]) as run, \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(bench.main(), 0)
                self.assertEqual(run.call_args.args[4], backend)
                self.assertEqual(run.call_args.kwargs.get("gpu_present", False), present)

    def test_explicit_calibration_reuses_frames_for_shorter_backend_runs(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            exe = root / "game.exe"
            exe.touch()
            calibration = root / "xbox_stderr.log"
            calibration.write_text(calibration_log())
            generated = []

            def fake_launch(exe, out, pad, duration, backend, update, **kwargs):
                generated.append((pad, duration, backend, kwargs))
                out.mkdir(parents=True, exist_ok=True)
                (out / "xbox_stderr.log").write_text(calibration_log())
                for flip in kwargs.get("capture_flips", "").split(","):
                    if flip:
                        (out / f"present_{flip}.bmp").write_bytes(b"exact-frame")
                return []

            for backend in ("software", "d3d8gl"):
                argv = ["frame_bench", "--runs", "1", "--duration", "20",
                        "--backend", backend, "--calibration", str(calibration)]
                out = root / backend
                with patch.object(sys, "argv", argv), \
                        patch.object(frame_bench, "OUT", out), \
                        patch.object(bench, "EXE_DEFAULT", exe), \
                        patch.object(bench, "host_info", return_value="test"), \
                        patch.object(bench, "run_one", side_effect=fake_launch), \
                        patch.object(bench, "compare_baseline", return_value=[]) as compare, \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(frame_bench.main(), 0)
                shots, update = compare.call_args.args
                self.assertEqual(len(shots), 7)
                self.assertFalse(update)
                self.assertEqual(shots[0].read_bytes(), b"exact-frame")
                self.assertTrue(list(out.rglob("frame_pad.txt")))
            self.assertEqual(len(generated), 2)  # No additional calibration launch.
            self.assertEqual(generated[0][0], generated[1][0])
            self.assertEqual(generated[0][1:3], (20, "software"))
            self.assertEqual(generated[1][1:3], (20, "d3d8gl"))
            self.assertEqual(generated[1][3]["capture_flips"],
                             "990,1275,1695,3420,3960,4800,5820")
            self.assertTrue(generated[1][3]["pad_frames"])

    def test_frame_cli_rejects_unusable_calibration_without_launch(self):
        invalid = ("", calibration_log().split("[BENCH] ms=110000")[0],
                   calibration_log().replace("ms=110000", "ms=1000"),
                   calibration_log().replace("ms=110000", "ms=2000"),
                   calibration_log().replace("flips=6600", "flips=30"))
        for text in invalid:
            with self.subTest(text=text), tempfile.TemporaryDirectory() as d:
                exe = Path(d) / "game.exe"
                exe.touch()
                cal = Path(d) / "xbox_stderr.log"
                cal.write_text(text)
                with patch.object(sys, "argv", ["frame_bench", "--calibration", str(cal)]), \
                        patch.object(bench, "EXE_DEFAULT", exe), \
                        patch.object(frame_bench, "OUT", Path(d) / "out"), \
                        patch.object(bench, "run_one") as launch, \
                        patch.object(bench, "host_info", return_value="test"), \
                        contextlib.redirect_stderr(io.StringIO()), \
                        contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(SystemExit) as raised:
                        frame_bench.main()
                    self.assertEqual(raised.exception.code, 2)
                launch.assert_not_called()

    def test_frame_cli_calibrates_software_for_full_timeline(self):
        with tempfile.TemporaryDirectory() as d:
            exe = Path(d) / "game.exe"
            exe.touch()
            launches = []

            def fake_launch(exe, out, pad, duration, backend, update, **kwargs):
                launches.append((pad, duration, backend, kwargs))
                out.mkdir(parents=True, exist_ok=True)
                (out / "xbox_stderr.log").write_text(calibration_log())
                return []

            with patch.object(sys, "argv", ["frame_bench", "--runs", "1",
                    "--duration", "20", "--backend", "d3d11", "--gpu-present"]), \
                    patch.object(bench, "EXE_DEFAULT", exe), \
                    patch.object(frame_bench, "OUT", Path(d) / "out"), \
                    patch.object(bench, "run_one", side_effect=fake_launch), \
                    patch.object(bench, "host_info", return_value="test"), \
                    patch.object(bench, "compare_baseline", return_value=[]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(frame_bench.main(), 0)
            self.assertEqual(len(launches), 2)
            self.assertEqual(launches[0][:3], (bench.MAIN_PAD, 112, "software"))
            self.assertEqual(launches[0][3], {"no_unique": True, "no_gpu_stats": True})
            self.assertEqual(launches[1][1:3], (20, "d3d11"))
            self.assertTrue(launches[1][3]["gpu_present"])

    def test_frame_cli_rejects_missing_calibration_or_invalid_present(self):
        for args in (["--calibration", "missing/xbox_stderr.log"],
                     ["--skip-calibration"], ["--backend", "d3d8gl", "--gpu-present"]):
            with self.subTest(args=args), \
                    patch.object(sys, "argv", ["frame_bench", *args]), \
                    patch.object(bench, "run_one") as launch, \
                    contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    frame_bench.main()
                self.assertEqual(raised.exception.code, 2)
                launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
