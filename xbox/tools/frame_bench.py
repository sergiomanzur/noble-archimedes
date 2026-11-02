#!/usr/bin/env python3
"""Frame-exact benchmark driver.

The first run is a calibration: the normal wall-clock pad timeline records the
presented flip number at each [BENCH] tick.  The calibration is converted to a
pad script measured in presented flips, and the real runs use that script plus
RECOMP_CAPTURE_FLIPS at the scene midpoints.  Screenshots and scene windows are
therefore tied to frames, not wall-clock time.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import statistics
import time
from pathlib import Path

import bench

ROOT = bench.ROOT
OUT = bench.OUT / "frame_exact"
PAD_LINE = re.compile(r"^\s*(\d+):([^:]+)(?::(\d+))?\s*$")


def flip_at(ms: float, points: list[tuple[float, int]]) -> int:
    if not points:
        return int(round(ms / 16.667))
    points = sorted(points)
    if ms <= points[0][0]:
        return points[0][1]
    if ms >= points[-1][0]:
        return points[-1][1]
    for (m0, f0), (m1, f1) in zip(points, points[1:]):
        if m0 <= ms <= m1:
            if m1 == m0:
                return f1
            return int(round(f0 + (f1 - f0) * (ms - m0) / (m1 - m0)))
    return points[-1][1]


def calibration_points(samples) -> list[tuple[float, int]]:
    points = [(s.ms, s.flip) for s in samples if s.flip]
    if len(points) < 2 or any(
            m1 <= m0 or f1 <= f0
            for (m0, f0), (m1, f1) in zip(points, points[1:])):
        raise ValueError("calibration needs at least two increasing frame timestamps")
    required_start = min(s0 for _, s0, _ in bench.MAIN_SCENES)
    required_end = max(s1 for _, _, s1 in bench.MAIN_SCENES)
    if points[0][0] > required_start or points[-1][0] < required_end:
        raise ValueError(f"calibration must cover scene timestamps "
                         f"{required_start}-{required_end} ms")
    return points


def make_frame_script(pad: str, points) -> tuple[str, dict[str, int]]:
    out = []
    mids: dict[str, int] = {}
    for line in pad.splitlines():
        line = line.strip()
        if not line:
            continue
        m = PAD_LINE.match(line)
        if not m:
            continue
        at, buttons, hold = m.groups()
        hold_ms = int(hold) if hold else 150
        at_frame = flip_at(int(at), points)
        hold_frames = max(1, int(round(hold_ms / 16.667)))
        out.append(f"{at_frame}:{buttons}:{hold_frames}")
    return "\n".join(out) + "\n", mids


def summarize_flip(name: str, fa: int, fb: int, samples) -> bench.SceneResult:
    rows = [s for s in samples if fa <= s.flip < fb]
    if not rows:
        return bench.SceneResult(name=name, seconds=0.0)
    span = max(0.001, (rows[-1].ms - rows[0].ms) / 1000.0)
    r = bench.SceneResult(name=name, seconds=span)
    first = min(samples, key=lambda s: abs(s.flip - fa))
    last = min(samples, key=lambda s: abs(s.flip - fb))
    for s in rows:
        r.fps.append(float(s.pace["flips"]))
        r.movie += int(s.pace.get("movie", 0))
        if s.perf:
            for k in r.splits:
                src = "gwait" if k == "wait" else k
                if src in s.perf:
                    r.splits[k].append(float(s.perf[src]))
        if s.tick:
            r.guest_ms.append(float(s.tick["guest"]))
            r.vblank.append(float(s.tick["vblank"]))
    if first.unique and last.unique:
        dp = max(0, last.unique["presents"] - first.unique["presents"])
        du = max(0, last.unique["unique"] - first.unique["unique"])
        r.present_unique_ratio = du / dp if dp else 0.0
        r.unique_fps.append(du / max(0.001, (last.ms - first.ms) / 1000.0))
    if r.guest_ms:
        r.guest_rate = (r.guest_ms[-1] - r.guest_ms[0]) / max(0.001, span)
    return r


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--duration", type=int, default=0)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--backend", choices=bench.BACKENDS, default="software")
    ap.add_argument("--calibration", type=Path,
                    help="reuse an existing calibration xbox_stderr.log across backends")
    ap.add_argument("--calibrate-backend", action="store_true",
                    help="calibrate with the selected backend instead of software")
    ap.add_argument("--skip-calibration", action="store_true",
                    help="reuse --calibration (requires an explicit existing log)")
    ap.add_argument("--gpu-present", action="store_true",
                    help="enable RECOMP_GPU_PRESENT for the D3D11 backend")
    ap.add_argument("--frame-stats", action="store_true",
                    help="collect per-flip durations with the in-memory recorder")
    a = ap.parse_args()
    if a.gpu_present and a.backend != "d3d11":
        ap.error("--gpu-present requires --backend d3d11")
    if a.skip_calibration and a.calibration is None:
        ap.error("--skip-calibration requires --calibration PATH")
    points = None
    if a.calibration is not None:
        try:
            points = calibration_points(bench.parse_log(a.calibration))
        except (OSError, ValueError) as exc:
            ap.error(f"invalid calibration {a.calibration}: {exc}")
    if not bench.EXE_DEFAULT.exists():
        print("missing executable")
        return 2
    stamp = time.strftime("%Y%m%d_%H%M%S")
    pdir = OUT / f"{stamp}_{a.backend}"
    pdir.mkdir(parents=True, exist_ok=True)
    print(f"host: {bench.host_info()}")
    print(f"frame-exact benchmark: {pdir}")
    duration = a.duration or bench.MAIN_DURATION

    cal_log = pdir / "calibration" / "xbox_stderr.log"
    if a.calibration is not None:
        cal_log.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(a.calibration, cal_log)
    else:
        print("[calibration] wall-clock pad timeline")
        cal_backend = a.backend if a.calibrate_backend else "software"
        bench.run_one(bench.EXE_DEFAULT, pdir / "calibration", bench.MAIN_PAD,
                      bench.MAIN_DURATION, cal_backend, False, no_unique=True,
                      no_gpu_stats=True)
        try:
            points = calibration_points(bench.parse_log(cal_log))
        except (OSError, ValueError) as exc:
            ap.error(f"invalid calibration {cal_log}: {exc}")
    print(f"[calibration] {len(points)} frame-timestamped samples")
    frame_pad, _ = make_frame_script(bench.MAIN_PAD, points)
    (pdir / "frame_pad.txt").write_text(frame_pad, encoding="ascii")
    scene_flips = []
    for name, s0, s1 in bench.MAIN_SCENES:
        fa, fb = flip_at(s0, points), flip_at(s1, points)
        mid = (fa + fb) // 2
        scene_flips.append((name, fa, fb, mid))
    capture = ",".join(str(mid) for *_, mid in scene_flips)
    required_flip = max(fb for _, _, fb, _ in scene_flips)
    wall_timeout = max(240, duration * 2)
    print(f"[frames] {[(n, a, b) for n, a, b, _ in scene_flips]}")
    print(f"[completion] required_flip={required_flip} wall_timeout={wall_timeout}s")

    results = []
    for run in range(1, a.runs + 1):
        rdir = pdir / f"run{run}"
        print(f"[run {run}/{a.runs}] {rdir}")
        bench.run_one(bench.EXE_DEFAULT, rdir, frame_pad, duration, a.backend,
                      False, no_unique=False, no_gpu_stats=False,
                      capture_flips=capture, pad_frames=True,
                      gpu_present=a.gpu_present, required_flip=required_flip,
                      wall_timeout=wall_timeout,
                      frame_stats=a.frame_stats)
        status_path = rdir / "run_status.json"
        status = json.loads(status_path.read_text()) if status_path.exists() else {}
        if not status.get("complete", True):
            print(f"  incomplete: max_flip={status.get('max_flip')} "
                  f"required={required_flip} timed_out={status.get('timed_out')}")
        # Rename actual-present captures to the names compare_baseline expects.
        shots = []
        for name, fa, fb, mid in scene_flips:
            src = rdir / f"present_{mid}.bmp"
            if src.exists():
                dst = rdir / f"capture_{name}.bmp"
                src.replace(dst)
                shots.append(dst)
        notes = bench.compare_baseline(shots, a.update_baseline)
        for note in notes:
            print("  shot:", note)
        samples = bench.parse_log(rdir / "xbox_stderr.log")
        for name, fa, fb, mid in scene_flips:
            r = summarize_flip(name, fa, fb, samples)
            results.append(r)
            print(f"  {name}: {r.median_fps():.1f} fps, "
                  f"{r.present_unique_ratio*100:.1f}% unique, "
                  f"flips {fa}-{fb}")

    by = {}
    for r in results:
        by.setdefault(r.name, []).append(r)
    agg = []
    for name, group in by.items():
        first = group[0]
        agg.append(bench.SceneResult(
            name=name, seconds=first.seconds,
            fps=[g.median_fps() for g in group],
            unique_fps=[g.unique_median() for g in group],
            splits={k: [g.median(k) for g in group] for k in first.splits},
            present_unique_ratio=statistics.median([g.present_unique_ratio for g in group]),
            guest_rate=statistics.median([g.guest_rate for g in group]),
        ))
    print("\n" + bench.fmt_table(agg))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
