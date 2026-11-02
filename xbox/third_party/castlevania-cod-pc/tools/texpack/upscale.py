"""HD pictures for the textures an HD pack has none for, from the game's own: the pictures CV_TEXPACK_MISSES saves,
each upscaled 4x (the pack's scale) by an AI model. Where the pack has the PS2's picture of one in the same colours
and the Xbox changed part of it (HARD DISK for MEMORY CARD), the result is the pack's picture with that part alone
taken from the upscale, so the pack's redrawn art stays.

usage: python -I tools/texpack/upscale.py make --misses <folder> --pack <textures> --out <folder> <model>
       python -I tools/texpack/upscale.py calibrate --misses <folder> --pack <textures> --out <folder> <model>...

make writes a picture for each miss, named as the pack would name it; put the folder inside `textures` (or list it
first in CV_TEXTURES to try it out). calibrate upscales Xbox textures the pack has the PS2's pictures of (kAliases'
and the changed ones') with each model, and measures each against the pack's (PSNR, and detail against the pack's),
with a sheet of crops: RealESRGAN_x4plus came out closest to the Panda Venom & Stan21 pack, its pictures likely
made with it, so pictures made with it look like the pack's.

Needs Python with PyTorch (CUDA for an NVIDIA GPU, else it runs on the CPU, slower; CV_UPSCALE_CPU=1 for the CPU
anyway, as while a game has the GPU: sharing its memory with one slows both to a crawl), spandrel, numpy and Pillow,
and a model file:
  python -m venv venv
  venv\\Scripts\\python -m pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128
  venv\\Scripts\\python -m pip install --no-deps spandrel
  venv\\Scripts\\python -m pip install numpy pillow safetensors einops
  RealESRGAN_x4plus.pth: https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth
(spandrel installs with no dependencies, or it would put a CPU PyTorch in place of the CUDA one.) The pictures are
made from the game's textures, so like a pack they are the game's art: keep them out of this repository.

Pictures are RGBA with the PS2's alpha (128 opaque), as PCSX2 dumps and the game reads them."""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 4  # the pack's scale

# Xbox textures found in a pack under the PS2's names (texpack.cpp kAliases): the game uses the pack's, so none made
ALIASED = {"0cf3381b4682dc75", "146b17b78291a0da", "2df803e5a08f824a", "43cd7f2402145e02", "4cbd1e391f78ce11",
           "6c619b9c19ccc7ac", "c57de7c4b83fd34a", "e0274fd1ae6986b8"}

# misses whose pack picture of the PS2 texture has the same colours, the PS2's picture for the calibration (the used
# part of the Xbox texture against the same part of it)
CALIBRATION = {
    "0cf3381b4682dc75-b014ec40749e17ab": "b6b4a1f431c15313-b014ec40749e17ab",  # a misty backdrop
    "146b17b78291a0da-46361247dbb7b045": "51240b26560f7efd-46361247dbb7b045",  # the Konami logo
    "2df803e5a08f824a-887cfe00634f8c92": "19a172cc2f3576b3-887cfe00634f8c92",  # an arch and carved panels
    "43cd7f2402145e02-d63eec7aa2ab2be3": "ff56bb0d4b8bb6d3-d63eec7aa2ab2be3",  # a cloudy sky
    "4cbd1e391f78ce11-3b3c2ba9916b03e1": "10a949fa295fb1fb-3b3c2ba9916b03e1",  # a floor of carved scrolls
    "c57de7c4b83fd34a-47911e17dd5e98b8": "90bbc4ca79461f9d-47911e17dd5e98b8",  # a mossy floor
    "b1144321f94e3f01-216b6ba910fe717a": "7ed467d1c6c85373-216b6ba910fe717a",  # the save screens' frame
    "b0aa12cade97d07f-bfb304e35ba2c133": "ebd4331679382106-bfb304e35ba2c133",  # the HUD's icons
}

# misses the Xbox changed from a PS2 texture the pack has in the same colours: the textures, the PS2's
CHANGED = {"b0aa12cade97d07f": "ebd4331679382106",  # the HUD's and menus' icons: NEW! labels, the d-pad
           "b1144321f94e3f01": "7ed467d1c6c85373"}  # the save screens' frame: HARD DISK for MEMORY CARD


def key(path):
    t, p = os.path.basename(path).split("-")[:2]
    return f"{int(t, 16):016x}-{int(p, 16):016x}"


def pack_index(pack):
    return {key(f): f for f in glob.glob(os.path.join(pack, "**", "*.*"), recursive=True)
            if os.path.splitext(f)[1].lower() in (".dds", ".png") and os.path.basename(f).count("-") == 2}


def load(path):
    """RGBA, 0..1, the PS2's alpha doubled as the game does"""
    a = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)
    a[..., 3] = np.minimum(255, (a[..., 3] * 255 + 64) // 128)
    return a / 255


def save(path, a):
    px = np.clip(np.rint(a * 255), 0, 255).astype(np.uint8)
    px[..., 3] = np.rint(px[..., 3].astype(np.float32) * 128 / 255).astype(np.uint8)
    Image.fromarray(px, "RGBA").save(path)


def wraps(a, axis):
    """whether the texture repeats along the axis: its last row (column) runs into its first as smoothly as within it"""
    p = np.concatenate([a[..., :3] * a[..., 3:], a[..., 3:]], axis=2)
    if axis == 1:
        seam, inner = np.abs(p[:, 0] - p[:, -1]).mean(), np.abs(p[:, 1:] - p[:, :-1]).mean()
    else:
        seam, inner = np.abs(p[0] - p[-1]).mean(), np.abs(p[1:] - p[:-1]).mean()
    return seam <= 1.5 * inner + 0.01


def resize(a, w, h, how=Image.LANCZOS):
    return np.stack([np.asarray(Image.fromarray(a[..., c]).resize((w, h), how)) for c in range(a.shape[2])], axis=2)


class Model:
    def __init__(self, path, tile=192):
        import torch
        from spandrel import ImageModelDescriptor, ModelLoader
        self.torch = torch
        m = ModelLoader().load_from_file(path)
        assert isinstance(m, ImageModelDescriptor) and m.scale == S, (path, m.scale)
        self.dev = "cuda" if torch.cuda.is_available() and not os.environ.get("CV_UPSCALE_CPU") else "cpu"
        self.m = m.to(self.dev).eval()
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.tile = tile

    def rgb(self, a):
        """HxWx3 0..1 to (4H)x(4W)x3, in tiles with a margin each, cut away after"""
        torch = self.torch
        x = torch.from_numpy(np.ascontiguousarray(a.transpose(2, 0, 1)))[None]
        _, _, H, W = x.shape
        out = torch.zeros(1, 3, H * S, W * S)
        T, O = self.tile, 16
        with torch.no_grad():
            for y in range(0, H, T):
                for xx in range(0, W, T):
                    y0, x0, y1, x1 = max(y - O, 0), max(xx - O, 0), min(y + T + O, H), min(xx + T + O, W)
                    t = self.m(x[:, :, y0:y1, x0:x1].to(self.dev)).float().cpu()
                    hh, ww = (min(y + T, H) - y) * S, (min(xx + T, W) - xx) * S
                    ty, tx = (y - y0) * S, (xx - x0) * S
                    out[:, :, y * S:y * S + hh, xx * S:xx * S + ww] = t[:, :, ty:ty + hh, tx:tx + ww]
        return out[0].numpy().transpose(1, 2, 0).clip(0, 1)


def upscale(model, a):
    """RGBA HxW to 4x. Repeating textures are padded with their other side, others with their own edge mirrored, so
    their edges come out as they would inside.

    Alpha that is mostly on or off (cut out shapes, text) goes through the model too, for sharp edges; softer alpha is
    resized smoothly. The colour of either goes through the model weighted by alpha: the colour under transparent
    texels (often a key colour, a green) would otherwise run into the edges; those texels keep their own colour, so
    drawn without alpha they look the same. Mostly soft alpha (glows, smoke) is resized smoothly, and their colour is
    half the model's, half resized: the model gives soft shapes a texture (fur, grass) they do not have."""
    h, w = a.shape[:2]
    pad = 16
    p = a
    for axis in (0, 1):
        widths = [(0, 0)] * 3
        widths[axis] = (pad, pad)
        p = np.pad(p, widths, mode="wrap" if wraps(a, axis) else "symmetric")
    big = (p.shape[1] * S, p.shape[0] * S)
    smooth = lambda x: resize(x, *big)
    alpha = p[..., 3]
    vis = (a[..., 3] > 0.03).mean()
    soft = ((a[..., 3] > 0.03) & (a[..., 3] < 0.97)).mean() / max(1e-6, vis)
    if not model:
        rgb, al = smooth(p[..., :3]), smooth(alpha[..., None])[..., 0]
    elif alpha.min() >= 0.995:
        rgb, al = model.rgb(p[..., :3]), np.ones(big[::-1], np.float32)
    elif soft < 0.6:
        if soft < 0.25:
            al = np.clip(model.rgb(np.repeat(alpha[..., None], 3, axis=2)).mean(axis=2), 0, 1)
        else:
            al = np.clip(smooth(alpha[..., None])[..., 0], 0, 1)
        pre = model.rgb(p[..., :3] * alpha[..., None])
        hidden = np.asarray(Image.fromarray((p[..., :3] * 255).astype(np.uint8)).resize(big, Image.NEAREST),
                            np.float32) / 255
        rgb = np.where(al[..., None] > 0.02, pre / np.maximum(al, 0.15)[..., None], hidden)
    else:
        al = smooth(alpha[..., None])[..., 0]
        rgb = (model.rgb(p[..., :3]) + smooth(p[..., :3])) / 2
    out = np.concatenate([np.clip(rgb, 0, 1), np.clip(al, 0, 1)[..., None]], axis=2)
    return out[pad * S:(pad + h) * S, pad * S:(pad + w) * S]


def changed_mask(low, ps2_low, thr=0.2):
    """where the Xbox texture differs from the pack's picture of the PS2's, shrunk to its size: where it differs
    throughout (new text or a new label), not along edges only, as it does wherever the pack's picture is redrawn"""
    pm = lambda a: np.concatenate([a[..., :3] * a[..., 3:], a[..., 3:]], axis=2)
    d = np.pad(np.abs(pm(low) - pm(ps2_low)).max(axis=2), 2, mode="edge")
    dense = sum(d[y:y + low.shape[0], x:x + low.shape[1]] for y in range(5) for x in range(5)) / 25 > thr
    return np.asarray(Image.fromarray((dense * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(7))) > 0


def consensus(pairs):
    """the part the Xbox changed, the same in every colouring of a texture (its indices changed): where most of them
    differ from the pack's (a colouring the game uses for a few icons only may differ elsewhere too, where the pack's
    picture in it was not redrawn as carefully, it being never seen)"""
    masks = []
    for low, ps2 in pairs:
        h, w = low.shape[:2]
        masks.append(changed_mask(low, resize(ps2, w, h, Image.BOX)))
    return np.mean(masks, axis=0) >= 0.5


def bleed(rgb, known, n):
    """the colours of the known pixels carried out n pixels into the others, each the mean of its known neighbours"""
    rgb, known = rgb.copy(), known.copy()
    for _ in range(n):
        acc, cnt = np.zeros_like(rgb), np.zeros(known.shape, np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dy or dx:
                    k = np.roll(known, (dy, dx), (0, 1))
                    acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
                    cnt += k
        new = ~known & (cnt > 0)
        rgb[new] = acc[new] / cnt[new][:, None]
        known |= new
    return rgb


def foreign(hd, low, thr=0.25):
    """the 4x picture's pixels whose colour is no mix of two of the colours seen (not transparent) in the 3x3 texels
    around theirs in the texture, nor one of them"""
    h, w = low.shape[:2]
    lp = np.pad(low, ((1, 1), (1, 1), (0, 0)), mode="edge")
    near = np.stack([lp[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)], axis=2)  # h, w, 9, RGBA
    cols, seen = near[..., :3], near[..., 3] >= 0.05
    c = hd[..., :3].reshape(h, S, w, S, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, S * S, 3)  # each texel's pixels
    d = np.abs(c[:, :, :, None] - cols[:, :, None]).max(-1)  # h, w, 16, 9: from each colour
    d[~np.broadcast_to(seen[:, :, None], d.shape)] = np.inf
    best = d.min(-1)
    # mixes of two, for the pixels no one colour is near (few: it is slow)
    yy, xx, kk = np.nonzero(best > thr)
    if len(yy):
        cc, nc, nv = c[yy, xx, kk], cols[yy, xx], seen[yy, xx]
        two = np.full(len(yy), np.inf, np.float32)
        for i in range(9):
            for j in range(i + 1, 9):
                a, ab = nc[:, i], nc[:, j] - nc[:, i]
                t = np.clip(((cc - a) * ab).sum(-1) / np.maximum((ab * ab).sum(-1), 1e-6), 0, 1)
                e = np.abs(cc - a - t[:, None] * ab).max(-1)
                e[~(nv[:, i] & nv[:, j])] = np.inf
                np.minimum(two, e, out=two)
        best[yy, xx, kk] = np.minimum(best[yy, xx, kk], two)
    return (best > thr).reshape(h, w, S, S).transpose(0, 2, 1, 3).reshape(h * S, w * S)


def stray(hd, own):
    """the 4x picture's pixels of a strong colour over a texel that is not of it (a grey, or another hue): in a sheet
    of icons an upscale runs each icon into its neighbours, which in another colouring may be a strong colour drawn
    nowhere (pure green letters, in the colouring the HUD draws the line under them in). own: the texture at 4x."""

    def hue_chroma(x):
        r, g, b = x[..., 0], x[..., 1], x[..., 2]
        mx, mn = x[..., :3].max(-1), x[..., :3].min(-1)
        c = mx - mn
        h = np.where(mx == r, (g - b) / np.maximum(c, 1e-6) % 6,
                     np.where(mx == g, (b - r) / np.maximum(c, 1e-6) + 2, (r - g) / np.maximum(c, 1e-6) + 4))
        return h * 60, c

    h1, c1 = hue_chroma(hd)
    h2, c2 = hue_chroma(own)
    dh = np.abs((h1 - h2 + 180) % 360 - 180)
    return (hd[..., 3] > 0.05) & (own[..., 3] > 0.05) & (c1 > 0.25) & ((c2 < 0.12) | (dh > 60))


def composite(model, low, ps2_hd, m):
    h, w = low.shape[:2]
    im = Image.fromarray((m * 255).astype(np.uint8))
    frac = m.mean()
    big = im.resize((w * S, h * S), Image.NEAREST).filter(ImageFilter.GaussianBlur(S))
    mk = np.asarray(big, dtype=np.float32)[..., None] / 255
    up = upscale(model, low)
    # the pack's upscale let the key colour under the PS2's transparent texels (a green) into the edges by them, a
    # green fringe on the HUD: where its colour is no mix of the colours seen around it in the Xbox's texture and
    # the upscale's (made without the key) is, the upscale's, its colours carried a few pixels out for the pack's
    # edges reaching past its own
    odd = foreign(ps2_hd, low)
    ok = (up[..., 3] > 0.1) & ~foreign(up, low)
    fringe = (ps2_hd[..., 3] > 0.05) & odd & ~foreign(clean := bleed(up[..., :3], ok, 8), low)
    ps2_hd = ps2_hd.copy()
    ps2_hd[fringe, :3] = clean[fringe]
    # mixed weighted by alpha, or where one is opaque and the other transparent the colour under the transparent one
    # (a key colour) would tint the mix
    pa, ua = ps2_hd[..., 3:] * (1 - mk), up[..., 3:] * mk
    al = pa + ua
    mixed = ps2_hd[..., :3] * (1 - mk) + up[..., :3] * mk
    rgb = np.where(al > 1e-3, (ps2_hd[..., :3] * pa + up[..., :3] * ua) / np.maximum(al, 1e-3), mixed)
    out = np.concatenate([rgb, al], axis=2)
    # those pixels from the texture itself, resized smoothly, or where that is of a stray colour too (by a texel of
    # another one) as it is
    own = np.repeat(np.repeat(low, S, axis=0), S, axis=1)
    smooth = resize(low, w * S, h * S, Image.BILINEAR)
    bad = stray(out, own)
    out[bad, :3] = np.where(stray(smooth, own)[..., None], own[..., :3], smooth[..., :3])[bad]
    return out, frac, (fringe | bad).mean()


def used(a):
    """the part before a right and bottom margin of one colour (the bottom right texel's)"""
    d = np.abs(a - a[-1, -1]).max(axis=2)
    cols, rows = np.nonzero(d.max(axis=0) > 0.025)[0], np.nonzero(d.max(axis=1) > 0.025)[0]
    if len(cols) == 0 or len(rows) == 0:
        return a.shape[1], a.shape[0]
    fx, fy = cols.max() + 1, rows.max() + 1
    return (fx, fy) if fx < a.shape[1] * 0.97 or fy < a.shape[0] * 0.97 else (a.shape[1], a.shape[0])


def detail(a):
    g = a[..., :3].mean(axis=2)
    return np.abs(g[1:-1, 1:-1] * 4 - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:]).mean()


def psnr(x, y):
    pm = lambda a: a[..., :3] * a[..., 3:]
    e = ((pm(x) - pm(y)) ** 2).mean()
    return 10 * np.log10(1 / max(e, 1e-10))


def calibrate(args):
    pack = pack_index(args.pack)
    misses = {key(f): f for f in glob.glob(os.path.join(args.misses, "*.png"))}
    models = [None] + [Model(p) for p in args.models]
    names = ["lanczos"] + [m.name for m in models[1:]]
    rows = []
    print(f"{'texture':22}" + "".join(f"{n[:22]:>24}" for n in names) + "   (PSNR dB against the pack / detail vs pack's)")
    totals = np.zeros((len(models), 2))
    for xk, pk in CALIBRATION.items():
        low = load(misses[xk])
        ref = load(pack[pk])
        uw, uh = used(low)
        ref = ref[:uh * S, :uw * S]
        outs = [upscale(m, low)[:uh * S, :uw * S] for m in models]
        scores = [(psnr(o, ref), detail(o) / detail(ref)) for o in outs]
        totals += np.array(scores)
        print(f"{xk[:16]:22}" + "".join(f"{p:17.2f} {d:5.2f}" for p, d in scores))
        rows.append((xk, outs, ref))
    totals /= len(CALIBRATION)
    print(f"{'mean':22}" + "".join(f"{p:17.2f} {d:5.2f}" for p, d in totals))
    # a sheet: a crop of each, the pack's last
    c = 192
    sheet = Image.new("RGB", ((len(models) + 1) * (c + 4) + 4, len(rows) * (c + 18) + 4), (16, 18, 26))
    dr = ImageDraw.Draw(sheet)
    for i, (xk, outs, ref) in enumerate(rows):
        h, w = ref.shape[:2]
        # the crop with the most detail in the pack's
        best, at = -1, (0, 0)
        for y in range(0, max(1, h - c), c // 2):
            for x in range(0, max(1, w - c), c // 2):
                d = detail(ref[y:y + c, x:x + c])
                if d > best:
                    best, at = d, (y, x)
        y, x = at
        for j, a in enumerate(outs + [ref]):
            t = a[y:y + c, x:x + c]
            t = np.clip(t[..., :3] * t[..., 3:] * 255, 0, 255).astype(np.uint8)
            sheet.paste(Image.fromarray(t), (4 + j * (c + 4), 4 + i * (c + 18) + 14))
            if i == 0:
                dr.text((4 + j * (c + 4), 2), (names + ["pack"])[j][:30], fill=(230, 200, 120))
    os.makedirs(args.out, exist_ok=True)
    sheet.save(os.path.join(args.out, "calibration.png"))


def make(args):
    pack = pack_index(args.pack)
    model = Model(args.models[0])
    os.makedirs(args.out, exist_ok=True)
    files = sorted(glob.glob(os.path.join(args.misses, "*.png")))
    masks = {}
    for t, ps2_t in CHANGED.items():
        pairs = [(load(f), load(pack[f"{ps2_t}-{key(f).split('-')[1]}"])) for f in files
                 if key(f).startswith(t) and f"{ps2_t}-{key(f).split('-')[1]}" in pack]
        if pairs:
            masks[t] = consensus(pairs)
            print(f"{t}: {100 * masks[t].mean():.1f}% changed, by {len(pairs)} colourings")
    for f in files:
        name = os.path.basename(f)
        t, p = key(f).split("-")
        if t in ALIASED:
            continue
        low = load(f)
        ps2 = pack.get(f"{CHANGED[t]}-{p}") if t in CHANGED else None
        if ps2:
            out, frac, fringe = composite(model, low, load(ps2), masks[t])
            how = (f"the pack's PS2 picture, {100 * frac:.1f}% upscaled where the Xbox changed it, "
                   f"{100 * fringe:.2f}% of its pixels cleaned")
        else:
            out, how = upscale(model, low), "upscaled"
        save(os.path.join(args.out, name), out)
        print(f"{name}: {low.shape[1]}x{low.shape[0]} to {out.shape[1]}x{out.shape[0]}, {how}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["calibrate", "make"])
    ap.add_argument("--misses", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("models", nargs="+")
    args = ap.parse_args()
    (calibrate if args.what == "calibrate" else make)(args)


if __name__ == "__main__":
    main()
