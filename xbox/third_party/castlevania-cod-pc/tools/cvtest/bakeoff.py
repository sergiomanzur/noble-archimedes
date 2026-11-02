"""The judge bake-off: a labelled set of screenshots from test runs, some with rendering defects planted in them, for
models to judge (judge.py asks Jev; other models get the same questions), and their answers scored.

usage: python -I tools/cvtest/bakeoff.py pool                 the shots of out/test runs by what their state says,
                                                               and contact sheets of them (out/test/bakeoff/pool)
       python -I tools/cvtest/bakeoff.py build <picks.json>    the set: the picked shots, planted defects, truth.json
       python -I tools/cvtest/bakeoff.py jev [<model>] [--env-file <.env>]   Jev's answers, answers/<model>.json
       python -I tools/cvtest/bakeoff.py codex [<model>]      a Codex model's, one call a picture as cvtest asks
                                                               (gpt-6-luna by default), answers/<model>-cli.json
       python -I tools/cvtest/bakeoff.py prompt                the same questions for a model that opens the pictures
       python -I tools/cvtest/bakeoff.py score                 every answers/*.json against truth.json

The set goes to out/test/bakeoff/set: img-<n>.png in a shuffled order, so a name says nothing of what is in it.
Answers files are {"img-01": {"scene": "<choice>", "defect": <0..1>, "hector": <0..1>}, ...}.
"""
import json
import math
import random
import struct
import sys
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "out" / "test" / "bakeoff"
SCENES = ["gameplay", "pause_menu", "message_box", "title_or_menu", "cutscene", "blank"]


# --- PNG, as cvtest writes it: 8 bit RGB, filter 0 on every row ---------------------------------------------------

def read_png(path):
    data = Path(path).read_bytes()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and colour == 2, f"{path}: not 8 bit RGB"
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    assert all(raw[y * stride] == 0 for y in range(h)), f"{path}: filtered rows"
    rgb = bytearray(b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(h)))
    return w, h, rgb


def write_png(path, w, h, rgb):
    rows = b"".join(b"\0" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def shrink(w, h, rgb, k):
    """every k-th pixel of every k-th row"""
    out = bytearray()
    for y in range(0, h - h % k, k):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        px = bytearray(len(row[0::3 * k]) * 3)
        px[0::3], px[1::3], px[2::3] = row[0::3 * k], row[1::3 * k], row[2::3 * k]
        out += px[:(w // k) * 3]
    return w // k, h // k, out


def sheet(path, images, cols=4, k=3):
    """a contact sheet of (label, png path), k times smaller, labels left to the caller's index"""
    tiles = [shrink(*read_png(p), k) for _, p in images]
    tw, th = tiles[0][0], tiles[0][1]
    rows = (len(tiles) + cols - 1) // cols
    W, H = tw * cols, th * rows
    canvas = bytearray(W * H * 3)
    for n, (w, h, rgb) in enumerate(tiles):
        ox, oy = (n % cols) * tw, (n // cols) * th
        for y in range(min(h, th)):
            canvas[((oy + y) * W + ox) * 3:((oy + y) * W + ox + min(w, tw)) * 3] = rgb[y * w * 3:(y * w + min(w, tw)) * 3]
    write_png(path, W, H, canvas)


# --- the pool ---------------------------------------------------------------------------------------------------

def label(r):
    """what the state says the screen is, or None when it cannot tell"""
    s, screen = r.get("state") or {}, r.get("screen") or {}
    p = s.get("player") or {}
    if screen.get("flat"):
        return "blank"
    if s.get("menu"):
        return "pause_menu"
    if s.get("modal"):
        return "message_box"
    if s.get("cutscene"):
        return "cutscene"
    if (s.get("camera") or {}).get("special") or s.get("loading"):
        return None
    if p.get("active") and p.get("pos"):
        return "gameplay"
    if "title" in str(r.get("label")):
        return "title_or_menu"
    return None


def pool():
    found = []
    for run in sorted(REPO.glob("out/test/*/results.json")):
        shots = run.parent / "shots"
        for r in json.loads(run.read_text()):
            if not isinstance(r.get("step"), int) or not r.get("state"):
                continue
            files = list(shots.glob(f"{r['step']:03d}_*.png"))
            kind = label(r)
            if files and kind:
                s = r["state"]
                found.append({"path": str(files[0].relative_to(REPO)), "kind": kind, "room": f"{s.get('map')}.{s.get('room')}",
                              "label": str(r.get("label"))[:40]})
    dest = OUT / "pool"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "pool.json").write_text(json.dumps(found, indent=1))
    rng = random.Random(7)
    for kind in SCENES:
        items = [f for f in found if f["kind"] == kind]
        # one a room at most, then a few of each other
        seen, picks = set(), []
        for f in rng.sample(items, len(items)):
            key = (f["room"], Path(f["path"]).name.split("_", 1)[1])
            if key not in seen:
                seen.add(key)
                picks.append(f)
        picks = picks[:24]
        (dest / f"{kind}.json").write_text(json.dumps(picks, indent=1))
        for n in range(0, len(picks), 12):
            sheet(dest / f"{kind}-{n // 12 + 1}.png", [(f["label"], REPO / f["path"]) for f in picks[n:n + 12]])
        print(f"{kind}: {len(items)} shots, {len(picks)} on sheets")


# --- planted defects: what a recompiled renderer gets wrong -------------------------------------------------------

def fill_triangle(w, h, rgb, pts, colour):
    pts = sorted(pts, key=lambda p: p[1])
    (x0, y0), (x1, y1), (x2, y2) = pts

    def at(ya, xa, yb, xb, y):
        return xa if yb == ya else xa + (xb - xa) * (y - ya) / (yb - ya)

    for y in range(max(0, int(y0)), min(h, int(y2) + 1)):
        xa = at(y0, x0, y2, x2, y)
        xb = at(y0, x0, y1, x1, y) if y < y1 else at(y1, x1, y2, x2, y)
        lo, hi = max(0, int(min(xa, xb))), min(w, int(max(xa, xb)) + 1)
        if hi > lo:
            rgb[(y * w + lo) * 3:(y * w + hi) * 3] = bytes(colour) * (hi - lo)


def plant(kind, w, h, rgb, rng):
    """the picture with a defect of that kind"""
    if kind == "colours":  # red and blue swapped: a surface format read the wrong way round
        rgb[0::3], rgb[2::3] = rgb[2::3], rgb[0::3]
    elif kind == "flipped":  # upside down: a render target copied with y the wrong way
        rgb[:] = b"".join(rgb[y * w * 3:(y + 1) * w * 3] for y in reversed(range(h)))
    elif kind in ("hole", "white", "garbage"):  # a part not drawn, a texture missing, a texture decoded wrong
        rw, rh = rng.randint(w // 4, w // 2), rng.randint(h // 4, h // 2)
        x0, y0 = rng.randint(0, w - rw), rng.randint(h // 6, h - rh)
        for y in range(y0, y0 + rh):
            if kind == "garbage":
                row = bytearray()
                for bx in range(0, rw, 8):
                    c = (rng.randrange(256), rng.randrange(256), rng.randrange(256))
                    row += bytes(c) * min(8, rw - bx)
                if (y - y0) % 8 == 0:
                    block = row
                rgb[(y * w + x0) * 3:(y * w + x0 + rw) * 3] = block
            else:
                c = (0, 0, 0) if kind == "hole" else (235, 235, 235)
                rgb[(y * w + x0) * 3:(y * w + x0 + rw) * 3] = bytes(c) * rw
    elif kind == "spikes":  # vertices blown out: long slivers of a surface across the screen
        cx, cy = rng.randint(w // 4, 3 * w // 4), rng.randint(h // 3, 3 * h // 4)
        for _ in range(rng.randint(4, 6)):
            i = (cy * w + cx) * 3
            c = tuple(min(255, rgb[i + j] + rng.randint(20, 90)) for j in range(3))
            a = rng.uniform(0, 6.283)
            far = [(cx + 3000 * math.cos(a + d), cy + 3000 * math.sin(a + d))
                   for d in (0, rng.uniform(0.02, 0.08))]
            fill_triangle(w, h, rgb, [(cx, cy)] + far, c)
    return rgb


def build(picks_path):
    """picks: [{"path", "scene", "hector": bool, "plant": "<kind>" or absent}, ...]"""
    picks = json.loads(Path(picks_path).read_text())
    dest = OUT / "set"
    if dest.exists():
        for f in dest.glob("*"):
            f.unlink()
    dest.mkdir(parents=True, exist_ok=True)
    rng = random.Random(29)
    order = rng.sample(range(len(picks)), len(picks))
    truth = {}
    for n, pick in zip(order, picks):
        name = f"img-{n + 1:02d}"
        w, h, rgb = read_png(REPO / pick["path"])
        if pick.get("plant"):
            plant(pick["plant"], w, h, rgb, rng)
        write_png(dest / f"{name}.png", w, h, rgb)
        # a defect can cover Hector: whether he shows is scored on the untouched pictures only
        truth[name] = {"scene": pick["scene"], "defect": bool(pick.get("plant")), "kind": pick.get("plant"),
                       "hector": None if pick.get("plant") else pick["hector"], "source": pick["path"]}
    truth = dict(sorted(truth.items()))
    (OUT / "truth.json").write_text(json.dumps(truth, indent=1))
    names = list(truth)
    for n in range(0, len(names), 12):
        sheet(OUT / f"set-{n // 12 + 1}.png", [(k, dest / f"{k}.png") for k in names[n:n + 12]])
    print(f"{len(truth)} pictures in {dest}, {sum(t['defect'] for t in truth.values())} with defects")


# --- judges and scores --------------------------------------------------------------------------------------------

sys.path.insert(0, str(Path(__file__).resolve().parent))  # judge.py beside this (python -I leaves it off the path)
import judge  # noqa: E402


def run_jev(model, env_file):
    key = judge.key(env_file)
    answers = {}
    for png in sorted((OUT / "set").glob("img-*.png")):
        a, info = judge.jev(png, key, model)
        answers[png.stem] = {**a, **info}
        print(png.stem, a["scene"], a["defect"], a["hector"], f"{info['seconds']}s")
    dest = OUT / "answers"
    dest.mkdir(exist_ok=True)
    (dest / f"{model}.json").write_text(json.dumps(answers, indent=1))


def run_codex(model):
    pngs = sorted((OUT / "set").glob("img-*.png"))
    with ThreadPoolExecutor(4) as workers:  # a call is mostly waiting on the model
        done = list(workers.map(lambda png: judge.codex([png], model), pngs))
    answers = {}
    for png, ((a,), info) in zip(pngs, done):
        answers[png.stem] = {**a, **info}
        print(png.stem, a["scene"], a["defect"], a["hector"], f"{info['seconds']}s", a["what"])
    dest = OUT / "answers"
    dest.mkdir(exist_ok=True)
    (dest / f"{model}-cli.json").write_text(json.dumps(answers, indent=1))


def prompt():
    """the same questions, for a model that reads the pictures itself"""
    scenes = "\n".join(f"   - {k}: {v}" for k, v in judge.SCENES.items())
    return f"""Judge each screenshot of the game Castlevania: Curse of Darkness in {OUT / 'set'} (img-01.png to
img-{len(list((OUT / 'set').glob('img-*.png'))):02d}.png). Open only those pictures: nothing else in out/test/bakeoff, the
repository or anywhere else, and do not run the game. For each picture answer three questions:
1. scene: what the game is showing, one of
{scenes}
2. defect: the probability (0 to 1) that {judge.DEFECT}.
3. hector: the probability (0 to 1) that {judge.HECTOR}.
Look at every picture yourself; do not guess from names or order. Write the answers as JSON to <ANSWERS>:
{{"img-01": {{"scene": "gameplay", "defect": 0.1, "hector": 0.9}}, ...}} with all pictures, and nothing else in that file."""


def auc(pos, neg):
    """the chance a defect scores above a clean picture"""
    if not pos or not neg:
        return None
    return sum((p > n) + 0.5 * (p == n) for p in pos for n in neg) / (len(pos) * len(neg))


def score():
    truth = json.loads((OUT / "truth.json").read_text())
    rows = []
    for f in sorted((OUT / "answers").glob("*.json")):
        ans = json.loads(f.read_text(encoding="utf-8-sig"))  # PowerShell writes a BOM
        missing = [k for k in truth if k not in ans]
        scene = sum(ans.get(k, {}).get("scene") == t["scene"] for k, t in truth.items())
        d = {k: float(ans[k].get("defect", 0)) for k in truth if k in ans}
        pos = [d[k] for k, t in truth.items() if t["defect"] and k in d]
        neg = [d[k] for k, t in truth.items() if not t["defect"] and k in d]
        hit = sum(v >= 0.5 for v in pos)
        false = sum(v >= 0.5 for v in neg)
        hk = [(float(ans[k].get("hector", 0)) >= 0.5) == t["hector"] for k, t in truth.items()
              if t["hector"] is not None and k in ans]
        missed = sorted({truth[k]["kind"] for k, t in truth.items() if t["defect"] and d.get(k, 0) < 0.5})
        wrong_scene = [f"{k} {t['scene']}->{ans.get(k, {}).get('scene')}" for k, t in truth.items()
                       if ans.get(k, {}).get("scene") != t["scene"]]
        secs = [a["seconds"] for a in ans.values() if isinstance(a, dict) and "seconds" in a]
        # the cut that gets the most pictures right: how good the ranking is once its scale is known
        cut, right = max(((c, sum(v >= c for v in pos) + sum(v < c for v in neg)) for c in sorted(set(pos + neg))),
                         key=lambda t: t[1], default=(0.5, 0))
        rows.append((f.stem, f"{scene}/{len(truth)}", f"{auc(pos, neg):.2f}" if pos else "-",
                     f"{hit}/{len(pos)}", f"{false}/{len(neg)}", f"{right}/{len(pos) + len(neg)} at {cut:g}",
                     f"{sum(hk)}/{len(hk)}",
                     f"{sum(secs) / len(secs):.2f}" if secs else "-", ", ".join(missed) or "-", missing,
                     wrong_scene))
    print("| judge | scene right | defect AUC | defects found (>= 0.5) | clean flagged | best cut | Hector right |"
          " s a picture | defect kinds missed |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        print("| " + " | ".join(r[:9]) + " |")
    for r in rows:
        if r[9]:
            print(f"{r[0]}: no answer for {', '.join(r[9])}")
        print(f"{r[0]} scenes wrong: {'; '.join(r[10]) or 'none'}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "pool":
        pool()
    elif cmd == "build":
        build(sys.argv[2])
    elif cmd == "jev":
        run_jev(sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else "jev-latest",
                sys.argv[sys.argv.index("--env-file") + 1] if "--env-file" in sys.argv else None)
    elif cmd == "codex":
        run_codex(sys.argv[2] if len(sys.argv) > 2 else judge.LUNA)
    elif cmd == "prompt":
        print(prompt())
    elif cmd == "score":
        score()
    else:
        print(__doc__)
