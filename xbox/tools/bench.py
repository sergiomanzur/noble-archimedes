#!/usr/bin/env python3
"""Deterministic Xbox host benchmark harness.

Runs the recompiled title through a scripted controller timeline, samples the
host's own PACE/GPU counters, checks that presented frames are unique, and
captures a reference frame per scene.  The default path is the software
rasteriser; pass --d3d11 or --d3d12 only to compare an opt-in backend.

Examples:
    python xbox/tools/bench.py --runs 1
    python xbox/tools/bench.py --runs 3 --update-baseline
    python xbox/tools/bench.py --profile movie --runs 1

Scenes are timed from the first frame-pacing report (BENCH ms), which is
within a second of the title's first controller report.  The reference
screenshots and logs are deliberately written under xbox/bench/out, which is
gitignored.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "xbox" / "bench"
OUT = BENCH / "out"
BASE = BENCH / "baselines"
EXE_DEFAULT = ROOT / "xbox" / "build" / "Release" / "curse_of_darkness_xbox_recomp.exe"

# Timings are milliseconds from the first PACE report.  The title reaches the
# menu around 15 s, the in-engine prologue around 25 s, and the courtyard
# around 48-50 s with the standard three Start presses (output/HANDOFF.md).
MAIN_SCENES = [
    ("title",          15000, 18000),
    ("main_menu",      19000, 23500),
    ("cutscene",       25000, 31500),
    ("courtyard_light", 54000, 60000),
    ("pause",          62000, 70000),
    ("courtyard_heavy", 74000, 86000),
    ("combat",         88000, 106000),
]

MAIN_PAD = """\
16000:start
30000:start
40000:start
60000:start:500
72000:start:500
76000:lup:2500
79000:a:150
80000:a:150
81000:a:150
84000:lleft:1600
87000:a:150
88000:a:150
89000:a:150
"""
MAIN_DURATION = 112

MOVIE_SCENES = [
    ("boot",       500,   9000),
    ("attract_movie", 94000, 111000),
]
MOVIE_DURATION = 118

PACE_RE = re.compile(
    r"\[PACE\]\s+(?P<flips>\d+) flips/s fps=(?P<fps>\d+) movie=(?P<movie>\d+) "
    r"resets=(?P<resets>\d+) period=(?P<period>[-0-9.]+)ms "
    r"min=(?P<min>[-0-9.]+) max=(?P<max>[-0-9.]+) wait=(?P<wait>[-0-9.]+)ms"
)
PERF_RE = re.compile(
    r"\[GPU-PERF\].*?draw=(?P<draw>[-0-9.]+) p8=(?P<p8>[-0-9.]+) "
    r"xform=(?P<xform>[-0-9.]+) raster=(?P<raster>[-0-9.]+) "
    r"share=(?P<share>[-0-9.]+) wait=(?P<gwait>[-0-9.]+) setup=(?P<setup>[-0-9.]+) "
    r"ms/frame"
)
BENCH_RE = re.compile(r"\[BENCH\] ms=(?P<ms>\d+)(?: flips=(?P<flip>\d+))?")
TICK_RE = re.compile(r"\[TICK\] guest_ms=(?P<guest>\d+) vblank=(?P<vblank>\d+) flips=(?P<flips>\d+)")
UNIQ_RE = re.compile(r"\[UNIQUE\] presents=(?P<presents>\d+) unique=(?P<unique>\d+)")


@dataclass
class Sample:
    ms: float
    pace: dict
    perf: dict | None
    tick: dict | None
    unique: dict | None
    flip: int = 0


@dataclass
class SceneResult:
    name: str
    seconds: float
    fps: list[float] = field(default_factory=list)
    unique_fps: list[float] = field(default_factory=list)
    splits: dict[str, list[float]] = field(default_factory=lambda: {
        "draw": [], "p8": [], "xform": [], "raster": [],
        "share": [], "wait": [], "setup": [],
    })
    guest_ms: list[float] = field(default_factory=list)
    vblank: list[float] = field(default_factory=list)
    movie: int = 0
    present_unique_ratio: float = 0.0
    guest_rate: float = 0.0
    screenshot: Path | None = None

    def median(self, key: str) -> float:
        v = self.splits[key]
        return statistics.median(v) if v else 0.0

    def low1(self) -> float:
        if not self.fps:
            return 0.0
        # The report is one bucket per second; the slowest bucket is the
        # closest reproducible 1%-low at this duration.
        return min(self.fps)

    def median_fps(self) -> float:
        return statistics.median(self.fps) if self.fps else 0.0

    def unique_median(self) -> float:
        return statistics.median(self.unique_fps) if self.unique_fps else 0.0


def parse_log(path: Path) -> list[Sample]:
    samples: list[Sample] = []
    last_ms = 0.0
    last_flip = 0
    last_perf = None
    last_tick = None
    last_unique = None
    for line in path.read_text(errors="replace").splitlines():
        m = BENCH_RE.search(line)
        if m:
            last_ms = float(m.group("ms"))
            if m.group("flip") is not None:
                last_flip = int(m.group("flip"))
            continue
        m = PACE_RE.search(line)
        if m:
            d = {k: (float(v) if k in ("period", "min", "max", "wait") else int(v))
                 for k, v in m.groupdict().items()}
            samples.append(Sample(last_ms, d, last_perf, last_tick, last_unique,
                                  last_flip))
            continue
        m = PERF_RE.search(line)
        if m:
            last_perf = {k: float(v) for k, v in m.groupdict().items()}
            continue
        m = TICK_RE.search(line)
        if m:
            last_tick = {k: float(v) for k, v in m.groupdict().items()}
            continue
        m = UNIQ_RE.search(line)
        if m:
            last_unique = {k: float(v) for k, v in m.groupdict().items()}
    return samples


def scene_samples(samples: list[Sample], a: float, b: float) -> list[Sample]:
    out = []
    for s in samples:
        if a <= s.ms < b:
            out.append(s)
    return out


def summarize(name: str, a: float, b: float, samples: list[Sample]) -> SceneResult:
    r = SceneResult(name=name, seconds=(b - a) / 1000.0)
    rows = scene_samples(samples, a, b)
    # Use the sample nearest each scene edge for cumulative UNIQUE counters.
    def nearest(ms: float):
        if not samples:
            return None
        return min(samples, key=lambda s: abs(s.ms - ms))
    first = nearest(a)
    last = nearest(b - 1)
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
    if first and last and first.unique and last.unique:
        dt = max(0.001, (last.ms - first.ms) / 1000.0)
        dp = max(0, last.unique["presents"] - first.unique["presents"])
        du = max(0, last.unique["unique"] - first.unique["unique"])
        r.present_unique_ratio = (du / dp) if dp else 0.0
        # A per-second unique rate is useful next to the PACE flips.
        r.unique_fps.append(du / dt)
    if len(r.guest_ms) >= 2 and rows:
        # Divide by the span between the samples that actually carried the
        # tick values, not by a nominal scene length.  Using the latter made a
        # healthy 1000 ms/s guest clock look like 667 ms/s.
        tick_span = max(0.001, (rows[-1].ms - rows[0].ms) / 1000.0)
        r.guest_rate = (r.guest_ms[-1] - r.guest_ms[0]) / tick_span
    return r


def host_info() -> str:
    cpu = os.environ.get("PROCESSOR_IDENTIFIER") or "unknown CPU"
    logical = os.cpu_count() or 0
    physical = ""
    gpu = "unknown GPU"
    try:
        ps = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name);"
             "(Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty NumberOfCores);"
             "(Get-CimInstance Win32_VideoController | Select-Object -First 1 -ExpandProperty Name)"],
            capture_output=True, text=True, timeout=15)
        lines = [x.strip() for x in ps.stdout.splitlines() if x.strip()]
        if lines:
            cpu = lines[0]
        if len(lines) > 1:
            physical = f" ({lines[1]} physical cores)"
        if len(lines) > 2:
            gpu = lines[2]
    except Exception:
        pass
    return f"{cpu}{physical}; {logical} logical CPUs; GPU: {gpu}"


def find_window(pid: int):
    user = ctypes.windll.user32
    callback = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    found = []

    @callback
    def visit(hwnd, unused):
        p = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            cls = ctypes.create_unicode_buffer(80)
            user.GetClassNameW(hwnd, cls, 80)
            if cls.value == "XboxRecompFramebuffer":
                found.append(hwnd)
        return True

    user.EnumWindows(visit, 0)
    return found[0] if found else None


def run_one(exe: Path, out: Path, pad_text: str, duration: int,
            backend: str, update_baseline: bool, no_unique: bool = False,
            no_gpu_stats: bool = False, no_p8_cache: bool = False,
            capture_flips: str = "", pad_frames: bool = False) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    (out / "pad_script.txt").write_text(pad_text, encoding="ascii")
    for f in ("xbox_stdout.log", "xbox_stderr.log", "xbox_kernel.log"):
        src = ROOT / f
        if src.exists():
            src.unlink()
    # Start clean: diagnostics from the caller's shell must not leak into a
    # benchmark.  The switches below are the harness contract.
    env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
    env.update(
        RECOMP_PAD_SCRIPT="@" + str(out / "pad_script.txt"),
        RECOMP_FB_WINDOW="1",
        RECOMP_FB_DUMP=str(out / "frame.bmp"),
        RECOMP_FB_WINDOW_DUMP_EVERY="30",
        RECOMP_PACE_STATS="1",
        RECOMP_BENCH_STATS="1",
    )
    if not no_unique:
        env["RECOMP_BENCH_UNIQUE"] = "1"
    if not no_gpu_stats:
        env["RECOMP_GPU_STATS"] = "1"
    if no_p8_cache:
        env["RECOMP_NO_P8_CACHE"] = "1"
    if capture_flips:
        env["RECOMP_CAPTURE_FLIPS"] = capture_flips
        env["RECOMP_CAPTURE_DIR"] = str(out)
    if pad_frames:
        env["RECOMP_PAD_FRAMES"] = "1"
    if backend == "d3d11":
        env["RECOMP_GPU_RASTER"] = "1"
    elif backend == "d3d12":
        env["RECOMP_GPU_D3D12"] = "1"
    st = subprocess.STARTUPINFO()
    st.dwFlags = subprocess.STARTF_USESHOWWINDOW
    st.wShowWindow = 0
    p = subprocess.Popen([str(exe)], cwd=ROOT, env=env, startupinfo=st)
    shots: list[Path] = []
    start = time.monotonic()
    # Capture at each scene midpoint; the window dump is refreshed every 30
    # presented frames, so a copy here is the frame the scene was measured on.
    scenes = MAIN_SCENES if pad_text else MOVIE_SCENES
    captures = []
    for name, a, b in scenes:
        captures.append((name, start + (a + b) / 2000.0))
    captures.sort(key=lambda x: x[1])
    ci = 0
    try:
        while time.monotonic() - start < duration and p.poll() is None:
            now = time.monotonic()
            while ci < len(captures) and now >= captures[ci][1]:
                frame = out / "frame.bmp"
                if frame.exists():
                    dst = out / f"capture_{captures[ci][0]}.bmp"
                    shutil.copy2(frame, dst)
                    shots.append(dst)
                ci += 1
            time.sleep(0.1)
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        for f in ("xbox_stdout.log", "xbox_stderr.log", "xbox_kernel.log"):
            src = ROOT / f
            if src.exists():
                shutil.copy2(src, out / f)
    return shots


def compare_baseline(shots: list[Path], update: bool) -> list[str]:
    try:
        from PIL import Image, ImageChops
    except Exception:
        return ["Pillow not installed; screenshot comparison skipped"]
    BASE.mkdir(parents=True, exist_ok=True)
    notes = []
    for shot in shots:
        name = shot.stem.removeprefix("capture_")
        base = BASE / f"{name}.bmp"
        if update or not base.exists():
            shutil.copy2(shot, base)
            notes.append(f"{name}: baseline written")
            continue
        try:
            a = Image.open(base).convert("RGB")
            b = Image.open(shot).convert("RGB")
            if a.size != b.size:
                notes.append(f"{name}: size mismatch {a.size} vs {b.size}")
                continue
            diff = ImageChops.difference(a, b)
            # Any channel moving more than 8 counts is a visible difference.
            changed = sum(1 for px in diff.getdata() if max(px) > 8)
            ratio = changed / (a.size[0] * a.size[1])
            dpath = shot.with_name(shot.stem + "_diff.png")
            diff.save(dpath)
            notes.append(f"{name}: {ratio*100:.2f}% pixels differ")
        except Exception as e:
            notes.append(f"{name}: compare failed: {e}")
    return notes


def fmt_table(results: list[SceneResult]) -> str:
    lines = []
    lines.append("| scene | fps median | 1% low | unique fps | unique/present | draw | p8 | xform | raster | share | wait | setup | guest ms/s |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in results:
        g = r.guest_rate
        lines.append(
            f"| {r.name} | {r.median_fps():.1f} | {r.low1():.1f} | "
            f"{r.unique_median():.1f} | {r.present_unique_ratio*100:.1f}% | {r.median('draw'):.2f} | "
            f"{r.median('p8'):.2f} | {r.median('xform'):.2f} | "
            f"{r.median('raster'):.2f} | {r.median('share'):.2f} | "
            f"{r.median('wait'):.2f} | {r.median('setup'):.2f} | {g:.0f} |"
        )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--duration", type=int, default=0,
                    help="override run length in seconds")
    ap.add_argument("--profile", choices=("main", "movie", "both"), default="main")
    ap.add_argument("--backend", choices=("software", "d3d11", "d3d12"),
                    default="software")
    ap.add_argument("--exe", type=Path, default=EXE_DEFAULT)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--no-unique", action="store_true",
                    help="disable framebuffer hashing to measure harness overhead")
    ap.add_argument("--no-gpu-stats", action="store_true",
                    help="disable per-second GPU/P8 reports")
    ap.add_argument("--no-p8-cache", action="store_true",
                    help="disable the P8 RGBA cache for comparison")
    ap.add_argument("--capture-flips", default="",
                    help="comma-separated presented flip numbers to capture")
    a = ap.parse_args()
    if not a.exe.exists():
        print(f"missing executable: {a.exe}", file=sys.stderr)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    BASE.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    print(f"host: {host_info()}")
    print(f"backend: {a.backend}, runs: {a.runs}, executable: {a.exe}")
    all_results: list[SceneResult] = []
    profiles = ["main", "movie"] if a.profile == "both" else [a.profile]
    for profile in profiles:
        scenes = MAIN_SCENES if profile == "main" else MOVIE_SCENES
        pad = MAIN_PAD if profile == "main" else ""
        duration = a.duration or (MAIN_DURATION if profile == "main" else MOVIE_DURATION)
        pdir = OUT / f"{stamp}_{profile}_{a.backend}"
        for run in range(1, a.runs + 1):
            rdir = pdir / f"run{run}"
            print(f"\n[{profile} run {run}/{a.runs}] {rdir}")
            shots = run_one(a.exe, rdir, pad, duration, a.backend, a.update_baseline,
                            a.no_unique, a.no_gpu_stats, a.no_p8_cache,
                            a.capture_flips)
            notes = compare_baseline(shots, a.update_baseline)
            log = rdir / "xbox_stderr.log"
            samples = parse_log(log) if log.exists() else []
            print(f"  samples={len(samples)}")
            for note in notes:
                print(f"  shot: {note}")
            for name, s0, s1 in scenes:
                r = summarize(name, s0, s1, samples)
                r.screenshot = next((x for x in shots if x.stem == f"capture_{name}"), None)
                all_results.append(r)
                print(f"  {name}: {r.median_fps():.1f} fps, "
                      f"{r.present_unique_ratio*100:.1f}% unique, "
                      f"{len(r.fps)} pace samples")
    # Aggregate across repeated runs by scene (median of the run medians).
    by_name: dict[str, list[SceneResult]] = {}
    for r in all_results:
        by_name.setdefault(r.name, []).append(r)
    agg = []
    for name, group in by_name.items():
        first = group[0]
        agg.append(SceneResult(
            name=name, seconds=first.seconds, fps=[g.median_fps() for g in group],
            unique_fps=[g.unique_median() for g in group],
            splits={k: [g.median(k) for g in group] for k in first.splits},
            guest_ms=[g.guest_ms[-1] if g.guest_ms else 0 for g in group],
            vblank=[g.vblank[-1] if g.vblank else 0 for g in group],
            present_unique_ratio=statistics.median([g.present_unique_ratio for g in group]),
            guest_rate=statistics.median([g.guest_rate for g in group]),
        ))
    print("\n" + fmt_table(agg))
    print(f"\nartifacts: {OUT / stamp}_{profiles[0]}_{a.backend}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
