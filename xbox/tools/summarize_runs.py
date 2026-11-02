"""Build benchmark tables from saved frame_bench runs, excluding incomplete runs."""
from __future__ import annotations
import argparse
import json
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "xbox/tools"))
import bench
import frame_bench


def valid_values(values):
    return [v for v in values if v is not None]


def median_or_none(values):
    v = valid_values(values)
    return statistics.median(v) if v else None


def completed_run_dirs(root: Path):
    runs = []
    for run in sorted(p for p in root.glob("run*") if p.is_dir()):
        status_path = run / "run_status.json"
        if status_path.exists():
            status = json.loads(status_path.read_text())
            if not status.get("complete", False):
                continue
        runs.append(run)
    return runs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run", type=Path)
    ap.add_argument("--calibration", type=Path, required=True)
    ap.add_argument("--format", choices=("markdown", "json"), default="markdown")
    a = ap.parse_args()
    points = frame_bench.calibration_points(bench.parse_log(a.calibration))
    runs = completed_run_dirs(a.run)
    table = {}
    for run in runs:
        log = run / "xbox_stderr.log"
        status_path = run / "run_status.json"
        status = json.loads(status_path.read_text()) if status_path.exists() else {}
        complete = status.get("complete", True)
        if not complete:
            continue
        samples = bench.parse_log(log)
        row = {}
        for name, s0, s1 in bench.MAIN_SCENES:
            fa, fb = frame_bench.flip_at(s0, points), frame_bench.flip_at(s1, points)
            r = frame_bench.summarize_flip(name, fa, fb, samples)
            row[name] = {
                "fps": r.median_fps() if r.fps else None,
                "samples": len(r.fps),
                "unique": r.present_unique_ratio,
                "guest": r.guest_rate,
            }
        table[run.name] = row
    result = {}
    for name, _, _ in bench.MAIN_SCENES:
        result[name] = {
            "runs": {run: table[run][name] for run in table},
            "median_fps": median_or_none([table[run][name]["fps"] for run in table]),
            "valid_samples": sum(table[run][name]["samples"] for run in table),
        }
    if a.format == "json":
        print(json.dumps(result, indent=2))
    else:
        print("| scene | " + " | ".join(table) + " | median | valid samples |")
        print("|---|" + "---:|" * (len(table) + 2))
        for name, _, _ in bench.MAIN_SCENES:
            cells = []
            for run in table:
                v = table[run][name]["fps"]
                cells.append("missing" if v is None else f"{v:.1f}")
            print(f"| {name} | " + " | ".join(cells) +
                  f" | {result[name]['median_fps']:.1f} | {result[name]['valid_samples']} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
