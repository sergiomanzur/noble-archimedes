#!/usr/bin/env python3
"""Bounded benchmark-validity gate for the Xbox host (read-only by default).

This tool does not change the renderer, guest runtime, frame pacing, the HLE
DLL, the capture code, or the shared build.  It exists because the current
benchmark artifacts cannot prove three things by themselves:

  1. which executable and which d3d8gl.dll were actually loaded;
  2. that the measured window is the same presented-frame window in both arms;
  3. that the diagnostic environment was identical and off.

`plan` prints the ordered host-run commands.  `run` and `identity` refuse to
launch a game unless --confirm-host-run is passed; the orchestrator owns host
time.  `validate`, `compare`, `audit-capture`, and `snapshot` only read files.

Gate protocol (pinned to the frozen 2026-10-21 inputs):

  * calibration hash  E0FAE694...  (software calibration, unchanged in the
    qualified run)
  * frame pad hash    30CE2B61...  (frame-exact controller script)
  * gameplay window   courtyard_light, presented flips [2980, 3160), 180 frames
  * captures          3 consecutive presented flips 3069, 3070, 3071
  * diagnostics       only PACE/BENCH/TICK/STALL prints and the in-memory
                      frame-duration recorder; no UNIQUE, GPU stats, timeline,
                      HLE stats, readback/texture traces, or periodic BMPs
  * timing arms       AB BA AB (three pairs, alternating order)

The frozen baseline exe (E9C7D678...) and DLL (CF6DAF2C...) are NOT on disk in
this worktree.  `snapshot` reports that as a blocker; it does not substitute the
current build.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "xbox" / "bench"
_gate_override = os.environ.get("RECOMP_GATE_DIR")
GATE = Path(_gate_override) if _gate_override else BENCH / "out" / "gate"
if not GATE.is_absolute():
    GATE = ROOT / GATE
ARMS = GATE / "arms"
FROZEN = ROOT / "output" / "qualified_checkpoint"
FROZEN_RUN = (ROOT / "xbox" / "bench" / "out" / "frame_exact"
              / "20261021_194702_d3d8gl")
FROZEN_CAL = FROZEN_RUN / "calibration" / "xbox_stderr.log"
FROZEN_PAD = FROZEN / "frame_pad.txt"

FROZEN_EXE_SHA = "E9C7D67812CA05CBC6857F4E2C1F0B34EBF2CCF3699462081845E366EF996E55"
FROZEN_DLL_SHA = "CF6DAF2CEF7A42488DB7B9B9445F456E125D9DC3F36E364A4DF166090A724282"
FROZEN_CAL_SHA = "E0FAE6940D656CE8294A4E2F13CC5947513C7F7645321A41E2E40C7C11A47CA2"
FROZEN_PAD_SHA = "30CE2B61F3BBBD2AA19AE5063524297569104B1D11675AF3547083C5CC78AF59"

# Scene boundaries in presented-flip space, derived once from FROZEN_CAL and
# recorded here so a different calibration cannot silently move a window.
SCENE_FLIPS = (
    ("title", 880, 1060),
    ("main_menu", 1108, 1323),
    ("cutscene", 1413, 1777),
    ("courtyard_light", 2912, 3218),
    ("pause", 3325, 3760),
    ("courtyard_heavy", 3971, 4562),
    ("combat", 4666, 5491),
)

GATE_SCENE = "courtyard_light"
GATE_START = 2980          # inside courtyard_light, after the scene transition
GATE_END = 3160            # half-open; frame_stats dumps at this flip
CAPTURE_FLIPS = (3069, 3070, 3071)
DEFAULT_WALL_TIMEOUT = 180

# Environment keys the gate intentionally sets.  Everything else with the
# RECOMP_ prefix is a diagnostic and must be absent.
GATE_ENV_KEYS = frozenset({
    "RECOMP_D3D8GL",
    "RECOMP_FB_WINDOW",
    "RECOMP_PAD_FRAMES",
    "RECOMP_PAD_SCRIPT",
    "RECOMP_PACE_STATS",
    "RECOMP_BENCH_STATS",
    "RECOMP_FRAME_STATS",
    "RECOMP_FRAME_STATS_FILE",
    "RECOMP_FRAME_STATS_START",
    "RECOMP_FRAME_STATS_END",
})
CAPTURE_ENV_KEYS = GATE_ENV_KEYS | {"RECOMP_CAPTURE_FLIPS", "RECOMP_CAPTURE_DIR"}

# Diagnostics whose output would make a timing arm non-comparable.
FORBIDDEN_STDERR = (
    "[GAME_STACK]", "[NV2A]", "[IO]", "[READBACK-PERF]", "[HLE-PERF]",
    "[TEX-PERF]", "[TIMELINE]", "[FRAME]", "[FRAMEHIST]", "[GPU-PERF]",
    "[RCAP]",
)
# Guard / executor events counted per arm.  [FB] refusals are capped at four
# printed lines by fb_present.c; the count is a floor when it reaches four.
GUARD_PATTERNS = {
    "fb_refusing": "[FB] refusing to present guest surface",
    "fb_not_reading": "[FB] not reading guest surface",
    "gpu_surface_refused": "[GPU] REFUSING",
    "crash": "[CRASH]",
    "pb_bad_target": "[PB] bad target",
    "icall_failed": "[ICALL] Failed to resolve",
    "pb_table_full": "[PB] method table full",
    "gpu_behind": "(GPU behind)",
}
GUARD_MATERIAL = (
    "fb_refusing", "fb_not_reading", "gpu_surface_refused", "crash",
    "pb_bad_target", "icall_failed",
)
# Unconditional startup lines that are not diagnostics of interest:
#   [D3D8HLE] init: ...            (3 lines)
#   [D3D8HLE] entry=...            (first <=32 distinct HLE VAs)
#   [DEBUG 0039C000] ...           (12 lines, generated-code startup)
# They are I/O outside the measured window; the gate records them as warnings.
BENCH_RE = re.compile(r"\[BENCH\]\s+ms=(\d+)\s+flips=(\d+)")
PACE_RE = re.compile(
    r"\[PACE\]\s+(\d+) flips/s fps=(\d+) movie=(\d+) resets=(\d+) "
    r"period=([-0-9.]+)ms min=([-0-9.]+) max=([-0-9.]+) wait=([-0-9.]+)ms "
    r"flag=([0-9A-Fa-f]+) frame_p50=([-0-9.]+) p95=([-0-9.]+) "
    r"p99=([-0-9.]+) max=([-0-9.]+) frames=(\d+)")
STALL_RE = re.compile(
    r"\[STALL\]\s+calls=(\d+) requested=([-0-9.]+)ms actual=([-0-9.]+)ms "
    r"dsp10=(\d+)")
TICK_RE = re.compile(r"\[TICK\]\s+guest_ms=(\d+) vblank=(\d+) flips=(\d+)")
MODULE_RE = re.compile(r"\[D3D8HLE\] module=(.*?) draw_up=")


class GateError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# hashing / small utilities
# --------------------------------------------------------------------------

def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk)
            if not block:
                break
            h.update(block)
    return h.hexdigest().upper()


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def percentile(sorted_values: list[float], p: float) -> float:
    """Linear-interpolated percentile, matching output/qualified_stats.py."""
    if not sorted_values:
        return 0.0
    k = (len(sorted_values) - 1) * p / 100.0
    f = int(k)
    c = min(f + 1, len(sorted_values) - 1)
    d = k - f
    return sorted_values[f] * (1 - d) + sorted_values[c] * d


def clean_recomp_env() -> dict[str, str]:
    return {k: v for k, v in os.environ.items()
            if not k.upper().startswith("RECOMP_")}


def resolve(path: str | Path) -> Path:
    return Path(path).resolve()


def norm_env_path(value: str) -> Path:
    path = Path(value.lstrip("@"))
    if not path.is_absolute():
        path = ROOT / path
    return path.resolve()


# --------------------------------------------------------------------------
# host preflight (one-shot, no polling or sleep)
# --------------------------------------------------------------------------

DEFAULT_HOST_LIMITS = {
    "max_cpu_percent": 10.0,
    "min_free_memory_mb": 4096,
    "max_gpu_util": 20.0,
    "max_gpu_temp_c": 80.0,
}
HOST_FORBIDDEN = {
    "curse_of_darkness_xbox_recomp", "msbuild", "cl", "link", "clang-cl",
    "clang", "cvtres", "ml", "rc",
}


def _host_number(value, default=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def host_probe() -> dict:
    """One-shot host snapshot. No sleep, polling loop, or background sampler."""
    snapshot = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}
    script = (
        "$total=(Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor "
        "-Filter \"Name='_Total'\").PercentProcessorTime;"
        "$os=Get-CimInstance Win32_OperatingSystem;"
        "$cpu=Get-CimInstance Win32_Processor | Select-Object -First 1;"
        "$proc=Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | "
        "Where-Object {$_.Name -notmatch '^_'} | "
        "Sort-Object {[int]$_.PercentProcessorTime} -Descending | "
        "Select-Object -First 10 Name,IDProcess,PercentProcessorTime;"
        "$names=(Get-Process).ProcessName;"
        "[pscustomobject]@{cpu_percent=[double]$total;"
        "free_memory_mb=[math]::Round($os.FreePhysicalMemory/1024);"
        "load_percentage=[double]$cpu.LoadPercentage;"
        "top=@($proc);process_names=@($names)} | "
        "ConvertTo-Json -Depth 5 -Compress"
    )
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, timeout=30)
        if proc.returncode != 0 or not proc.stdout.strip():
            raise GateError(proc.stderr.strip() or "host probe returned no data")
        data = json.loads(proc.stdout)
        top = data.get("top") or []
        names = data.get("process_names") or []
        if isinstance(top, dict):
            top = [top]
        if isinstance(names, str):
            names = [names]
        snapshot.update({
            "cpu_percent": _host_number(data.get("cpu_percent")),
            "free_memory_mb": _host_number(data.get("free_memory_mb")),
            "load_percentage": _host_number(data.get("load_percentage")),
            "top_processes": top,
            "process_count": len(set(names)),
            "forbidden_processes": sorted({
                str(n).lower() for n in names
                if str(n).lower() in HOST_FORBIDDEN}),
        })
    except Exception as exc:
        snapshot["error"] = f"host probe failed: {exc}"
        return snapshot

    try:
        out = subprocess.run(["powercfg", "/getactivescheme"],
                             capture_output=True, text=True, timeout=15)
        line = out.stdout.strip()
        match = re.search(r"\((.*?)\)\s*$", line)
        snapshot["power_plan"] = match.group(1) if match else line
    except Exception as exc:
        snapshot["power_plan_error"] = str(exc)

    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,utilization.gpu,temperature.gpu,"
             "power.draw,clocks.sm,clocks.max.sm",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=15)
        if out.returncode == 0 and out.stdout.strip():
            fields = [x.strip() for x in out.stdout.splitlines()[0].split(",")]
            if len(fields) >= 6:
                snapshot["gpu"] = {
                    "name": fields[0],
                    "utilization": _host_number(fields[1]),
                    "temperature": _host_number(fields[2]),
                    "power_w": _host_number(fields[3]),
                    "clock_mhz": _host_number(fields[4]),
                    "max_clock_mhz": _host_number(fields[5]),
                }
    except Exception as exc:
        snapshot["gpu_error"] = str(exc)
    return snapshot


def preflight_reasons(snapshot: dict, limits: dict | None = None) -> dict:
    limits = {**DEFAULT_HOST_LIMITS, **(limits or {})}
    reasons = []
    warnings = []
    if "error" in snapshot:
        reasons.append(snapshot["error"])
    cpu = snapshot.get("cpu_percent")
    load = snapshot.get("load_percentage")
    if cpu is not None and cpu > limits["max_cpu_percent"]:
        reasons.append(
            f"CPU busy {cpu:.0f}% > {limits['max_cpu_percent']:.0f}%")
    if load is not None and load > limits["max_cpu_percent"]:
        reasons.append(
            f"CPU load {load:.0f}% > {limits['max_cpu_percent']:.0f}%")
    free = snapshot.get("free_memory_mb")
    if free is not None and free < limits["min_free_memory_mb"]:
        reasons.append(
            f"free memory {free:.0f} MB < {limits['min_free_memory_mb']:.0f} MB")
    plan = (snapshot.get("power_plan") or "").lower()
    if plan and "power saver" in plan:
        reasons.append(f"power plan is {snapshot['power_plan']}")
    elif plan and not ("high performance" in plan or "ultimate" in plan):
        warnings.append(
            f"power plan is {snapshot['power_plan']}; High/Ultimate expected")
    gpu = snapshot.get("gpu") or {}
    util = gpu.get("utilization")
    temp = gpu.get("temperature")
    if util is not None and util > limits["max_gpu_util"]:
        reasons.append(
            f"GPU util {util:.0f}% > {limits['max_gpu_util']:.0f}%")
    if temp is not None and temp > limits["max_gpu_temp_c"]:
        reasons.append(
            f"GPU temp {temp:.0f}C > {limits['max_gpu_temp_c']:.0f}C")
    forbidden = snapshot.get("forbidden_processes")
    if forbidden is None:
        names = {str(n).lower() for n in snapshot.get("process_names") or []}
        forbidden = sorted(names & HOST_FORBIDDEN)
    else:
        forbidden = sorted(forbidden)
    if forbidden:
        reasons.append(f"forbidden processes running: {forbidden}")
    return {"ok": not reasons, "reasons": reasons, "warnings": warnings,
            "limits": limits}


# --------------------------------------------------------------------------
# frozen-input derivation (no dependency on the mutable bench modules)
# --------------------------------------------------------------------------

def parse_calibration_points(text: str) -> list[tuple[float, int]]:
    points: list[tuple[float, int]] = []
    for line in text.splitlines():
        m = BENCH_RE.search(line)
        if m:
            points.append((float(m.group(1)), int(m.group(2))))
    if len(points) < 2:
        raise GateError("calibration has fewer than two frame-timestamped points")
    if any(m1 <= m0 or f1 <= f0
           for (m0, f0), (m1, f1) in zip(points, points[1:])):
        raise GateError("calibration timestamps/flips are not increasing")
    return points


def flip_at(ms: float, points: list[tuple[float, int]]) -> int:
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


def derived_scene_flips(points: list[tuple[float, int]]) -> tuple:
    return tuple((name, flip_at(a, points), flip_at(b, points))
                 for name, a, b in (
                     ("title", 15000, 18000),
                     ("main_menu", 19000, 23500),
                     ("cutscene", 25000, 31500),
                     ("courtyard_light", 54000, 60000),
                     ("pause", 62000, 70000),
                     ("courtyard_heavy", 74000, 86000),
                     ("combat", 88000, 104000),
                 ))


def require_frozen_inputs() -> dict:
    if not FROZEN_CAL.exists():
        raise GateError(f"missing frozen calibration: {FROZEN_CAL}")
    if not FROZEN_PAD.exists():
        raise GateError(f"missing frozen frame pad: {FROZEN_PAD}")
    cal_sha = sha256_file(FROZEN_CAL)
    pad_sha = sha256_file(FROZEN_PAD)
    if cal_sha != FROZEN_CAL_SHA:
        raise GateError(f"frozen calibration hash changed: {cal_sha}")
    if pad_sha != FROZEN_PAD_SHA:
        raise GateError(f"frozen frame-pad hash changed: {pad_sha}")
    derived = derived_scene_flips(parse_calibration_points(read_text(FROZEN_CAL)))
    if derived != SCENE_FLIPS:
        raise GateError(f"scene flips moved: {derived}")
    if not (GATE_START < GATE_END
            and any(name == GATE_SCENE and a <= GATE_START and GATE_END <= b
                    for name, a, b in SCENE_FLIPS)):
        raise GateError("gate window is outside the frozen courtyard_light scene")
    if not all(GATE_START <= f < GATE_END for f in CAPTURE_FLIPS):
        raise GateError("capture flips are outside the gate window")
    return {"calibration_sha256": cal_sha, "frame_pad_sha256": pad_sha,
            "scene_flips": [list(x) for x in SCENE_FLIPS]}


# --------------------------------------------------------------------------
# log / framestats parsing
# --------------------------------------------------------------------------

def parse_framestats(path: Path) -> list[dict]:
    with open(path, newline="", encoding="ascii") as f:
        rows = list(csv.reader(f))
    if not rows or [c.strip() for c in rows[0]] != ["flip", "us", "capture"]:
        raise GateError(f"{path}: unexpected header {rows[0] if rows else None}")
    out = []
    last = 0
    for i, row in enumerate(rows[1:], start=2):
        if len(row) != 3:
            raise GateError(f"{path}:{i}: expected 3 columns")
        try:
            flip, us, cap = int(row[0]), int(row[1]), int(row[2])
        except ValueError as exc:
            raise GateError(f"{path}:{i}: non-integer row: {exc}") from exc
        if flip <= last:
            raise GateError(f"{path}:{i}: flip {flip} not increasing (prev {last})")
        if us <= 0:
            raise GateError(f"{path}:{i}: non-positive duration")
        if cap not in (0, 1):
            raise GateError(f"{path}:{i}: capture flag must be 0 or 1")
        last = flip
        out.append({"flip": flip, "us": us, "capture": cap})
    return out


def window_metrics(rows: list[dict], start: int, end: int) -> dict:
    window = [r for r in rows if start <= r["flip"] < end]
    problems = []
    flips = [r["flip"] for r in window]
    if flips != list(range(start, end)):
        missing = sorted(set(range(start, end)) - set(flips))
        extra = sorted(set(flips) - set(range(start, end)))
        problems.append(
            f"window not contiguous: got {len(flips)} of {end - start} "
            f"presented flips (first missing {missing[:3]}, extra {extra[:3]})")
    ms = sorted(r["us"] / 1000.0 for r in window)
    total = sum(ms)
    return {
        "start": start,
        "end": end,
        "frames": len(window),
        "expected_frames": end - start,
        "p50_ms": round(percentile(ms, 50), 4),
        "p95_ms": round(percentile(ms, 95), 4),
        "p99_ms": round(percentile(ms, 99), 4),
        "max_ms": round(ms[-1], 4) if ms else 0.0,
        "mean_ms": round(total / len(ms), 4) if ms else 0.0,
        "sum_ms": round(total, 4),
        "interval_rate_hz": round(1000.0 * len(ms) / total, 3) if total else 0.0,
        "frac_gt_16_667": round(sum(1 for x in ms if x > 16.667) / len(ms), 4)
        if ms else 0.0,
        "frac_gt_20": round(sum(1 for x in ms if x > 20.0) / len(ms), 4)
        if ms else 0.0,
        "frac_gt_25": round(sum(1 for x in ms if x > 25.0) / len(ms), 4)
        if ms else 0.0,
        "problems": problems,
    }


def parse_pace_buckets(path: Path) -> list[dict]:
    buckets: list[dict] = []
    open_bucket = None
    last_bench_ms = None
    last_bench_flip = None
    for line in read_text(path).splitlines():
        m = PACE_RE.search(line)
        if m:
            open_bucket = {
                "flips": int(m.group(1)),
                "frames": int(m.group(14)),
                "frame_p50_ms": float(m.group(10)),
                "flip": None,
                "ms": None,
                "dsp10": None,
                "requested_ms": None,
                "actual_ms": None,
                "tick_guest_ms": None,
            }
            buckets.append(open_bucket)
            continue
        m = STALL_RE.search(line)
        if m and open_bucket is not None:
            open_bucket["calls"] = int(m.group(1))
            open_bucket["requested_ms"] = float(m.group(2))
            open_bucket["actual_ms"] = float(m.group(3))
            open_bucket["dsp10"] = int(m.group(4))
            continue
        m = BENCH_RE.search(line)
        if m:
            last_bench_ms = float(m.group(1))
            last_bench_flip = int(m.group(2))
            if open_bucket is not None and open_bucket["flip"] is None:
                open_bucket["flip"] = last_bench_flip
                open_bucket["ms"] = last_bench_ms
            continue
        m = TICK_RE.search(line)
        if m and open_bucket is not None:
            open_bucket["tick_guest_ms"] = int(m.group(1))
    return buckets


def bucket_checks(buckets: list[dict], start_ms: float, end_ms: float) -> dict:
    """DSP/guest-clock parity over the gate window.

    [BENCH] flips are cumulative snapshots taken once per second, so buckets
    cannot be attributed to a single flip exactly.  Select by the millisecond
    range that the frozen calibration maps to the gate flips, with a one-second
    margin, and require DSP 10 ms calls to match presented frames there.
    """
    selected = [b for b in buckets
                if b["ms"] is not None
                and start_ms - 1000 <= b["ms"] <= end_ms + 1000
                and b["frames"] > 0]
    problems = []
    for b in selected:
        if b["dsp10"] != b["frames"]:
            problems.append(
                f"bucket ms={b['ms']:.0f}: dsp10={b['dsp10']} != frames={b['frames']}")
        if (b["requested_ms"] is not None
                and abs(b["actual_ms"] - b["requested_ms"]) > 0.5):
            problems.append(
                f"bucket ms={b['ms']:.0f}: stall requested {b['requested_ms']} "
                f"!= actual {b['actual_ms']}")
    guest_rate = None
    ticked = [b for b in selected if b["tick_guest_ms"] is not None]
    if len(ticked) >= 2 and ticked[-1]["ms"] > ticked[0]["ms"]:
        guest_rate = ((ticked[-1]["tick_guest_ms"] - ticked[0]["tick_guest_ms"])
                      / ((ticked[-1]["ms"] - ticked[0]["ms"]) / 1000.0))
        if not 950 <= guest_rate <= 1050:
            problems.append(
                f"guest clock {guest_rate:.0f} ms/s outside 950-1050")
    if len(selected) < 2:
        problems.append("fewer than two PACE/STALL buckets in the gate window")
    return {"buckets": len(selected), "guest_ms_per_s": round(guest_rate, 1)
            if guest_rate is not None else None, "problems": problems}


def forbidden_stderr_lines(path: Path) -> list[str]:
    hits = []
    for line in read_text(path).splitlines():
        if any(tag in line for tag in FORBIDDEN_STDERR):
            hits.append(line[:160])
    return hits


def parse_guard_events(path: Path) -> dict:
    text = read_text(path)
    counts = {name: text.count(pattern) for name, pattern in GUARD_PATTERNS.items()}
    counts["fb_refusing_capped"] = counts["fb_refusing"] >= 4
    return counts


# --------------------------------------------------------------------------
# render-capture denominator audit (read-only, for the existing reports)
# --------------------------------------------------------------------------

def audit_render_capture(directory: Path) -> dict:
    """Check a render-capture artifact for denominator/replay validity.

    The record-only capture emits one swap/present row per renderer frame, not
    per presented flip, and its per-bind deltas are measured inside SetTexture.
    Any report that divides those rows by a probe's presented-flip count mixes
    two counters.
    """
    directory = resolve(directory)
    findings = []
    csv_path = directory / "render_capture.csv"
    manifest_path = directory / "render_capture.csv.manifest.csv"
    probe_path = directory / "probe.json"
    counts: dict[str, int] = {}
    if csv_path.exists():
        with open(csv_path, newline="", encoding="ascii") as f:
            for row in csv.DictReader(f):
                if row.get("type") in ("swap", "present", "copy", "readback",
                                        "clear"):
                    counts[row["type"]] = int(row["count"])
    swap = counts.get("swap")
    if swap is None:
        findings.append("no swap row: renderer frame denominator unavailable")
    span = None
    if manifest_path.exists():
        for line in read_text(manifest_path).splitlines():
            parts = line.split(",")
            if parts and parts[0] == "texture_frame_range" and len(parts) >= 5:
                a, _, b = parts[4].partition("..")
                if a.isdigit() and b.isdigit():
                    span = int(b) - int(a)
    if span is not None and swap is not None and span != swap:
        findings.append(
            f"texture_frame_range span {span} != swap rows {swap}")
    probe_frames = None
    if probe_path.exists():
        probe_frames = json.loads(read_text(probe_path)).get("frames")
        if probe_frames is not None and swap is not None and probe_frames != swap:
            findings.append(
                f"denominator mismatch: probe presented frames {probe_frames} "
                f"vs renderer swap rows {swap} "
                f"(probe is {probe_frames / swap:.3f}x the renderer count)")
    manifest_text = read_text(manifest_path) if manifest_path.exists() else ""
    binds = re.search(r"^texture_binds,,,,\s*(\d+)$", manifest_text, re.M)
    events = re.search(r"^renderer_events,,,,\s*(\d+)$", manifest_text, re.M)
    if binds and events and binds.group(1) == events.group(1):
        findings.append(
            "renderer_events == texture_binds: renderer bytes were measured "
            "around SetTexture, not at draw-time texture consumption")
    replay = [p.name for p in directory.iterdir()
              if p.name.lower().startswith(("replay", "replayed"))]
    if not replay:
        findings.append("record-only capture: no replay artifact or consumer")
    return {
        "directory": str(directory),
        "renderer_frames": swap,
        "probe_presented_frames": probe_frames,
        "recommended_denominator": swap,
        "counts": counts,
        "findings": findings,
    }


# --------------------------------------------------------------------------
# run validation
# --------------------------------------------------------------------------

def ms_at_flip(flip: int, points: list[tuple[float, int]]) -> float:
    for (m0, f0), (m1, f1) in zip(points, points[1:]):
        if f0 <= flip <= f1:
            if f1 == f0:
                return m1
            return m0 + (m1 - m0) * (flip - f0) / (f1 - f0)
    raise GateError(f"flip {flip} is outside the calibration")


def expected_env(run_dir: Path, capture: bool) -> dict[str, str]:
    env = {
        "RECOMP_D3D8GL": "1",
        "RECOMP_FB_WINDOW": "1",
        "RECOMP_PAD_FRAMES": "1",
        "RECOMP_PAD_SCRIPT": "@" + str(run_dir / "pad_script.txt"),
        "RECOMP_PACE_STATS": "1",
        "RECOMP_BENCH_STATS": "1",
        "RECOMP_FRAME_STATS": "1",
        "RECOMP_FRAME_STATS_FILE": str(run_dir / "framestats.csv"),
        "RECOMP_FRAME_STATS_START": "0",
        "RECOMP_FRAME_STATS_END": str(GATE_END),
    }
    if capture:
        env["RECOMP_CAPTURE_FLIPS"] = ",".join(str(x) for x in CAPTURE_FLIPS)
        env["RECOMP_CAPTURE_DIR"] = str(run_dir)
    return env


def env_problems(recorded: dict[str, str], run_dir: Path, capture: bool) -> list[str]:
    problems = []
    allowed = CAPTURE_ENV_KEYS if capture else GATE_ENV_KEYS
    keys = {k for k in recorded if k.upper().startswith("RECOMP_")}
    extra = sorted(keys - allowed)
    missing = sorted(allowed - keys)
    if extra:
        problems.append(f"diagnostic env present: {extra}")
    if missing:
        problems.append(f"required env missing: {missing}")
    want = expected_env(run_dir, capture)
    for key in sorted(keys & allowed):
        got = recorded[key]
        exp = want[key]
        if key in ("RECOMP_PAD_SCRIPT", "RECOMP_FRAME_STATS_FILE",
                   "RECOMP_CAPTURE_DIR"):
            if norm_env_path(got) != norm_env_path(exp):
                problems.append(f"{key} points elsewhere: {got}")
        elif got != exp:
            problems.append(f"{key}={got!r}, expected {exp!r}")
    return problems


def validate_run(run_dir: Path, mode: str = "timing",
                 legacy: bool = False,
                 expect_exe_sha: str | None = None,
                 expect_dll_sha: str | None = None) -> dict:
    run_dir = resolve(run_dir)
    capture = mode == "capture"
    errors: list[str] = []
    warnings: list[str] = []
    metrics = None

    if not run_dir.is_dir():
        raise GateError(f"not a run directory: {run_dir}")

    # --- identity ---------------------------------------------------------
    identity = {}
    run_manifest = run_dir / "run.json"
    identity_manifest = run_dir.parent.parent / "identity.json"
    if run_manifest.exists():
        identity = json.loads(read_text(run_manifest))
    else:
        warnings.append("run.json missing: binary identity unverifiable")
    if identity_manifest.exists():
        arm_identity = json.loads(read_text(identity_manifest))
        for key in ("exe_sha256", "dll_sha256"):
            if key in identity and arm_identity.get(key) != identity[key]:
                errors.append(
                    f"run {key} differs from staged arm {key}")
    elif not legacy:
        warnings.append("arm identity.json missing: load identity unverified")
    if expect_exe_sha:
        if identity.get("exe_sha256", "").upper() != expect_exe_sha.upper():
            errors.append("executable hash does not match the expected baseline")
    if expect_dll_sha:
        if identity.get("dll_sha256", "").upper() != expect_dll_sha.upper():
            errors.append("DLL hash does not match the expected baseline")
    if identity.get("loaded_module"):
        loaded = Path(identity["loaded_module"])
        if loaded.name.lower() != "d3d8gl.dll":
            errors.append(f"loaded module is not d3d8gl.dll: {loaded}")
        if identity.get("loaded_module_sha256") and identity.get("dll_sha256"):
            if identity["loaded_module_sha256"].upper() != identity["dll_sha256"].upper():
                errors.append("loaded DLL hash differs from the staged DLL")
        if identity.get("dll_sha256") and loaded.exists():
            if sha256_file(loaded) != identity["dll_sha256"].upper():
                errors.append("loaded DLL hash differs from staged DLL")
    elif not legacy:
        warnings.append("loaded module path not recorded (identity run)")

    # --- env --------------------------------------------------------------
    env_path = run_dir / "env.json"
    if env_path.exists():
        env = json.loads(read_text(env_path))
        errors.extend(env_problems(env.get("recomp", {}), run_dir, capture))
    elif legacy:
        warnings.append("env.json missing: environment unverifiable")
    else:
        errors.append("env.json missing: gate runs must record their environment")

    # --- inputs -----------------------------------------------------------
    pad = run_dir / "pad_script.txt"
    if not pad.exists():
        errors.append("pad_script.txt missing")
    elif sha256_file(pad) != FROZEN_PAD_SHA:
        errors.append("pad script does not match the frozen frame-exact input")

    # --- completion -------------------------------------------------------
    status_path = run_dir / "run_status.json"
    status = {}
    if status_path.exists():
        status = json.loads(read_text(status_path))
        if not status.get("complete"):
            errors.append(f"run incomplete: {status}")
        if status.get("required_flip") not in (None, GATE_END):
            errors.append(
                f"required_flip={status.get('required_flip')} != {GATE_END}")
        if status.get("max_flip") is not None and status["max_flip"] < GATE_END:
            errors.append("max presented flip is below the gate window end")
    else:
        errors.append("run_status.json missing")

    # --- frame durations --------------------------------------------------
    fs = run_dir / "framestats.csv"
    if fs.exists():
        rows = parse_framestats(fs)
        metrics = window_metrics(rows, GATE_START, GATE_END)
        errors.extend(metrics["problems"])
        captured = [r["flip"] for r in rows if r["capture"]]
        if capture:
            if sorted(captured) != list(CAPTURE_FLIPS):
                errors.append(
                    f"capture flips {captured} != {list(CAPTURE_FLIPS)}")
        elif captured:
            errors.append(
                f"timing run wrote capture frames {captured}; use a separate "
                f"capture run")
    else:
        errors.append("framestats.csv missing (frame recorder did not reach END)")

    # --- diagnostics ------------------------------------------------------
    stderr_path = run_dir / "xbox_stderr.log"
    guard = {}
    if stderr_path.exists():
        hits = forbidden_stderr_lines(stderr_path)
        if hits:
            errors.append(f"diagnostic output present: {hits[:3]}")
        guard = parse_guard_events(stderr_path)
        if guard["crash"]:
            errors.append(f"crash markers present: {guard['crash']}")
        if guard["fb_refusing"] or guard["fb_not_reading"]:
            warnings.append(
                f"frame-source refusals: {guard['fb_refusing']} present + "
                f"{guard['fb_not_reading']} window"
                + (" (log capped at 4)" if guard["fb_refusing_capped"] else ""))
        if metrics is not None:
            points = parse_calibration_points(read_text(FROZEN_CAL))
            start_ms = ms_at_flip(GATE_START, points)
            end_ms = ms_at_flip(GATE_END, points)
            parity = bucket_checks(parse_pace_buckets(stderr_path),
                                   start_ms, end_ms)
            errors.extend(parity["problems"])
            metrics["dsp_buckets"] = parity["buckets"]
            metrics["guest_ms_per_s"] = parity["guest_ms_per_s"]
    else:
        errors.append("xbox_stderr.log missing")

    # --- capture artifacts ------------------------------------------------
    bmp_caps = sorted(run_dir.glob("capture_*.bmp"))
    present_caps = sorted(run_dir.glob("present_*.bmp"))
    frame_dumps = sorted(run_dir.glob("frame.bmp*"))
    if capture:
        if len(present_caps) + len(bmp_caps) < len(CAPTURE_FLIPS):
            warnings.append("fewer than three capture images were copied")
    elif bmp_caps or present_caps:
        warnings.append("timing run contains capture images")
    if frame_dumps:
        errors.append(
            f"periodic frame dumps present ({len(frame_dumps)}); they are not "
            f"part of the diagnostic-off environment")

    return {
        "run": str(run_dir),
        "mode": mode,
        "errors": errors,
        "warnings": warnings,
        "identity": identity,
        "status": status,
        "guard": guard,
        "metrics": metrics,
    }


# --------------------------------------------------------------------------
# arm comparison
# --------------------------------------------------------------------------

def decide_verdict(baseline_p50: list[float], candidate_p50: list[float],
                   min_effect_ms: float = 0.5,
                   max_arm_spread_ms: float = 1.5,
                   baseline_expected_ms: tuple[float, float] = (17.5, 20.5)) -> dict:
    """Pure decision rule: non-overlapping arms and a pre-registered effect.

    The spread bound is 1.5 ms because the frozen arm itself spans 1.23 ms in
    the gate window across three sequential runs.  A smaller apparent win is
    not claimable; a wider arm means the host state was not controlled.
    """
    if len(baseline_p50) < 2 or len(candidate_p50) < 2:
        raise GateError("each arm needs at least two timing runs")
    b_med = statistics.median(baseline_p50)
    c_med = statistics.median(candidate_p50)
    spread_b = max(baseline_p50) - min(baseline_p50)
    spread_c = max(candidate_p50) - min(candidate_p50)
    effect = b_med - c_med  # positive = candidate is faster
    non_overlap = (max(candidate_p50) < min(baseline_p50)
                   or min(candidate_p50) > max(baseline_p50))
    problems = []
    warnings = []
    if spread_b > max_arm_spread_ms or spread_c > max_arm_spread_ms:
        problems.append(
            f"arm spread too wide (baseline {spread_b:.3f} ms, "
            f"candidate {spread_c:.3f} ms); host state is not controlled")
    reproduces = baseline_expected_ms[0] <= b_med <= baseline_expected_ms[1]
    if not reproduces:
        warnings.append(
            f"baseline median {b_med:.3f} ms is outside the frozen "
            f"{baseline_expected_ms[0]}-{baseline_expected_ms[1]} ms range; "
            f"the historical baseline is not reproduced")
    if problems:
        verdict = "invalid"
    elif non_overlap and effect >= min_effect_ms:
        verdict = "candidate_faster"
    elif non_overlap and effect <= -min_effect_ms:
        verdict = "candidate_slower"
    else:
        verdict = "inconclusive"
    return {
        "baseline_median_ms": round(b_med, 4),
        "candidate_median_ms": round(c_med, 4),
        "effect_ms": round(effect, 4),
        "baseline_spread_ms": round(spread_b, 4),
        "candidate_spread_ms": round(spread_c, 4),
        "non_overlap": non_overlap,
        "min_effect_ms": min_effect_ms,
        "baseline_expected_ms": list(baseline_expected_ms),
        "baseline_reproduces_frozen": reproduces,
        "problems": problems,
        "warnings": warnings,
        "verdict": verdict,
        "note": "p50 frame intervals only; no sustained-FPS claim is derived",
    }


def compare_arms(baseline: Path, candidate: Path, run_pattern: str = "*",
                 min_effect_ms: float = 0.5,
                 max_arm_spread_ms: float = 1.5) -> dict:
    def collect(directory: Path) -> list[dict]:
        out = []
        for run in sorted(directory.glob(run_pattern)):
            if not run.is_dir():
                continue
            if "capture" in run.name:
                continue
            result = validate_run(run, mode="timing")
            if result["errors"]:
                raise GateError(
                    f"{run} failed validation: {result['errors'][:3]}")
            out.append(result)
        if len(out) < 2:
            raise GateError(f"{directory}: need at least two timing runs")
        return out

    base = collect(resolve(baseline))
    cand = collect(resolve(candidate))
    b = [r["metrics"]["p50_ms"] for r in base]
    c = [r["metrics"]["p50_ms"] for r in cand]
    result = decide_verdict(b, c, min_effect_ms, max_arm_spread_ms)
    result["baseline_p50_ms"] = b
    result["candidate_p50_ms"] = c
    guard_base = {k: sum(int(r.get("guard", {}).get(k, 0)) for r in base)
                  for k in GUARD_PATTERNS}
    guard_cand = {k: sum(int(r.get("guard", {}).get(k, 0)) for r in cand)
                  for k in GUARD_PATTERNS}
    divergence = {
        k: {"baseline": guard_base[k], "candidate": guard_cand[k]}
        for k in GUARD_MATERIAL if guard_cand[k] > guard_base[k]
    }
    capped = any(r.get("guard", {}).get("fb_refusing_capped") for r in base + cand)
    result["guard"] = {
        "baseline": guard_base,
        "candidate": guard_cand,
        "divergence": divergence,
        "fb_refusing_count_is_floor": capped,
        "note": "[FB] present refusals print at most four lines per process",
    }
    if divergence:
        result["problems"].append(
            f"guard/executor events increased in candidate: {divergence}")
    base_ids = {(r["identity"].get("exe_sha256"),
                 r["identity"].get("dll_sha256")) for r in base}
    cand_ids = {(r["identity"].get("exe_sha256"),
                 r["identity"].get("dll_sha256")) for r in cand}
    if len(base_ids) > 1 or len(cand_ids) > 1:
        result["problems"].append("binary identity changes within an arm")
    if base_ids & cand_ids:
        result["problems"].append(
            "baseline and candidate arms share an identical exe+DLL pair")
    if result["problems"]:
        result["verdict"] = "invalid"
    result["pairs"] = [
        {"pair": i + 1,
         "baseline_p50_ms": b[i],
         "candidate_p50_ms": c[i],
         "candidate_minus_baseline_ms": round(c[i] - b[i], 4)}
        for i in range(min(len(b), len(c)))
    ]
    return result


# --------------------------------------------------------------------------
# capture equivalence
# --------------------------------------------------------------------------

def capture_images(run_dir: Path) -> dict[int, Path]:
    run_dir = resolve(run_dir)
    found = {}
    for path in sorted(run_dir.glob("present_*.bmp")):
        m = re.search(r"present_(\d+)\.bmp$", path.name)
        if m:
            found[int(m.group(1))] = path
    return found


def changed_pixel_ratio(a: Path, b: Path, threshold: int = 8) -> float | None:
    try:
        from PIL import Image, ImageChops
    except Exception as exc:  # pragma: no cover - Pillow is normally present
        raise GateError(f"Pillow is required for capture comparison: {exc}")
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return None
    diff = ImageChops.difference(ia, ib)
    pixels = ia.size[0] * ia.size[1]
    changed = sum(1 for px in diff.getdata() if max(px) > threshold)
    return changed / pixels


def compare_captures(baseline_dirs: list[Path], candidate_dir: Path,
                     margin: float = 0.005) -> dict:
    """Compare matched-flip captures against a same-binary null distribution.

    The game animates and blinking is open, so pixel equality is not expected.
    The null is baseline-vs-baseline at the same flips from the same script.
    """
    baseline = [resolve(p) for p in baseline_dirs]
    candidate = resolve(candidate_dir)
    if len(baseline) < 2:
        raise GateError("capture comparison needs at least two baseline runs")
    sets = [capture_images(p) for p in baseline] + [capture_images(candidate)]
    flips = set(sets[0])
    if not flips:
        raise GateError("no present_<flip>.bmp captures in the baseline run")
    for name, s in zip([str(p) for p in baseline] + [str(candidate)], sets):
        if set(s) != flips:
            raise GateError(
                f"{name}: capture flips {sorted(s)} != {sorted(flips)}")
    null = []
    for i in range(len(baseline)):
        for j in range(i + 1, len(baseline)):
            for flip in sorted(flips):
                ratio = changed_pixel_ratio(sets[i][flip], sets[j][flip])
                if ratio is not None:
                    null.append(ratio)
    candidate_ratios = []
    for i in range(len(baseline)):
        for flip in sorted(flips):
            ratio = changed_pixel_ratio(sets[i][flip], sets[-1][flip])
            candidate_ratios.append({
                "baseline": str(baseline[i]), "flip": flip,
                "changed_fraction": round(ratio, 6) if ratio is not None else None})
    null_max = max(null) if null else 0.0
    cand_max = max((r["changed_fraction"] or 0.0) for r in candidate_ratios)
    equivalent = cand_max <= null_max + margin
    return {
        "flips": sorted(flips),
        "null_max_fraction": round(null_max, 6),
        "candidate_max_fraction": round(cand_max, 6),
        "margin": margin,
        "candidate_ratios": candidate_ratios,
        "verdict": "equivalent_within_null" if equivalent else "divergence",
        "note": ("matched flips and frozen input only; a single capture set is "
                 "not a correctness proof, and blinking remains open"),
    }


# --------------------------------------------------------------------------
# staging / snapshot / host run
# --------------------------------------------------------------------------

def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="ascii")


def stage_arm(name: str, exe: Path, dll: Path, force: bool = False) -> Path:
    dest = ARMS / name
    if dest.exists():
        if not force:
            raise GateError(f"{dest} exists; pass --force to replace it")
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    exe, dll = resolve(exe), resolve(dll)
    if not exe.is_file() or exe.suffix.lower() != ".exe":
        raise GateError(f"not an executable: {exe}")
    if not dll.is_file() or dll.name.lower() != "d3d8gl.dll":
        raise GateError(f"not a d3d8gl.dll: {dll}")
    shutil.copy2(exe, dest / "game.exe")
    shutil.copy2(dll, dest / "d3d8gl.dll")
    if sha256_file(dest / "game.exe") != sha256_file(exe):
        raise GateError("staged executable copy changed")
    if sha256_file(dest / "d3d8gl.dll") != sha256_file(dll):
        raise GateError("staged DLL copy changed")
    write_json(dest / "identity.json", {
        "arm": name,
        "exe_path": str(exe),
        "exe_sha256": sha256_file(exe),
        "exe_bytes": exe.stat().st_size,
        "dll_path": str(dll),
        "dll_sha256": sha256_file(dll),
        "dll_bytes": dll.stat().st_size,
        "staged_exe": str(dest / "game.exe"),
        "staged_dll": str(dest / "d3d8gl.dll"),
        "note": ("LoadLibraryA('d3d8gl.dll') resolves to the executable "
                 "directory first; run with cwd=repository root for assets."),
    })
    return dest


def _root_logs() -> list[Path]:
    return [ROOT / "xbox_stdout.log", ROOT / "xbox_stderr.log",
            ROOT / "xbox_kernel.log"]


def _acquire_lock() -> Path:
    GATE.mkdir(parents=True, exist_ok=True)
    lock = GATE / ".host.lock"
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as exc:
        raise GateError(
            f"{lock} exists: a host run is already active or crashed; the "
            f"orchestrator must serialize host runs") from exc
    with os.fdopen(fd, "w") as f:
        f.write(json.dumps({"pid": os.getpid(), "started": time.time()}))
    return lock


def _release_lock(lock: Path) -> None:
    try:
        lock.unlink()
    except OSError:
        pass


def latest_flip() -> int:
    try:
        data = read_text(ROOT / "xbox_stderr.log")
    except OSError:
        return 0
    found = re.findall(r"\[BENCH\] ms=\d+ flips=(\d+)", data)
    return int(found[-1]) if found else 0


def identity_run(arm: str) -> dict:
    arm_dir = resolve(ARMS / arm)
    identity = json.loads(read_text(arm_dir / "identity.json"))
    run_dir = arm_dir / "identity"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    lock = _acquire_lock()
    try:
        for p in _root_logs():
            p.unlink(missing_ok=True)
        env = clean_recomp_env()
        env.update({
            "RECOMP_D3D8GL": "1",
            "RECOMP_FB_WINDOW": "1",
            "RECOMP_D3D8GL_TRACE": "1",
            "RECOMP_PAD_FRAMES": "1",
            "RECOMP_PAD_SCRIPT": "@" + str(FROZEN_PAD),
        })
        p = subprocess.Popen([str(arm_dir / "game.exe")], cwd=ROOT, env=env)
        try:
            deadline = time.monotonic() + 90
            module = None
            while time.monotonic() < deadline and p.poll() is None:
                try:
                    m = MODULE_RE.search(read_text(ROOT / "xbox_stderr.log"))
                except OSError:
                    m = None
                if m:
                    module = m.group(1).strip()
                    break
                time.sleep(0.1)
        finally:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        for src in _root_logs():
            if src.exists():
                shutil.copy2(src, run_dir / src.name)
        if not module:
            raise GateError(f"no [D3D8HLE] module line in {run_dir}")
        module_path = resolve(module)
        loaded_sha = sha256_file(module_path) if module_path.exists() else None
        result = {
            "exe_sha256": identity["exe_sha256"],
            "dll_sha256": identity["dll_sha256"],
            "loaded_module": str(module_path),
            "loaded_module_sha256": loaded_sha,
            "matches_staged": (str(module_path) ==
                               str(resolve(arm_dir / "d3d8gl.dll"))
                               and loaded_sha == identity["dll_sha256"]),
        }
        write_json(run_dir / "identity_run.json", result)
        write_json(run_dir.parent / "identity_loaded.json", result)
        if not result["matches_staged"]:
            raise GateError(f"loaded DLL is not the staged DLL: {result}")
        return result
    finally:
        _release_lock(lock)


def launch_run(arm: str, run_name: str, capture: bool,
               wall_timeout: int = DEFAULT_WALL_TIMEOUT,
               force: bool = False, preflight: bool = False) -> Path:
    arm_dir = resolve(ARMS / arm)
    identity = json.loads(read_text(arm_dir / "identity.json"))
    loaded_path = arm_dir / "identity_loaded.json"
    loaded = json.loads(read_text(loaded_path)) if loaded_path.exists() else {}
    if not loaded.get("matches_staged"):
        raise GateError(
            f"{arm}: run `gate.py identity --arm {arm}` first; the loaded DLL "
            f"identity is not established")
    run_dir = arm_dir / "runs" / run_name
    if run_dir.exists():
        if not force:
            raise GateError(f"{run_dir} exists; pass --force to replace it")
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    host_before = host_probe()
    write_json(run_dir / "host_before.json", host_before)
    verdict = preflight_reasons(host_before)
    write_json(run_dir / "preflight.json", verdict)
    if preflight and not verdict["ok"]:
        raise GateError(
            f"host preflight failed, refusing to launch: {verdict['reasons']}")
    shutil.copy2(FROZEN_PAD, run_dir / "pad_script.txt")
    lock = _acquire_lock()
    try:
        for p in _root_logs():
            p.unlink(missing_ok=True)
        (run_dir / "framestats.csv").unlink(missing_ok=True)
        env = clean_recomp_env()
        env.update(expected_env(run_dir, capture))
        write_json(run_dir / "env.json", {
            "recomp": {k: v for k, v in env.items()
                       if k.upper().startswith("RECOMP_")},
            "cwd": str(ROOT),
            "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        })
        started = time.monotonic()
        p = subprocess.Popen([str(arm_dir / "game.exe")], cwd=ROOT, env=env)
        complete = False
        try:
            while time.monotonic() - started < wall_timeout and p.poll() is None:
                fs = run_dir / "framestats.csv"
                if fs.exists():
                    caps = list(run_dir.glob("present_*.bmp"))
                    if not capture or len(caps) >= len(CAPTURE_FLIPS):
                        # The recorder writes and closes the file once, at
                        # GATE_END.  Wait for the size to settle so terminate
                        # cannot truncate it.
                        last = -1
                        for _ in range(20):
                            size = fs.stat().st_size
                            if size > 0 and size == last:
                                complete = True
                                break
                            last = size
                            time.sleep(0.25)
                        if complete:
                            break
                time.sleep(0.1)
        finally:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
        for src in _root_logs():
            if src.exists():
                shutil.copy2(src, run_dir / src.name)
        max_flip = 0
        fs = run_dir / "framestats.csv"
        if fs.exists():
            max_flip = max((r["flip"] for r in parse_framestats(fs)), default=0)
        host_after = host_probe()
        write_json(run_dir / "host_after.json", host_after)
        write_json(run_dir / "postflight.json", preflight_reasons(host_after))
        write_json(run_dir / "run.json", {
            "arm": arm,
            "exe_sha256": identity["exe_sha256"],
            "dll_sha256": identity["dll_sha256"],
            "loaded_module": loaded.get("loaded_module"),
            "loaded_module_sha256": loaded.get("loaded_module_sha256"),
            "mode": "capture" if capture else "timing",
            "gate_start": GATE_START,
            "gate_end": GATE_END,
            "capture_flips": list(CAPTURE_FLIPS) if capture else [],
        })
        write_json(run_dir / "run_status.json", {
            "required_flip": GATE_END,
            "max_flip": max_flip,
            "complete": complete and max_flip >= GATE_END,
            "timed_out": not complete,
            "wall_seconds": round(time.monotonic() - started, 3),
        })
        return run_dir
    finally:
        _release_lock(lock)


def snapshot() -> dict:
    frozen = require_frozen_inputs()
    exe = ROOT / "xbox" / "build" / "Release" / "curse_of_darkness_xbox_recomp.exe"
    root_dll = ROOT / "d3d8gl.dll"
    out_dll = ROOT / "output" / "d3d8gl.dll"
    result = {
        "frozen_inputs": frozen,
        "current_tree": {
            "exe": {"path": str(exe), "exists": exe.exists(),
                    "sha256": sha256_file(exe) if exe.exists() else None},
            "root_dll": {"path": str(root_dll), "exists": root_dll.exists(),
                         "sha256": sha256_file(root_dll)
                         if root_dll.exists() else None},
            "output_dll": {"path": str(out_dll), "exists": out_dll.exists(),
                           "sha256": sha256_file(out_dll)
                           if out_dll.exists() else None},
        },
        "frozen_binaries": {
            "exe_expected_sha256": FROZEN_EXE_SHA,
            "dll_expected_sha256": FROZEN_DLL_SHA,
            "exe_found": False,
            "dll_found": False,
        },
        "blockers": [],
        "snapshot_requirements": [
            "worktree snapshot: HEAD, tools/xboxrecomp HEAD, binary diff, "
            "untracked/generated manifest, and hashes of every build input",
            "retained bytes for the exact exe and d3d8gl.dll (relink changes "
            "the hash even with identical source)",
            "toolchain: clang-cl/MSVC and CMake versions plus exact build "
            "commands",
            "runtime inputs: extracted/default.xbe hash, assets, frozen "
            "calibration, frame pad, scene flip table, env allowlist",
            "host state: CPU, GPU/driver, OS build, power plan, refresh rate, "
            "resolution, no concurrent load",
            "per-run identity: exe hash, loaded module path and DLL hash from "
            "an identity run",
        ],
    }
    for p in sorted(ROOT.rglob("curse_of_darkness_xbox_recomp.exe")):
        try:
            if sha256_file(p) == FROZEN_EXE_SHA:
                result["frozen_binaries"]["exe_found"] = str(p)
        except OSError:
            pass
    for p in sorted(ROOT.rglob("d3d8gl.dll")):
        try:
            if sha256_file(p) == FROZEN_DLL_SHA:
                result["frozen_binaries"]["dll_found"] = str(p)
        except OSError:
            pass
    if not result["frozen_binaries"]["exe_found"]:
        result["blockers"].append(
            f"frozen executable {FROZEN_EXE_SHA} is not present anywhere in "
            f"the worktree")
    if not result["frozen_binaries"]["dll_found"]:
        result["blockers"].append(
            f"frozen DLL {FROZEN_DLL_SHA} is not present anywhere in the "
            f"worktree; current DLL is "
            f"{result['current_tree']['root_dll']['sha256']}")
    xbe = ROOT / "extracted" / "default.xbe"
    result["runtime_inputs"] = {
        "default_xbe": str(xbe),
        "default_xbe_sha256": sha256_file(xbe) if xbe.exists() else None,
    }
    result["git"] = {}
    for label, cwd in (("main", ROOT), ("tools_xboxrecomp", ROOT / "tools" / "xboxrecomp")):
        try:
            head = subprocess.run(["git", "-C", str(cwd), "rev-parse", "HEAD"],
                                  capture_output=True, text=True, timeout=15)
            status = subprocess.run(["git", "-C", str(cwd), "status",
                                     "--porcelain=v1"],
                                    capture_output=True, text=True, timeout=30)
            result["git"][label] = {
                "head": head.stdout.strip(),
                "dirty_entries": len([x for x in status.stdout.splitlines() if x]),
            }
        except OSError as exc:
            result["git"][label] = {"error": str(exc)}
    write_json(GATE / "snapshot.json", result)
    return result


def plan() -> str:
    base = ("python xbox/bench/gate.py")
    lines = [
        "# 0. Validate frozen inputs and current identity (read-only)",
        f"{base} snapshot",
        "",
        "# 1. Stage the frozen baseline and the candidate (staged dirs only;",
        "#    do not overwrite xbox/build or repo-root d3d8gl.dll)",
        f"{base} stage --name baseline --exe <FROZEN_EXE> --dll <FROZEN_DLL>",
        f"{base} stage --name candidate --exe <CANDIDATE_EXE> --dll <CANDIDATE_DLL>",
        "",
        "# 1b. One-shot host-idle preflight; no game, no polling loop",
        f"{base} preflight",
        "",
        "# 2. Prove which DLL Windows loads (trace on, not a timing run)",
        f"{base} identity --arm baseline --confirm-host-run",
        f"{base} identity --arm candidate --confirm-host-run",
        "",
        "# 3. Three timing pairs, order AB BA AB, serialized",
        f"{base} run --arm baseline --run 1 --confirm-host-run --preflight",
        f"{base} run --arm candidate --run 1 --confirm-host-run --preflight",
        f"{base} run --arm candidate --run 2 --confirm-host-run --preflight",
        f"{base} run --arm baseline --run 2 --confirm-host-run --preflight",
        f"{base} run --arm baseline --run 3 --confirm-host-run --preflight",
        f"{base} run --arm candidate --run 3 --confirm-host-run --preflight",
        "",
        "# 4. Separate capture runs: three baseline (for the null) plus the",
        "#    candidate at the same three consecutive flips",
        f"{base} run --arm baseline --run capture1 --captures --confirm-host-run",
        f"{base} run --arm baseline --run capture2 --captures --confirm-host-run",
        f"{base} run --arm baseline --run capture3 --captures --confirm-host-run",
        f"{base} run --arm candidate --run capture1 --captures --confirm-host-run",
        "",
        "# 5. Validate and compare (p50 frame intervals only)",
        f"{base} validate --dir xbox/bench/out/gate/arms/baseline/runs/run1",
        f"{base} validate --dir xbox/bench/out/gate/arms/candidate/runs/run1",
        f"{base} validate --mode capture --dir "
        f"xbox/bench/out/gate/arms/baseline/runs/capture1",
        f"{base} validate --mode capture --dir "
        f"xbox/bench/out/gate/arms/candidate/runs/capture1",
        f"{base} compare --baseline xbox/bench/out/gate/arms/baseline/runs "
        f"--candidate xbox/bench/out/gate/arms/candidate/runs",
        f"{base} compare-captures "
        f"--baseline xbox/bench/out/gate/arms/baseline/runs/capture1 "
        f"xbox/bench/out/gate/arms/baseline/runs/capture2 "
        f"xbox/bench/out/gate/arms/baseline/runs/capture3 "
        f"--candidate xbox/bench/out/gate/arms/candidate/runs/capture1",
    ]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _print_result(result: dict) -> None:
    print(json.dumps(result, indent=2))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("plan", help="print the ordered gate commands")
    sub.add_parser("snapshot", help="verify frozen inputs and record identity")

    p = sub.add_parser("stage", help="copy exe+dll into an isolated arm dir")
    p.add_argument("--name", required=True)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--dll", type=Path, required=True)
    p.add_argument("--force", action="store_true")

    p = sub.add_parser("identity", help="record the loaded DLL path/hash")
    p.add_argument("--arm", required=True)
    p.add_argument("--confirm-host-run", action="store_true")

    p = sub.add_parser("preflight",
                       help="one-shot host-idle check; no game launch")
    p.add_argument("--max-cpu", type=float,
                   default=DEFAULT_HOST_LIMITS["max_cpu_percent"])
    p.add_argument("--min-free-mb", type=float,
                   default=DEFAULT_HOST_LIMITS["min_free_memory_mb"])
    p.add_argument("--max-gpu-util", type=float,
                   default=DEFAULT_HOST_LIMITS["max_gpu_util"])
    p.add_argument("--max-gpu-temp", type=float,
                   default=DEFAULT_HOST_LIMITS["max_gpu_temp_c"])

    p = sub.add_parser("run", help="run one bounded gate replay")
    p.add_argument("--arm", required=True)
    p.add_argument("--run", required=True)
    p.add_argument("--captures", action="store_true")
    p.add_argument("--wall-timeout", type=int, default=DEFAULT_WALL_TIMEOUT)
    p.add_argument("--force", action="store_true")
    p.add_argument("--preflight", action="store_true",
                   help="refuse to launch if the host is not idle")
    p.add_argument("--confirm-host-run", action="store_true")

    p = sub.add_parser("validate", help="validate a completed run directory")
    p.add_argument("--dir", type=Path, required=True)
    p.add_argument("--mode", choices=("timing", "capture"), default="timing")
    p.add_argument("--legacy", action="store_true",
                   help="tolerate missing gate manifests")
    p.add_argument("--expect-exe-sha")
    p.add_argument("--expect-dll-sha")

    p = sub.add_parser("compare", help="paired baseline/candidate comparison")
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--min-effect-ms", type=float, default=0.5)
    p.add_argument("--max-arm-spread-ms", type=float, default=1.5)

    p = sub.add_parser("audit-capture",
                       help="check render-capture denominator/replay validity")
    p.add_argument("--dir", type=Path, required=True)

    p = sub.add_parser("compare-captures",
                       help="matched-flip capture comparison against a null")
    p.add_argument("--baseline", type=Path, nargs="+", required=True)
    p.add_argument("--candidate", type=Path, required=True)
    p.add_argument("--margin", type=float, default=0.005)

    a = ap.parse_args(argv)
    try:
        if a.cmd == "plan":
            print(plan())
            return 0
        if a.cmd == "snapshot":
            _print_result(snapshot())
            return 0
        if a.cmd == "preflight":
            snap = host_probe()
            verdict = preflight_reasons(snap, {
                "max_cpu_percent": a.max_cpu,
                "min_free_memory_mb": a.min_free_mb,
                "max_gpu_util": a.max_gpu_util,
                "max_gpu_temp_c": a.max_gpu_temp,
            })
            _print_result({"snapshot": snap, "verdict": verdict})
            return 0 if verdict["ok"] else 1
        if a.cmd == "stage":
            dest = stage_arm(a.name, a.exe, a.dll, a.force)
            print(f"staged {dest}")
            return 0
        if a.cmd == "identity":
            if not a.confirm_host_run:
                print("refusing to launch; pass --confirm-host-run "
                      "(the orchestrator serializes host runs)", file=sys.stderr)
                return 2
            _print_result(identity_run(a.arm))
            return 0
        if a.cmd == "run":
            if not a.confirm_host_run:
                print("refusing to launch; pass --confirm-host-run "
                      "(the orchestrator serializes host runs)", file=sys.stderr)
                return 2
            run_dir = launch_run(a.arm, a.run, a.captures, a.wall_timeout,
                                 a.force, a.preflight)
            result = validate_run(run_dir, "capture" if a.captures else "timing")
            _print_result(result)
            return 0 if not result["errors"] else 1
        if a.cmd == "validate":
            result = validate_run(a.dir, a.mode, a.legacy,
                                  a.expect_exe_sha, a.expect_dll_sha)
            _print_result(result)
            return 0 if not result["errors"] else 1
        if a.cmd == "compare":
            result = compare_arms(a.baseline, a.candidate,
                                  min_effect_ms=a.min_effect_ms,
                                  max_arm_spread_ms=a.max_arm_spread_ms)
            _print_result(result)
            return 0 if result["verdict"] != "invalid" else 1
        if a.cmd == "audit-capture":
            _print_result(audit_render_capture(a.dir))
            return 0
        if a.cmd == "compare-captures":
            result = compare_captures(a.baseline, a.candidate, a.margin)
            _print_result(result)
            return 0 if result["verdict"] != "divergence" else 1
    except GateError as exc:
        print(f"gate: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
