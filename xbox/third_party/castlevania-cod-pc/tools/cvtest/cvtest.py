"""Gameplay tests: runs the built game in a test folder of its own (its own settings and saves, the play folder is not
touched), plays a scenario turn by turn through CV_PAD_LIVE (src/common/keyboard.cpp) and checks every step.

usage: python -I tools/cvtest/cvtest.py <scenario.json> [--build out/build/release] [--keep] [--judge[=<model>]]
                                        [--review=<model>|none] [--set <setting>=<value> ...]
                                        [--release eu]   the European release's executable (CastlevaniaEU)
       python -I tools/cvtest/cvtest.py --record <route> [--save <name>]    play, and keep it as routes/<route>.json
       python -I tools/cvtest/cvtest.py --make-route <telemetry.jsonl> <route>

A scenario is JSON:
  {"name": "...",
   "after": "<scenario>.json",            optional: that scenario's steps first (a path beside this one)
   "save": "<name>",                      optional: start from the saves a step kept with save_as
   "settings": {"display_mode": ...},     optional: castlevania.toml lines (windowed 960x540 by default)
   "dump": "<va>:<bytes>,...",            optional: guest memory saved after every command (game/live/mem-<n>-<va>.bin)
   "telemetry": <frames>,                 optional: the state every that many frames, game/telemetry.jsonl
   "perf": true,                          optional: the SDK's timings (XBE_PERF, a log line set each second): a
                                          step's own, those of the seconds it played, are its perf, in the report
   "shader_cache": "<name>",              optional: compiled shaders kept in out/test/shader-cache/<name> for the
                                          next run with that name (a run's own, so cold, when left out)
   "judge": "gpt-6-luna",                optional: every screenshot judged (judge.py; a step's "judge": false
                                          skips it, true is gpt-6-luna): screen.judge has scene, defect and hector,
                                          and a likely rendering defect or a scene the game state disagrees with is
                                          a warning. A Codex model takes some 4 s a shot; "jev-latest" well under
                                          one, but it is the weaker judge, so it only screens
   "review": "gpt-6-luna",                optional: with Jev, the Codex model that looks again at a shot Jev
                                          flags, its answers then the step's (the default; false for none)
   "steps": [step, ...]}
A step plays one of
  "press": "<entries>"                    keyboard.cpp's syntax, "0:a,500:b:300" or frames "0f:lsu:60f"
  "until": "<expression>"                 plays "poll" (default "30f:wait") again until the expression holds, at most
                                          "timeout" seconds (default 60); "poll_by": [["<expression>", "<entries>"],
                                          ...] plays the first whose expression held after the last poll instead
  "sweep": {"maps": [m, ...]}             warps to every room of those maps (all when left out), from the room
                                          table the game wrote (rooms.json), and keeps a screenshot of each; a room
                                          that fails does not stop the sweep. "rooms": ["m.r", ...] warps to those
                                          instead, "hold": <frames> stands that long in each after (with "perf",
                                          the room's timings)
  "route": "<route>"                      walks routes/<route>.json from its first room: the bot (src/common/bot.cpp)
                                          through each room's points, pressing where the recording pressed and
                                          playing its menus where it played them, out by the way the recording
                                          left; enemies within "guard" (default 600, 0 for none) are fought on the
                                          way and story scenes skipped; "box_shots": true keeps a screenshot of
                                          each box that came up on the way (<step>_box-<n>.png)
and may have
  "expect": ["<expression>", ...]         each must hold after the step
  "shot": "<name>"                        keeps the screenshot as <name>.png
  "save_as": "<name>"                     keeps the game's saves (hdd) for a later scenario's "save"
  "measure": {"<name>": [x0, y0, x1, y1]} the edge strength (sharpness) of those parts of the screenshot, as
                                          fractions of the window: screen.edges.<name>, the mean brightness step
                                          between neighbouring pixels (render scale and texture filtering move it)
  "ask": {"question": "...",              asks a Codex model (judge.py, gpt-6-luna) about the screenshot:
          "choices": {"<name>": "...", ...}}   screen.ask.answer is the choice's name, screen.ask.why its reason
  "note": "..."                           shown in the report
Expressions are Python over the game state (src/common/telemetry.cpp) after the step: frame, map, room,
player.active, player.pos (x, y, z), player.yaw, player.hp, camera.heading, menu, modal, ...; prev is the state
before the step, screen has mean, std and flat (one colour) and, with a judge, judge.scene, judge.defect,
judge.hector and judge.by (the model; judge.first has Jev's answers a review replaced), step has fps, seconds and
saved (the game wrote a save during it), settings the run's settings (--set changes them), shots["<shot>"] the
screen of an earlier step with that shot (shots["a"].edges.floor < screen.edges.floor). A field the game did
not report is None.

Each step also fails when the game exits, a command does not finish in time, or the log has a new error or guest
trap. The report (report.md, results.json, a screenshot per step) goes to out/test/<scenario>-<time>/.

--record starts the game as for play (the play folder's settings, keyboard and controllers; a fresh new game, or
the saves of --save) with its state recorded (CV_TELEMETRY, src/common/telemetry.cpp). Once the game is closed the
recording becomes a route, a leg per room as played, and the game's saves become saves/<route>.
"""
import ctypes
import itertools
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from ctypes import wintypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SAVES = REPO / "out" / "test" / "saves"
ROUTES = Path(__file__).resolve().parent / "routes"
ROUTE_BUTTONS = ("a", "b")  # presses a route keeps, outside fights: A jumps, B opens doors and starts save points
# (in a fight X and B attack, for 15 and 25 HP a hit: the bot fights there)
FIGHT_NEAR = 800  # an enemy this close: a fight, whose path and presses a route leaves to the guard
EXIT_BUTTONS = ("a", "b", "x", "y")  # pressed just before a room change: B opens doors
DEFAULT_SETTINGS = {"display_mode": "windowed", "width": 960, "height": 540, "texture_pack": True,
                    "pause_on_focus_loss": False}
FRAME_MS = 17  # a frame at 60 fps, for how long a frame-timed command may take

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
user32.SetProcessDPIAware()


# --- the game window -------------------------------------------------------------------------------------------

class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


def family(pid):
    """the process and those it started, and theirs: Castlevania.exe hands another release's game to its executable"""
    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    parents = {}
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = kernel32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        parents[e.th32ProcessID] = e.th32ParentProcessID
        ok = kernel32.Process32NextW(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    out, grew = {pid}, True
    while grew:
        grew = False
        for child, parent in parents.items():
            if parent in out and child not in out:
                out.add(child)
                grew = True
    return out


def windows(pid):
    """the visible top level windows of the process and of those it started"""
    found = []
    pids = family(pid)

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    return found


def find_window(pid):
    found = windows(pid)
    return found[0] if found else None


def window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def dialog_text(hwnd):
    """a message box's text, from its static controls"""
    texts = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(child, _):
        if window_class(child) == "Static":
            buf = ctypes.create_unicode_buffer(2048)
            user32.GetWindowTextW(child, buf, 2048)
            if buf.value:
                texts.append(buf.value)
        return True

    user32.EnumChildWindows(hwnd, each, 0)
    return " ".join(" ".join(texts).split())


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG),
                ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


def capture(hwnd):
    """the client area, drawn by the window itself (works behind other windows): width, height, BGRA rows"""
    r = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        return None
    wdc = user32.GetDC(hwnd)
    dc = gdi32.CreateCompatibleDC(wdc)
    bmp = gdi32.CreateCompatibleBitmap(wdc, w, h)
    old = gdi32.SelectObject(dc, bmp)
    user32.PrintWindow(hwnd, dc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT: what D3D presented
    info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)  # top-down
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(info), 0)
    gdi32.SelectObject(dc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(hwnd, wdc)
    return w, h, buf.raw


def write_png(path, w, h, bgra):
    rgb = bytearray(w * h * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
    rows = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def screen_stats(w, h, bgra):
    """mean and spread of the brightness over about 20,000 pixels; flat is one colour (black, white, a fade)"""
    step = max(1, (w * h) // 20000)
    lum = [(bgra[i + 2] * 299 + bgra[i + 1] * 587 + bgra[i] * 114) / 1000 for i in range(0, w * h * 4, step * 4)]
    mean = sum(lum) / len(lum)
    std = (sum((v - mean) ** 2 for v in lum) / len(lum)) ** 0.5
    return {"mean": round(mean, 1), "std": round(std, 1), "flat": std < 3}


def edges(w, h, bgra, box):
    """the mean brightness step between neighbouring pixels in box (fractions of the window), right and down; rows
    skipped over a large box, to keep under about 100,000 pixels"""
    x0, y0, x1, y1 = (round(f * n) for f, n in zip(box, (w, h, w, h)))
    x0, y0, x1, y1 = max(0, x0), max(0, y0), min(w - 1, x1), min(h - 1, y1)
    if x1 <= x0 or y1 <= y0:
        return None
    every = max(1, (x1 - x0) * (y1 - y0) // 100000)

    def lum(x, y):
        i = (y * w + x) * 4
        return bgra[i + 2] * 299 + bgra[i + 1] * 587 + bgra[i] * 114

    total = count = 0
    for y in range(y0, y1, every):
        row = [lum(x, y) for x in range(x0, x1 + 1)]
        below = [lum(x, y + 1) for x in range(x0, x1)]
        for x in range(x1 - x0):
            total += abs(row[x + 1] - row[x]) + abs(below[x] - row[x])
            count += 2
    return round(total / count / 1000, 2)


# --- expressions over the state ---------------------------------------------------------------------------------

class View:
    """a JSON object as attributes; a missing one is None"""

    def __init__(self, d):
        self._d = d or {}

    def __getattr__(self, k):
        return view(self._d.get(k))

    def __repr__(self):
        return json.dumps(self._d)


def view(v):
    """objects as Views, also inside lists, so `enemies[0].dist` reads"""
    if isinstance(v, dict):
        return View(v)
    if isinstance(v, list):
        return [view(x) for x in v]
    return v


def holds(expr, state, prev, screen, step, settings=None, shots=None):
    names = {k: view(v) for k, v in (state or {}).items()}
    names.update(prev=View(prev), screen=View(screen), step=View(step), settings=View(settings),
                 shots={k: View(v) for k, v in (shots or {}).items()}, abs=abs, min=min, max=max, round=round, len=len,
                 any=any, all=all)
    try:
        return bool(eval(expr, {"__builtins__": {}}, names)), None
    except Exception as e:  # a field the game did not report, usually
        return False, f"{type(e).__name__}: {e}"


# --- the run ------------------------------------------------------------------------------------------------------

def command_ms(entries):
    """how long a command plays, in ms, frames counted at 60 fps"""
    end = 0
    for e in entries.replace("\n", ",").split(","):
        f = e.strip().split(":")
        if len(f) < 2 or not f[0]:
            continue
        k = FRAME_MS if f[0].endswith("f") else 1
        hold = int(f[2].rstrip("f")) * k if len(f) > 2 and f[2] else (9 * FRAME_MS if k > 1 else 150)
        end = max(end, int(f[0].rstrip("f")) * k + hold)
    return end


def exe_name(scenario):
    """the executable a scenario plays: "release": "eu" the European release's"""
    return "CastlevaniaEU" if scenario.get("release") == "eu" else "Castlevania"


class Game:
    def __init__(self, build, folder, scenario, installed=False, env=None):
        """installed: folder is a player's install (install.py), played where it is with env as the whole
        environment; its first start builds the game code behind a window of its own (the SDK's
        runtime/app/game_code.cpp), whose Start is pressed once it is ready"""
        self.folder = folder
        self.exe = exe_name(scenario)
        if not installed:
            folder.mkdir(parents=True)
            for f in (self.exe + ".exe", self.exe + ".dll"):
                shutil.copy2(build / f, folder / f)  # dates kept: the exe takes a DLL that is not older than it
        settings = dict(DEFAULT_SETTINGS, **scenario.get("settings", {}))
        lines = [f"{k} = {json.dumps(v) if isinstance(v, str) else str(v).lower()}" for k, v in settings.items()]
        (folder / "castlevania.toml").write_text("\n".join(lines) + "\n")
        if scenario.get("save"):
            shutil.copytree(SAVES / scenario["save"], folder / "hdd")
        self.live = folder / "live"
        self.live.mkdir()
        if scenario.get("dump"):  # "<va>:<bytes>,...": guest memory saved after each command, mem-<n>-<va>.bin
            (self.live / "dump.txt").write_text(scenario["dump"] + "\n")
        self.log = folder / "logs" / "castlevania.log"
        self.log_pos = 0
        env = dict(os.environ if env is None else env, CV_PAD_LIVE=str(self.live))
        env.pop("CV_PAD_SCRIPT", None)
        if scenario.get("perf"):
            env["XBE_PERF"] = "1"
        if scenario.get("shader_cache"):
            env["XBE_SHADER_CACHE"] = str(REPO / "out" / "test" / "shader-cache" / scenario["shader_cache"])
        if scenario.get("telemetry"):  # the state every n frames, as a recording would have it
            env.update(CV_TELEMETRY=str(folder / "telemetry.jsonl"), CV_TELEMETRY_EVERY=str(scenario["telemetry"]))
        started = time.time()
        # an install is started as a player starts it, by Castlevania.exe, whatever the release
        self.proc = subprocess.Popen([str(folder / ("Castlevania.exe" if installed else self.exe + ".exe"))], cwd=folder,
                                     env=env)
        self.n = 0
        self.hwnd = None
        self.dialog = None  # a message box's text, shown instead of the game
        self.first_start = None  # seconds the game code took to build, when it was built
        deadline = started + (1800 if installed else 30)
        while not self.hwnd and not self.dialog and self.proc.poll() is None and time.time() < deadline:
            for h in windows(self.proc.pid):
                kind = window_class(h)
                if kind.endswith("GameCode"):  # building: Enter is Start once it is ready, nothing before
                    self.first_start = time.time() - started
                    user32.PostMessageW(h, 0x0100, 0x0D, 0)  # WM_KEYDOWN VK_RETURN
                elif kind == "#32770":
                    self.dialog = dialog_text(h) or "a message box"
                else:
                    self.hwnd = h
            time.sleep(0.5 if self.first_start is not None else 0.1)

    def alive(self):
        return self.proc.poll() is None

    def play(self, entries):
        """one command; the state after it, its frames per second, or an error"""
        self.n += 1
        tmp = self.live / "cmd.tmp"
        tmp.write_text("\n".join(e.strip() for e in entries.replace("\n", ",").split(",") if e.strip()) + "\n")
        os.replace(tmp, self.live / f"cmd-{self.n}.txt")
        done = self.live / f"done-{self.n}.txt"
        start = time.time()
        deadline = start + command_ms(entries) / 1000 + 30
        while not done.exists():
            if not self.alive():
                return None, None, f"the game exited (code {self.proc.returncode})"
            if time.time() > deadline:
                return None, None, "the command did not finish in time (the game hangs or runs slowly)"
            time.sleep(0.02)
        seconds = time.time() - start
        state = json.loads((self.live / f"state-{self.n}.json").read_text())
        return state, seconds, None

    def new_log_lines(self):
        if not self.log.exists():
            return []
        with open(self.log, "rb") as f:
            f.seek(self.log_pos)
            data = f.read()
            self.log_pos = f.tell()
        return data.decode("utf-8", "replace").splitlines()

    def close(self):
        if self.alive():
            if self.hwnd:
                user32.PostMessageW(self.hwnd, 0x0010, 0, 0)  # WM_CLOSE
            try:
                self.proc.wait(8)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# Jev's defect answers run high (tools/cvtest/bakeoff.py: 20 of 26 clean pictures over 0.5). Alone, a shot is called
# defective from JEV_DEFECT (41 of 44 right there, the colour swaps missed); screening for a review, from JEV_SCREEN,
# where it lets no defect by and sends 15 of 26 clean pictures on.
JEV_DEFECT = 0.93
JEV_SCREEN = 0.6


def judge_warnings(j, state, screen, cut=None):
    """what the judge saw that the step should hear of: a likely rendering defect (from cut, by default the
    judge's own), or a scene other than the one the game state says (when the state is sure: in play, the pause
    menu, a one-colour screen)"""
    if not j:
        return []
    out = []
    if j["defect"] >= (cut or (JEV_DEFECT if j["by"].startswith("jev") else 0.5)):
        out.append(f"{j['by']} sees a rendering defect ({j['defect']:.2f}" + (f": {j['what']})" if j.get("what") else ")"))
    p = state.get("player") or {}
    said = ("blank" if screen.get("flat") else "pause_menu" if state.get("menu") else
            None if state.get("modal") or state.get("cutscene") or state.get("loading")
            or (state.get("camera") or {}).get("special") else "gameplay" if p.get("active") and p.get("pos") else None)
    if said and j["scene"] != said and j.get("scene_p", {}).get(j["scene"], 1) >= 0.8:
        out.append(f"{j['by']} sees {j['scene']} where the game state says {said}")
    return out


def poll_until(game, expr, timeout, poll, poll_by, state):
    """plays poll (or the poll_by entries whose expression holds) until expr holds; the state, seconds, an error"""
    limit = time.time() + timeout
    seconds, prev = 0, state
    while True:
        entries = poll or "30f:wait"
        for when, then in poll_by:
            if state and holds(when, state, prev, {}, {})[0]:
                entries = then
                break
        state, s, err = game.play(entries)
        seconds += s or 0
        if err or holds(expr, state, prev, {}, {})[0]:
            return state, seconds, err
        if time.time() > limit:
            return state, seconds, f"until {expr!r} did not hold within {timeout} s"


SKIP_SCENES = [["cutscene", "0:start,45f:wait"], ["menu", "0:b,30f:wait"]]


def sweep_steps(game, spec):
    """a warp, a wait and a screenshot for each room in the game's room table (or those spec picks), and a stand
    in each when it asks for one"""
    table = json.loads((game.live / "rooms.json").read_text()) if (game.live / "rooms.json").exists() else []
    maps = set(spec.get("maps") or [g["map"] for g in table])
    rooms = [tuple(int(v) for v in r.split(".")) for r in spec.get("rooms", [])]
    out = []
    for m, room in rooms or [(g["map"], room) for g in table if g["map"] in maps for room in g["rooms"]]:
        out.append({"press": f"0:warp={m}.{room},10f:wait:20f", "note": f"warp {m}.{room}", "stop_on_fail": False})
        # a story scene the room starts is skipped with Start, a menu that opens instead is closed
        out.append({"until": f"map == {m} and room == {room} and player.active and not cutscene and not loading "
                             f"and not menu", "timeout": spec.get("timeout", 30), "poll_by": SKIP_SCENES,
                    "shot": f"room-{m}-{room}", "expect": ["not screen.flat"], "stop_on_fail": False,
                    "sweep_room": f"{m}.{room}"})
        if spec.get("hold"):
            out.append({"press": f"0f:wait:{spec['hold']}f", "shot": f"hold-{m}-{room}", "judge": False,
                        "expect": [f"map == {m} and room == {room}"], "stop_on_fail": False, "perf_room": f"{m}.{room}"})
    return out


PERF = [  # the SDK's perf lines (runtime/gpu/d3d12.cpp LogPerf), the figures kept of each
    (r"perf: ([\d.]+) fps ([\d.]+) ms/frame \| host ([\d.]+) \(draw ([\d.]+): .*compile ([\d.]+) pso ([\d.]+);"
     r".*present ([\d.]+) \[swap ([\d.]+) wait ([\d.]+)\]\) other ([\d.]+)",
     ("fps", "ms", "host", "draw", "compile", "pso", "present", "swap", "present_wait", "other")),
    (r"recording ([\d.]+) ms, waiting ([\d.]+) ms \(([\d.]+) waits: read-back ([\d.]+) resource ([\d.]+) fence "
     r"([\d.]+) flush ([\d.]+) copy ([\d.]+), next frame ([\d.]+)\)",
     ("recording", "waiting", "waits", "wait_readback", "wait_resource", "wait_fence", "wait_flush", "wait_copy",
      "wait_next_frame")),
    (r"per frame ([\d.]+) draws \(([\d.]+) raw\) ([\d.]+)k verts ([\d.]+) MB upload .*\| ([\d.]+) tex uploads "
     r"([\d.]+) MB, ([\d.]+) new tex, ([\d.]+) new targets, ([\d.]+) target uploads, ([\d.]+) shrinks, ([\d.]+) "
     r"read-backs ([\d.]+) late", ("draws", "raw_draws", "kverts", "upload_mb", "tex_uploads", "tex_mb", "new_tex",
                                   "new_targets", "target_uploads", "shrinks", "readbacks", "late")),
]


def perf_of(lines):
    """the mean of each figure over a step's perf seconds, the first left out: the game stood still before the step
    and that second has the wait in it"""
    seconds = {}
    for pattern, names in PERF:
        found = [m for m in (re.search(pattern, line) for line in lines) if m][1:]
        for i, name in enumerate(names):
            if found:
                seconds[name] = round(sum(float(m.group(i + 1)) for m in found) / len(found), 2)
        seconds.setdefault("seconds", len(found))
    return seconds if seconds.get("seconds") else None


# --- routes -------------------------------------------------------------------------------------------------------

def in_menu(s):
    """a menu, a message box or the map screen (its own camera) is up"""
    return bool(s.get("menu") or s.get("modal") or (s.get("camera") or {}).get("special"))


def playing(s):
    p = s.get("player") or {}
    return bool(p.get("active") and p.get("pos") and not s.get("cutscene") and not s.get("loading") and not in_menu(s))


def simplify(pts, eps):
    """Ramer-Douglas-Peucker: the indices of the points that keep the path within eps of it (of each segment, so
    walking back the same way is kept too)"""
    keep, stack = {0, len(pts) - 1}, [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        (ax, az), (bx, bz) = pts[a], pts[b]
        sx, sz = bx - ax, bz - az
        length2 = sx * sx + sz * sz or 1e-9
        far, at = 0, None
        for i in range(a + 1, b):
            x, z = pts[i]
            t = max(0.0, min(1.0, ((x - ax) * sx + (z - az) * sz) / length2))
            d = math.hypot(x - ax - t * sx, z - az - t * sz)
            if d > far:
                far, at = d, i
        if at is not None and far > eps:
            keep.add(at)
            stack += [(a, at), (at, b)]
    return sorted(keep)


def menu_macros(lines, presses):
    """the menus played through: {line index of the last play before one: (kind, entries)}, and the lines they took.
    A "menu" was opened by a press (Start, Back for the map), its entries timed from that press; a "box" came up by
    itself where Hector stood (a save point, an item, a hint), its entries timed from when it came up. A press still
    held when play goes on is cut there: it walks on rather than answers"""
    macros, taken, last_play, i = {}, set(), None, 0
    while i < len(lines):
        s = lines[i]
        if playing(s):
            last_play = i
        elif last_play is not None and in_menu(s):
            # the press that opened it came in the half second before (the pause menu is up ~16 frames after Start)
            near = itertools.takewhile(lambda j: lines[i]["frame"] - lines[j]["frame"] <= 30, range(i, -1, -1))
            opener = next((j for j in near if j in presses and j not in taken), None)
            start = i if opener is None else opener
            end = next((j for j in range(i, len(lines)) if playing(lines[j])), len(lines) - 1)
            f0, f1 = lines[start]["frame"], lines[end]["frame"]
            entries = [f"{lines[j]['frame'] - f0}f:{b}:{max(1, min(held, f1 - lines[j]['frame']))}f"
                       for j in range(start, end) for b, held in presses.get(j, [])]
            if entries:
                macros[last_play] = ("box" if opener is None else "menu", ",".join(entries))
                taken.update(range(start, end))
            i = end + 1
            continue
        i += 1
    return macros, taken


def make_route(lines):
    """recorded play (CV_TELEMETRY lines with "pad") as a route: a leg per room in the order played, each a list of
    points to walk through and where it walked out. A point is [x, z], [x, z, "a" or "b", frames] for a jump or B
    (doors, save points) there, [x, z, "menu" or "box", "<entries>"] for a menu played through there (menu_macros),
    [x, z, "fight", radius] where a fight that was won began; "exit_press" the buttons pressed as it walked out (B
    opens doors). Where an enemy was within FIGHT_NEAR the path keeps only where the fight began and ended, and no
    presses: the bot fights there (and its guard on the way)"""
    held_from, presses = {}, {}  # button: frame pressed; line index: [(button, frames held)]
    for i, s in enumerate(lines):
        for b in filter(None, s.get("pressed", "").split("+")):
            held_from[b] = (i, s["frame"])
        for b in filter(None, s.get("released", "").split("+")):
            if b in held_from:
                at, frame = held_from.pop(b)
                presses.setdefault(at, []).append((b, max(1, s["frame"] - frame)))
    macros, taken = menu_macros(lines, presses)
    legs = []
    for i, s in enumerate(lines):
        if not playing(s):
            continue
        if not legs or (legs[-1]["map"], legs[-1]["room"]) != (s["map"], s["room"]):
            legs.append({"map": s["map"], "room": s["room"], "samples": [], "enemies": 0, "frames": [s["frame"]] * 2})
        leg = legs[-1]
        leg["last"] = i
        x, _, z = s["player"]["pos"]
        fight = any(e.get("dist", 1e9) < FIGHT_NEAR for e in s.get("enemies") or [])
        press = None
        if i in macros:
            press = macros[i]
        elif not fight and i not in taken:
            press = next((p for p in presses.get(i, []) if p[0] in ROUTE_BUTTONS), None)
        leg["samples"].append([x, z, press, fight, i])
        leg["enemies"] = max(leg["enemies"], len(s.get("enemies") or []))
        leg["frames"][1] = s["frame"]
    exp = lambda i: (lines[min(i, len(lines) - 1)].get("player") or {}).get("exp") or 0
    route = {"legs": []}
    for n, leg in enumerate(legs):
        samples = leg.pop("samples")
        # a fight won (EXP up by its end, or a second after): fought there again, from where it began
        for k, smp in enumerate(samples):
            if smp[3] and (k == 0 or not samples[k - 1][3]) and not smp[2]:
                end = next((j for j in range(k, len(samples)) if not samples[j][3]), len(samples) - 1)
                if exp(samples[end][4] + 10) > exp(smp[4]):
                    smp[2] = ("fight", 1500)
        # a fight: its first and last sample, and any menu in it
        samples = [smp for k, smp in enumerate(samples) if not smp[3] or smp[2] or k in (0, len(samples) - 1)
                   or not samples[k - 1][3] or not samples[k + 1][3]]
        pts = [(x, z) for x, z, _, _, _ in samples]
        forced = [i for i, smp in enumerate(samples) if smp[2]]
        cuts = sorted({0, len(pts) - 1, *forced})
        idx = sorted({i for a, b in zip(cuts, cuts[1:]) for i in (a + j for j in simplify(pts[a:b + 1], 40))} | set(cuts))
        points, last = [], None
        for i in idx:
            x, z, press, _, _ = samples[i]
            if not press and last and math.hypot(x - last[0], z - last[1]) < 150 and i != len(pts) - 1:
                continue
            points.append([round(x), round(z)] + (list(press) if press else []))
            last = (x, z)
        leg["points"] = points
        last_line = leg.pop("last")
        if n + 1 < len(legs) and len(pts) > 1:  # walked out: on past the last point, the way the stick pushed
            ex, ez = pts[-1]
            dx, dz = stick_way(lines, last_line)
            if not dx and not dz:  # no stick: the way it was going
                back = next(((x, z) for x, z in reversed(pts) if math.hypot(ex - x, ez - z) >= 100), pts[0])
                dx, dz = ex - back[0], ez - back[1]
            d = math.hypot(dx, dz) or 1
            leg["exit"] = [round(ex + dx / d * 600), round(ez + dz / d * 600)]
            # a door opens with B: what was pressed in the half second before the room changed
            near = itertools.takewhile(lambda j: lines[last_line]["frame"] - lines[j]["frame"] <= 30,
                                       range(last_line, -1, -1))
            buttons = sorted({b for j in near for b, _ in presses.get(j, []) if b in EXIT_BUTTONS})
            if buttons:
                leg["exit_press"] = buttons
        route["legs"].append(leg)
    return route


def stick_way(lines, last):
    """where the left stick pushed Hector, across the floor, in the half second up to that line: (dx, dz), or (0, 0).
    A door in a wall is walked into, not along: the stick says so, the path (sliding along the wall) does not"""
    for s in itertools.takewhile(lambda t: lines[last]["frame"] - t["frame"] <= 30, reversed(lines[:last + 1])):
        pad, h = s.get("pad") or {}, (s.get("camera") or {}).get("heading")
        lx, ly = (pad.get("lx") or 0) / 32767, (pad.get("ly") or 0) / 32767
        if h is not None and math.hypot(lx, ly) > 0.5:  # forward (sin h, cos h), right (cos h, -sin h)
            return ly * math.sin(h) + lx * math.cos(h), ly * math.cos(h) - lx * math.sin(h)
    return 0.0, 0.0


def play_route(game, route, step, prev, box_shots=None):
    """walks a route leg by leg: each point a command of its own (the game waits between commands, so Hector walks
    on), a jump where the recording jumped, its menus and boxes answered as it answered them, story scenes skipped,
    enemies within "guard" fought on the way"""
    guard = step.get("guard", 600)
    state, seconds, notes = prev, 0.0, []
    boxes = 0

    def play(entries):
        nonlocal state, seconds
        st, s, err = game.play(entries)
        seconds += s or 0
        if st:
            state = st
            if (st.get("player") or {}).get("hp") == 0:
                return "Hector died"
        return err

    def keep_box(where):
        """box_shots: the box once it is drawn (it opens over some frames)"""
        nonlocal boxes
        if not box_shots or play("45f:wait") or not game.alive():
            return
        img = capture(game.hwnd)
        if img:
            boxes += 1
            write_png(Path(f"{box_shots}-{boxes}.png"), *img)
            notes.append(f"{where}: box shot {boxes}")

    def answer(p, where):
        """a menu or box point: the box waited for (up to 2 s), then the presses as recorded"""
        kind, entries = p[2], p[3]
        if kind == "box" and not state.get("modal"):
            for _ in range(10):
                if play("12f:wait") or state.get("modal"):
                    break
            else:
                notes.append(f"{where}: the box did not come up (an item that did not drop?)")
                return None
        if kind == "box":
            keep_box(where)
        err = play(f"{entries},{command_ms(entries) // FRAME_MS + 30}f:wait")
        if not err and in_menu(state):
            notes.append(f"{where}: still in a menu after {entries!r}")
        return err

    def close_box(where):
        """a box the recording did not have: a level up's goes by itself in ~2 s, an item's closes with A"""
        notes.append(f"{where}: a box the recording did not have")
        if state.get("modal"):
            keep_box(where)
        for t in range(6):
            if not state.get("modal") or play(("60f:wait", "0:a,30f:wait", "0:b,30f:wait")[t % 3]):
                return

    legs = route["legs"]
    n = 0
    while n < len(legs):
        leg = legs[n]
        here = f"map == {leg['map']} and room == {leg['room']}"
        st, s, err = poll_until(game, f"{here} and player.active and not cutscene and not loading and not menu", 30,
                                None, SKIP_SCENES, state)
        seconds += s
        state = st or state
        if err:
            return state, seconds, f"leg {n + 1}: not playing in room {leg['map']}.{leg['room']} ({err})", notes
        targets = [(p, 60 if len(p) > 2 else 120) for p in leg["points"]]
        if leg.get("exit"):  # right to where the recording left from, then on the way it pushed
            targets[-1] = (targets[-1][0], 40)
            targets.append((leg["exit"], 30))
        press, left, k = None, False, 0
        while k < len(targets):
            p, radius = targets[k]
            where = f"leg {n + 1} point {k + 1} {p[:2]}"
            exit_ = bool(leg.get("exit")) and k == len(targets) - 1
            for attempt in range(6):
                pos = (state.get("player") or {}).get("pos") or [p[0], 0, p[1]]
                ms = 5000 + 3 * int(math.hypot(p[0] - pos[0], p[1] - pos[2])) + (20000 if guard else 0)
                entries = [f"0:walk={p[0]}/{p[1]}/{radius}:{ms}"] + ([f"0:guard={guard}"] if guard else [])
                if press and attempt == 0:
                    entries.append(f"0f:{press[0]}:{press[1]}f")
                if exit_:  # what opened the door, pressed every 20 frames while pushing into it
                    entries += [f"{t}f:{b}:8f" for b in leg.get("exit_press", []) for t in range(0, 200, 20)]
                err = play(",".join(entries))
                if err:
                    return state, seconds, f"{where}: {err}", notes
                result = ((state.get("bot") or {}).get("results") or ["?"])[0]
                if result == "room":
                    left = True
                    break
                if result == "modal":  # a box on the way: the recording's, when it came up about there
                    at = (state.get("player") or {}).get("pos") or pos
                    j = next((j for j in range(k, min(k + 4, len(targets))) if len(targets[j][0]) > 2
                              and targets[j][0][2] == "box"
                              and math.hypot(targets[j][0][0] - at[0], targets[j][0][1] - at[2]) < 400), None)
                    if j is not None:
                        k = j
                        p, where = targets[j][0], f"leg {n + 1} point {j + 1} {targets[j][0][:2]}"
                        break
                    close_box(where)
                    continue
                if result == "reached" and not exit_:
                    break
                if result == "stuck":
                    return state, seconds, f"{where}: stuck", notes
                if state.get("cutscene") or state.get("menu"):
                    play("0:start,45f:wait" if state.get("cutscene") else "0:b,30f:wait")
                    notes.append(f"{where}: a {'story scene' if state.get('cutscene') else 'menu'} on the way")
                    continue
                if exit_:  # reached the point past the exit, or timed out, in the same room
                    return state, seconds, f"leg {n + 1}: the exit at {p} did not lead out of the room", notes
            else:
                return state, seconds, f"{where}: not reached in 6 tries", notes
            if left:
                break
            kind = p[2] if len(p) > 2 else None
            press = p[2:4] if kind == "a" else None  # a jump: running, on the way to the next point
            if kind in ROUTE_BUTTONS and kind != "a":  # B and the like: standing, then a second for what it starts
                err = play(f"0f:{p[2]}:{p[3]}f,0f:wait:60f")
                if err:
                    return state, seconds, f"{where}: {p[2]}: {err}", notes
            if kind in ("menu", "box"):
                err = answer(p, where)
                if err:
                    return state, seconds, f"{where}: {kind} {p[3]!r}: {err}", notes
            if kind == "fight":  # the recording won a fight here: the enemies around fought until gone
                err = play(f"0:fight={p[3]}:60000")
                if err:
                    return state, seconds, f"{where}: fight: {err}", notes
                result = ((state.get("bot") or {}).get("results") or ["?"])[0]
                if result == "modal":  # the last hit's level up, say
                    close_box(where)
                elif result != "cleared":
                    notes.append(f"{where}: the fight ended {result}")
            if state.get("room") is not None and (state.get("map"), state.get("room")) != (leg["map"], leg["room"]):
                left = True  # a press there opened a door
                break
            k += 1
        if press and not left:  # a jump on the route's last point
            play(f"0f:{press[0]}:{press[1]}f,0f:wait:30f")
        if left:  # where did it go: the next leg's room, or the route lost
            st, s, err = poll_until(game, "player.active and not loading", 30, None, SKIP_SCENES, state)
            seconds += s
            state = st or state
            got = (state.get("map"), state.get("room"))
            nxt = next((j for j in range(n + 1, len(legs)) if (legs[j]["map"], legs[j]["room"]) == got), None)
            if nxt is None:
                return state, seconds, f"leg {n + 1}: walked into room {got[0]}.{got[1]}, not on the route", notes
            if nxt != n + 1:
                notes.append(f"leg {n + 1}: walked into room {got[0]}.{got[1]}, the route's leg {nxt + 1}")
            n = nxt
        else:
            n += 1
    return state, seconds, None, notes


def record(name, build, save):
    """the game as usual (the play folder's settings, the keyboard and controllers), its state recorded; once it is
    closed the recording becomes routes/<name>.json, and its saves saves/<name>"""
    out = REPO / "out" / "test" / f"record-{name}-{time.strftime('%Y%m%d-%H%M%S')}"
    folder = out / "game"
    folder.mkdir(parents=True)
    for f in ("Castlevania.exe", "Castlevania.dll"):
        shutil.copy2(build / f, folder / f)
    if (build / "castlevania.toml").exists():
        shutil.copy2(build / "castlevania.toml", folder / "castlevania.toml")
    if save:
        shutil.copytree(SAVES / save, folder / "hdd")
    tel = out / "telemetry.jsonl"
    env = dict(os.environ, CV_TELEMETRY=str(tel), CV_TELEMETRY_EVERY="6")
    for k in ("CV_PAD_LIVE", "CV_PAD_SCRIPT"):
        env.pop(k, None)
    print(f"recording {name}: play, then close the game ({out})")
    subprocess.run([str(folder / "Castlevania.exe")], cwd=folder, env=env)
    if (folder / "hdd").exists():
        shutil.rmtree(SAVES / name, ignore_errors=True)
        shutil.copytree(folder / "hdd", SAVES / name)
    return save_route(tel, name)


def save_route(telemetry, name):
    lines = [json.loads(l) for l in Path(telemetry).read_text().splitlines() if l.strip()]
    route = make_route(lines)
    if not route["legs"]:
        print("nothing recorded in play")
        return False
    ROUTES.mkdir(exist_ok=True)
    legs = ",\n  ".join(json.dumps(l) for l in route["legs"])  # a leg a line
    (ROUTES / f"{name}.json").write_text(f'{{"recorded": "{time.strftime("%Y-%m-%d %H:%M")}",\n "legs": [\n  {legs}\n ]}}\n')
    rooms = " -> ".join(f"{l['map']}.{l['room']}" for l in route["legs"])
    points = sum(len(l["points"]) for l in route["legs"])
    print(f"routes/{name}.json: {len(route['legs'])} legs, {points} points, {rooms}")
    return True


def load(path):
    """a scenario with the steps of the ones it comes after"""
    scenario = json.loads(Path(path).read_text(encoding="utf-8"))
    if scenario.get("after"):
        first = load(Path(path).parent / scenario["after"])
        scenario["steps"] = first["steps"] + scenario["steps"]
        scenario.setdefault("save", first.get("save"))
    return scenario


def run(scenario_path, build, keep, judge_model=None, review=None, overrides=None, install=None, env=None, checks=(),
        release=None):
    """install: a player's install to play in (install.py), env its environment, checks its results to report first;
    release "eu": the European release's executable, which finds its own game files in assets (a disc image there)"""
    scenario = load(scenario_path)
    if release:
        scenario["release"] = release
    scenario["settings"] = dict(scenario.get("settings", {}), **(overrides or {}))
    settings = dict(DEFAULT_SETTINGS, **scenario["settings"])
    if judge_model:
        scenario["judge"] = judge_model
    if review:
        scenario["review"] = False if review == "none" else review
    name = scenario.get("name", Path(scenario_path).stem)
    out = REPO / "out" / "test" / f"{'install-' if install else ''}{name}-{time.strftime('%Y%m%d-%H%M%S')}"
    game = Game(build, install or out / "game", scenario, installed=bool(install), env=env)
    shots = out / "shots"
    shots.mkdir(parents=True)
    results = list(checks)
    prev, failed = None, not all(c["ok"] for c in results)
    if install:  # the first start: the game code built, Start pressed, the game window up
        r = {"step": "start", "label": "first start", "ok": bool(game.hwnd), "errors": [], "warnings": []}
        if game.first_start is not None:
            r["warnings"].append(f"the game code was built in {game.first_start:.0f} s")
        found = [l.split("] ", 1)[-1] for l in game.new_log_lines() if "] Game: " in l]
        r["warnings"] += found[:1]  # the copy the game found, and how it matched
        if not game.hwnd:
            r["errors"].append(f"message box: {game.dialog}" if game.dialog else "no game window")
        results.append(r)
        failed |= not r["ok"]
    places = {}  # sweep: where each room put Hector and the camera
    screens = {}  # each shot's screen, for later steps' shots["<shot>"]
    judge_model, review = scenario.get("judge"), None
    if judge_model or any("ask" in st for st in scenario["steps"]):
        sys.path.insert(0, str(Path(__file__).resolve().parent))  # judge.py beside this (python -I leaves it off)
        import judge
    if judge_model:
        judge_model = judge.LUNA if judge_model is True else judge_model
        judge_key = judge.key() if judge_model.startswith("jev") else None
        # Jev screens every shot, a Codex model looks again at what it flags
        review = scenario.get("review", judge.LUNA) if judge_model.startswith("jev") else None
    print(f"{name}: {out}")
    try:
        if not game.hwnd:
            if install:
                return False  # the report, from finally, says why
            raise RuntimeError("no game window" + (f": {game.dialog}" if game.dialog else ""))
        steps = scenario["steps"]
        for i, step in enumerate(steps, 1):
            if "sweep" in step:
                rooms = sweep_steps(game, step["sweep"])
                steps[i:i] = rooms  # played next
                n = sum("sweep_room" in s for s in rooms)
                results.append({"step": i, "label": f"sweep: {n} rooms", "ok": bool(rooms),
                                "errors": [] if rooms else ["no rooms.json from the game"], "warnings": []})
                print(f"  {i:3d} sweep of {n} rooms")
                continue
            label = step.get("shot") or step.get("note") or step.get("press") or step.get("until") or step.get("route")
            r = {"step": i, "label": label, "errors": [], "warnings": []}
            frame0 = prev.get("frame") if prev else None
            started = time.time()
            if "until" in step:
                state, seconds, err = poll_until(game, step["until"], step.get("timeout", 60), step.get("poll"),
                                                 step.get("poll_by", []), prev)
            elif "route" in step:
                route = json.loads((ROUTES / f"{step['route']}.json").read_text())
                state, seconds, err, notes = play_route(game, route, step, prev,
                                                        shots / f"{i:03d}_box" if step.get("box_shots") else None)
                r["warnings"] += notes
            else:
                state, seconds, err = game.play(step["press"])
            if err:
                r["errors"].append(err)
            log = game.new_log_lines()
            for line in log:
                if "error: " in line or "guest trap" in line:
                    r["errors"].append("log: " + line.strip())
            if scenario.get("perf"):
                r["perf"] = perf_of(log)
                if step.get("perf_room"):
                    r["perf_room"] = step["perf_room"]
            screen = {}
            if game.alive():
                img = capture(game.hwnd)
                if img:
                    screen = screen_stats(*img)
                    if step.get("measure"):
                        screen["edges"] = {k: edges(*img, box) for k, box in step["measure"].items()}
                    shot = shots / f"{i:03d}_{(step.get('shot') or 'step')}.png"
                    write_png(shot, *img)
                    if judge_model and step.get("judge", True):
                        try:
                            screen["judge"], _ = judge.judged(shot, judge_model, judge_key)
                        except Exception as e:  # the judge is advice: the step goes on without it
                            r["warnings"].append(f"judge: {e}")
                    if step.get("ask"):
                        try:
                            screen["ask"], _ = judge.ask(shot, step["ask"]["question"], step["ask"]["choices"])
                        except Exception as e:  # an expect on the answer fails then, and says why
                            r["warnings"].append(f"ask: {e}")
            if state:
                frames = state.get("frame", 0) - (frame0 if frame0 is not None else state.get("frame", 0))
                hdd = game.folder / "hdd"
                info = {"seconds": round(seconds or 0, 2),
                        "fps": round(frames / seconds, 1) if seconds and frame0 is not None else None,
                        # the game wrote a save during the step
                        "saved": any(f.is_file() and f.stat().st_mtime >= started for f in hdd.rglob("*"))
                        if hdd.exists() else False}
                seen = judge_warnings(screen.get("judge"), state, screen, JEV_SCREEN if review else None)
                if seen and review:
                    try:  # the second opinion is the step's: a warning it does not share goes
                        second, _ = judge.judged(shot, review)
                        screen["judge"] = dict(second, first=screen["judge"])
                        seen = judge_warnings(screen["judge"], state, screen)
                    except Exception as e:  # Jev's word alone, then
                        r["warnings"].append(f"review: {e}")
                        seen = judge_warnings(screen["judge"], state, screen)
                for expr in step.get("expect", []):
                    ok, why = holds(expr, state, prev, screen, info, settings, screens)
                    if not ok:
                        r["errors"].append(f"expect {expr!r} failed" + (f" ({why})" if why else ""))
                r.update(state=state, screen=screen, **info)
                if step.get("shot"):
                    screens[step["shot"]] = screen
                if screen.get("flat"):
                    r["warnings"].append(f"screen is one colour (mean {screen['mean']})")
                r["warnings"] += seen
                # a room ID that lands where another one did: the same room, or an ID the warp falls back from
                p, c = (state.get("player") or {}).get("pos"), (state.get("camera") or {}).get("pos")
                if step.get("sweep_room") and p and c:
                    place = tuple(round(v) for v in p + c)
                    if place in places:
                        r["warnings"].append(f"same place and camera as room {places[place]}")
                    places.setdefault(place, step["sweep_room"])
            if step.get("save_as") and not r["errors"]:
                dest = SAVES / step["save_as"]
                shutil.rmtree(dest, ignore_errors=True)
                shutil.copytree(game.folder / "hdd", dest)
                r["saved"] = step["save_as"]
            r["ok"] = not r["errors"]
            results.append(r)
            asked = (r.get("screen") or {}).get("ask")
            print(f"  {i:3d} {'ok  ' if r['ok'] else 'FAIL'} {label}"
                  + (f"\n        asked: {asked['answer']} ({asked['why']})" if asked else "")
                  + "".join(f"\n        {e}" for e in r["errors"] + r["warnings"]))
            if state:
                prev = state
            if not r["ok"]:
                failed = True
                if not game.alive() or step.get("stop_on_fail", True):
                    break
    finally:
        game.close()
        crash = game.folder / "CrashDumps"
        dumps = [p.name for p in crash.glob("*")] if crash.exists() else []
        if dumps:
            failed = True
            results.append({"step": "end", "ok": False, "errors": ["crash dumps: " + ", ".join(dumps)]})
        report(out, name, results, failed)
        if not keep and not install:
            for f in (game.exe + ".exe", game.exe + ".dll"):
                (game.folder / f).unlink(missing_ok=True)
    print(f"{name}: {'FAILED' if failed else 'passed'} ({sum(r['ok'] for r in results)}/{len(results)} steps)")
    return not failed


def report(out, name, results, failed):
    (out / "results.json").write_text(json.dumps(results, indent=1))
    lines = [f"# {name}: {'FAILED' if failed else 'passed'}", "",
             "| step | result | frame | map/room | Hector (x, y, z) | HP | fps | judge | notes |",
             "|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        s = r.get("state") or {}
        p = s.get("player") or {}
        pos = ", ".join(f"{v:.0f}" for v in p["pos"]) if p.get("pos") else ""
        notes = "; ".join(r.get("errors", []) + r.get("warnings", []))
        j = (r.get("screen") or {}).get("judge")
        seen = f"{j['scene']}, defect {j['defect']:.2f}" if j else ""
        if j and j.get("first"):
            seen += f" ({j['by']}; {j['first']['by']} saw {j['first']['scene']}, defect {j['first']['defect']:.2f})"
        if (r.get("screen") or {}).get("ask"):
            a = r["screen"]["ask"]
            seen += ("; " if seen else "") + f"asked: **{a['answer']}**, {a['why']}"
        lines.append(f"| {r['step']} {r.get('label', '')} | {'ok' if r['ok'] else '**FAIL**'} | {s.get('frame', '')} | "
                     f"{s.get('map', '')}/{s.get('room', '')} | {pos} | {p.get('hp', '')} | {r.get('fps') or ''} | "
                     f"{seen} | {notes.replace('|', '/')} |")
    timed = [r for r in results if r.get("perf") and r.get("perf_room")]
    if timed:  # the rooms stood in, slowest first: where a frame's time goes
        lines += ["", "## Timings", "", "Milliseconds a frame (the SDK's XBE_PERF), the slowest room first. host: the "
                  "renderer's work, on its own thread; recording: the game thread's time in its D3D calls (the game's "
                  "own code is not timed); waiting: the game thread's on the renderer, of which next frame is for the "
                  "frame before to finish drawing.", "",
                  "| room | fps | ms | host | draw | present | recording | waiting | next frame | draws | k verts | "
                  "tex uploads |", "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in sorted(timed, key=lambda r: r["perf"].get("fps", 0)):
            f = r["perf"]
            lines.append("| " + " | ".join([r["perf_room"]] + [str(f.get(k, "")) for k in (
                "fps", "ms", "host", "draw", "present", "recording", "waiting", "wait_next_frame", "draws", "kverts",
                "tex_uploads")]) + " |")
    (out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    args = sys.argv[1:]
    if not args or (args[0].startswith("-") and args[0] not in ("--record", "--make-route")):
        print(__doc__)
        return 2
    build = REPO / "out" / "build" / "release"
    if "--build" in args:
        build = Path(args[args.index("--build") + 1]).resolve()
    if args[0] == "--record":
        save = args[args.index("--save") + 1] if "--save" in args else None
        return 0 if record(args[1], build, save) else 1
    if args[0] == "--make-route":
        return 0 if save_route(args[1], args[2]) else 1
    flag = next((a for a in args if a.startswith("--judge")), None)  # --judge=jev-*: the TypeSafe key from the env
    judge_model = (flag.split("=", 1)[1] if "=" in flag else True) if flag else None
    review = next((a.split("=", 1)[1] for a in args if a.startswith("--review=")), None)
    overrides = {}  # --set button_layout=1: a setting for this run, JSON (a number, true) or else a string
    for i, a in enumerate(args[:-1]):
        if a == "--set":
            k, v = args[i + 1].split("=", 1)
            try:
                overrides[k] = json.loads(v)
            except ValueError:
                overrides[k] = v
    release = args[args.index("--release") + 1] if "--release" in args else None  # --release eu
    return 0 if run(args[0], build, "--keep" in args, judge_model, review, overrides, release=release) else 1


if __name__ == "__main__":
    sys.exit(main())
