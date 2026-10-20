import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH_PATH = ROOT / "xbox" / "tools" / "bench.py"
spec = importlib.util.spec_from_file_location("xbox_bench", BENCH_PATH)
bench = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = bench
spec.loader.exec_module(bench)


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


if __name__ == "__main__":
    unittest.main()
