"""Run a hidden, frame-bounded HLE trace without replacing existing logs."""
import argparse
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--aspect", type=float, help="enable the experimental aspect ratio, e.g. 1.7777778")
parser.add_argument("--fov-scale", type=float, help="experimental tangent-of-FOV multiplier")
parser.add_argument("--frames", default="600,1200,3000,3500", help="up to eight increasing renderer frames")
parser.add_argument("--guest-stack", action="store_true", help="capture guest call stacks around requested frames")
parser.add_argument("--sound-trace", action="store_true", help="enable opt-in transition crash diagnostics")
parser.add_argument("--guest-projection", action="store_true", help="enable experimental guest-side visibility projection")
parser.add_argument("--timeout", type=int, default=360, help="wall-clock timeout in seconds")
parser.add_argument("--pad-file", type=Path, help="override the frame-based controller script")
parser.add_argument("--live-pad", action="store_true", help="enable an append-only live controller script in the run directory")
args = parser.parse_args()
if args.aspect is not None and not 4 / 3 <= args.aspect <= 4:
    parser.error("aspect must be between 4:3 and 4:1")
if args.fov_scale is not None and not 0.5 <= args.fov_scale <= 2:
    parser.error("FOV scale must be between 0.5 and 2")
if args.timeout < 1 or args.timeout > 3600:
    parser.error("timeout must be between 1 and 3600 seconds")
try:
    frames = tuple(int(value) for value in args.frames.split(","))
    if not frames or len(frames) > 8 or any(x < 1 for x in frames) or sorted(set(frames)) != list(frames):
        raise ValueError
except ValueError:
    parser.error("frames must be up to eight increasing positive integers")
root = Path(__file__).resolve().parents[2]
pad = (args.pad_file.resolve() if args.pad_file else root / "output/qualified_checkpoint/frame_pad.txt")
if not pad.is_file():
    parser.error("controller script does not exist")
running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq curse_of_darkness_xbox_recomp.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True, check=True)
if '"curse_of_darkness_xbox_recomp.exe"' in running.stdout.lower():
    raise SystemExit("The game is already running; refusing to replace its logs.")
base = root / "output" / "widescreen_probe"
base.mkdir(parents=True, exist_ok=True)
out = Path(tempfile.mkdtemp(prefix="run_", dir=base))
print("probe output:", out, flush=True)
logs = [root / n for n in ("xbox_stdout.log", "xbox_stderr.log", "xbox_kernel.log")]
prior = {p: p.read_bytes() if p.exists() else None for p in logs}
for p, data in prior.items():
    if data is not None:
        (out / ("original_" + p.name)).write_bytes(data)
env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
env.update(RECOMP_WIDE_PROBE="1", RECOMP_D3D8GL="1", RECOMP_USB="1",
           RECOMP_PAD_SCRIPT="@" + str(pad),
           RECOMP_PAD_FRAMES="1", RECOMP_WIDE_TRACE_FRAMES=",".join(map(str, frames)),
           RECOMP_WIDE_GUEST_TRACE=",".join(map(str, frames)),
           RECOMP_D3D8GL_TRACE_FRAME="-1", RECOMP_WIDE_CAPTURE_DIR=str(out),
           RECOMP_WIDE_LOG=str(out / "renderer.log"),
           RECOMP_D3D8GL_DUMP_SHADERS=str(out),
           RECOMP_BENCH_STATS="1")
if args.aspect is not None or args.fov_scale is not None:
    env["RECOMP_WIDE_EXPERIMENT"] = "1"
    env["RECOMP_WIDE_ASPECT"] = str(args.aspect if args.aspect is not None else 4 / 3)
if args.fov_scale is not None:
    env["RECOMP_WIDE_FOV_SCALE"] = str(args.fov_scale)
if args.guest_projection:
    env["RECOMP_WIDE_GUEST_PROJECTION"] = "1"
if args.sound_trace:
    env["RECOMP_SOUND_TRACE"] = "1"
if args.live_pad:
    live_file = out / "live_pad.txt"
    live_file.write_text("", encoding="ascii")
    env["RECOMP_PAD_LIVE"] = str(live_file)
    print("live controller file:", live_file, flush=True)
if args.guest_stack:
    env["RECOMP_GUEST_PROFILE"] = "1"
    env["RECOMP_GUEST_PROFILE_START"] = str(max(0, frames[0] - 5))
    env["RECOMP_GUEST_PROFILE_END"] = str(frames[-1] + 5)
    env["RECOMP_GUEST_PROFILE_FILE"] = str(out / "guest_profile.csv")
proc = None
try:
    proc = subprocess.Popen([str(root / "xbox/build/Release/curse_of_darkness_xbox_recomp.exe")],
                            cwd=root, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    deadline = time.monotonic() + args.timeout
    while proc.poll() is None and time.monotonic() < deadline:
        if (out / f"frame{frames[-1]:05}.bmp").exists():
            break
        time.sleep(1)
    if proc.poll() is None:
        proc.terminate()
        proc.wait(timeout=10)
    for p in logs:
        if p.exists(): shutil.copy2(p, out / p.name)
    present = [(out / f"frame{frame:05}.bmp").exists() for frame in frames]
    print("captures", frames, ":", present, "exit:", proc.returncode)
    if not all(present): raise SystemExit(1)
finally:
    if proc and proc.poll() is None:
        proc.kill()
        proc.wait()
    for p, data in prior.items():
        if data is None:
            if p.exists(): p.unlink()
        else: p.write_bytes(data)
