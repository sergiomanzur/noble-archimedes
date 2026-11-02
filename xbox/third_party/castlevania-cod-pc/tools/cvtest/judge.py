"""The judge: asks a model what a screenshot shows, whether it rendered right and whether Hector is in it. Jev
(TypeSafe System One, https://docs.typesafe.ai) answers with probabilities rather than text, in well under a second;
a model of the Codex CLI (codex exec, on its own sign-in) takes some 4 s and is the better judge
(tools/cvtest/bakeoff.py): cvtest's own, and the one that looks again at what Jev flags when Jev screens.

usage: python -I tools/cvtest/judge.py <shot.png> [...] [--model gpt-6-luna|jev-latest] [--env-file <.env>]

Jev's key is TYPESAFE_API_KEY, from the environment or a .env file (--env-file, or TYPESAFE_ENV_FILE). Answers are
{"scene": "<one of SCENES>", "scene_p": {...}, "defect": <0..1>, "hector": <0..1>}; a Codex model's have no
scene_p and add "what", the defect in a few words, and "colours", those it saw of the HUD and Hector's scarf.
"""
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

API = "https://api.typesafe.ai/v1/systemone"
LUNA = "gpt-6-luna"  # the Codex model that judged best (bakeoff.py)

SCENES = {
    "gameplay": "the player controls Hector in a 3D level, the HUD (HP bar) at the top left",
    "pause_menu": "the pause or status menu",
    "message_box": "a text box or dialog over the gameplay",
    "map_screen": "the castle map",
    "title_or_menu": "the title screen or a front-end menu (name entry, save and load)",
    "cutscene": "a story scene or a pre-rendered movie",
    "blank": "a black or one-colour screen, a fade, loading",
}
DEFECT = ("the picture has a rendering defect: garbage or exploded polygons (long slivers across the screen), "
          "corrupted textures (blocky noise), missing textures (flat white or solid areas), wrong colours (red and "
          "blue swapped, say: in play the icon beside the HP bar at the top left is red, the HP bar yellow and "
          "Hector's scarf red, so blue ones there are swapped colours), black or unrendered regions, or an upside-down "
          "image. Effects, motion blur, light rays, sparks, smoke and dark scenes are not defects")
HECTOR = "Hector, the white-haired man in a long dark coat with a red scarf, is visible"

# the same three questions, as Jev takes them; the picture is `screen` in the state
QUESTIONS = {
    "scene": {"type": "choice", "instructions": "What is the game showing in `screen`?", "criteria": SCENES},
    "defect": {"type": "noul", "instructions": f"In `screen`, {DEFECT}."},
    "hector": {"type": "noul", "instructions": f"In `screen`, {HECTOR}."},
}


def key(env_file=None):
    if os.environ.get("TYPESAFE_API_KEY"):
        return os.environ["TYPESAFE_API_KEY"]
    path = env_file or os.environ.get("TYPESAFE_ENV_FILE")
    if path and Path(path).exists():
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            if line.startswith("TYPESAFE_API_KEY="):
                return line.split("=", 1)[1].strip().strip('"')
    raise SystemExit("no TypeSafe key: set TYPESAFE_API_KEY, or give --env-file / TYPESAFE_ENV_FILE")


def jev(png, api_key, model="jev-latest", state=None, tries=4):
    """one screenshot judged: the answers and {"seconds", "tokens", "model"}"""
    ref = "img-3c71e0a9b4d2"  # replaced as plain text wherever it appears: a name nothing else will contain
    body = {"model": model, "state": dict(state or {}, screen=ref), "questions": QUESTIONS,
            "assets": {ref: {"type": "image", "format": "png", "data_base64": base64.b64encode(Path(png).read_bytes()).decode()}}}
    data = json.dumps(body).encode()
    for attempt in range(tries):
        req = urllib.request.Request(API, data=data, method="POST", headers={
            "Authorization": f"Bearer {api_key}", "Content-Type": "application/json",
            "TypeSafe-Danger-Zone-Images": "true"})
        start = time.time()
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                res = json.load(r)
            break
        except urllib.error.HTTPError as e:
            if e.code in (429, 500, 502, 503, 529) and attempt + 1 < tries:
                time.sleep(2 ** attempt)
                continue
            raise RuntimeError(f"Jev: HTTP {e.code}: {e.read()[:300]!r}") from None
    a = res["answers"]
    answers = {"scene": a["scene"]["choice"], "scene_p": {k: round(v, 3) for k, v in a["scene"]["probabilities"].items()},
               "defect": round(a["defect"]["noul"], 3), "hector": round(a["hector"]["noul"], 3)}
    return answers, {"seconds": round(time.time() - start, 2), "tokens": res.get("usage", {}).get("input_tokens"),
                     "model": res.get("model")}


def codex_exec(pngs, prompt, schema, model=LUNA, effort="medium", timeout=600):
    """one codex exec call with the pictures attached and an answer in the shape of schema: the answer, and
    {"seconds", "tokens", "model"}. The user's Codex config is left out (its plugins and hooks), its sign-in is not;
    the model runs in a read-only sandbox in an empty folder and is told to run nothing."""
    exe = shutil.which("codex")
    if not exe:
        raise RuntimeError("no codex on PATH (the Codex CLI)")
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "schema.json").write_text(json.dumps(schema))
        last = Path(tmp) / "last.json"
        cmd = [exe, "exec", "-m", model, "-c", f'model_reasoning_effort="{effort}"', "--ignore-user-config",
               "--ephemeral", "--skip-git-repo-check", "-s", "read-only", "-C", tmp,
               "--output-schema", str(Path(tmp) / "schema.json"), "-o", str(last), "--color", "never"]
        for p in pngs:
            cmd += ["-i", str(Path(p).resolve())]
        start = time.time()
        r = subprocess.run(cmd + ["-"], input=prompt, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=timeout)
        if r.returncode or not last.exists():
            raise RuntimeError(f"codex: exit {r.returncode}: {(r.stderr or r.stdout).strip()[-300:]!r}")
        answer = json.loads(last.read_text(encoding="utf-8"))
    used = re.search(r"tokens used\s+([\d.,]+)", r.stdout + r.stderr)  # "16.090" or "16,090", by the locale
    return answer, {"seconds": round(time.time() - start, 2), "model": model,
                    "tokens": int(re.sub(r"\D", "", used.group(1))) if used else None}


def ask(png, question, choices, model=LUNA):
    """one screenshot and a question about it, with choices {name: what it means}: {"answer": <name>, "why": ...}
    from a Codex model, and the call's info"""
    listed = "; ".join(f"{k}: {v}" for k, v in choices.items())
    prompt = (f"A screenshot of the game Castlevania: Curse of Darkness is attached. {question} Answer with one of "
              f"these: {listed}. Say in a sentence what in the picture decides it. Look only at the attached picture: "
              f"run no commands and open no files.")
    schema = {"type": "object", "additionalProperties": False, "required": ["answer", "why"],
              "properties": {"answer": {"type": "string", "enum": list(choices)}, "why": {"type": "string"}}}
    answer, info = codex_exec([png], prompt, schema, model)
    return dict(answer, by=model), info


def codex(pngs, model=LUNA, effort="medium", timeout=600):
    """screenshots judged by a Codex model in one codex exec call (codex_exec): the answers of each, in order, and
    the call's info"""
    scenes = "; ".join(f"{k}: {v}" for k, v in SCENES.items())
    prompt = (f"{len(pngs)} screenshot(s) of the game Castlevania: Curse of Darkness are attached, numbered from 1 in "
              f"the order attached. For each one answer: shot, its number; scene, what the game is showing, one of "
              f"{scenes}; icon, bar and scarf, the colour you see of the icon beside the HP bar at the top left, of "
              f"the filled part of that HP bar and of Hector's scarf (blue for cyan too; none when it is not in the "
              f"picture); defect, the probability (0 to 1) that {DEFECT}; what, that defect in a few words (empty when "
              f"there is none); hector, the probability (0 to 1) that {HECTOR}. Look only at the attached pictures: "
              f"run no commands and open no files.")
    colour = {"type": "string", "enum": ["red", "yellow", "blue", "other", "none"]}
    keys = ["shot", "scene", "icon", "bar", "scarf", "defect", "what", "hector"]
    shot = {"type": "object", "additionalProperties": False, "required": keys,
            "properties": {"shot": {"type": "integer"}, "scene": {"type": "string", "enum": list(SCENES)},
                           "icon": colour, "bar": colour, "scarf": colour, "defect": {"type": "number"},
                           "what": {"type": "string"}, "hector": {"type": "number"}}}
    schema = {"type": "object", "additionalProperties": False, "required": ["shots"],
              "properties": {"shots": {"type": "array", "items": shot}}}
    answer, info = codex_exec(pngs, prompt, schema, model, effort, timeout)
    got = {a.pop("shot"): a for a in answer["shots"]}
    if sorted(got) != list(range(1, len(pngs) + 1)):
        raise RuntimeError(f"codex: answers for shots {sorted(got)} of {len(pngs)}")
    answers = []
    for _, a in sorted(got.items()):
        j = {"scene": a["scene"], "defect": round(a["defect"], 3), "what": a["what"], "hector": round(a["hector"], 3),
             "colours": {k: a[k] for k in ("icon", "bar", "scarf")}}
        # the model reports a blue HP icon and bar and still calls the picture clean (bakeoff.py: 0 of 3 swaps found,
        # at high effort too), so the swap is told from what it saw
        if a["icon"] == "blue" and a["bar"] == "blue":
            j["defect"] = max(j["defect"], 0.95)
            j["what"] = j["what"] or "red and blue swapped (a blue HP icon and bar)"
        answers.append(j)
    return answers, info


def judged(png, model, api_key=None):
    """one screenshot judged by Jev or a Codex model, with "by", the model"""
    if model.startswith("jev"):
        answers, info = jev(png, api_key, model)
    else:
        (answers,), info = codex([png], model)
    return dict(answers, by=model), info


def main():
    args = sys.argv[1:]
    model = args[args.index("--model") + 1] if "--model" in args else LUNA
    env_file = args[args.index("--env-file") + 1] if "--env-file" in args else None
    shots = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--model", "--env-file"))]
    if not shots:
        print(__doc__)
        return 2
    k = key(env_file) if model.startswith("jev") else None
    for shot in shots:
        answers, info = judged(shot, model, k)
        print(json.dumps({"shot": shot, **answers, **info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
