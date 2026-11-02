"""Where the USA default.xbe's functions and data are in the European one: the same game built later, its code and data
a little shifted. A hook needs its function's address in each release, common/release.h has them.

usage: python -I tools/release_map.py map <usa.xbe> <usa generated dir> <eu.xbe> <eu generated dir> <map.json>
       python -I tools/release_map.py find <map.json> <usa.xbe> <eu.xbe> <0xADDR[:f]> ...

map: each xbe's functions (the recompiler's x86_func_mapping.cpp, in generated/game and generated/game_eu once each
image is built) matched by their bytes with what moves between builds masked (4 byte values inside the image, call
and jump displacements), the ones repeated in either matched in order between matched neighbours; then the data the
instructions of each matched pair name, the USA value against the European one at the same place.
find: an address's European one: a function (":f") from the matched functions; data as matched instructions name it,
else from every instruction naming a value near it (its surroundings masked, found in the European code), else by the
shift its mapped neighbours agree on, which then wants a check in play (cvtest's "dump" against the USA release's
memory at the same moment).
"""
import bisect
import collections
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

IMAGE_LO, IMAGE_HI = 0x10000, 0x1400000


class Xbe:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        self.base, = struct.unpack_from("<I", self.d, 0x104)
        n, headers = struct.unpack_from("<II", self.d, 0x11C)
        self.sections = []
        for i in range(n):
            _, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", self.d, headers - self.base + i * 56)
            at = name_va - self.base
            self.sections.append((self.d[at:self.d.index(b"\0", at)].decode(), va, vsize, raw, rsize))

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def read(self, va, n):
        for _, sva, _, raw, rsize in self.sections:
            if sva <= va < sva + rsize:
                return self.d[raw + va - sva:raw + va - sva + n]
        return None

    def code(self):
        """.text: its address and bytes"""
        _, va, _, raw, rsize = self.section(".text")
        return va, self.d[raw:raw + rsize]


def mask(b):
    """per byte, whether two builds' copies of this code should agree on it"""
    keep, i, n = [True] * len(b), 0, len(b)
    while i < n:
        if b[i] in (0xE8, 0xE9) and i + 5 <= n:  # call, jmp rel32
            keep[i + 1:i + 5] = [False] * 4
            i += 5
        elif b[i] == 0x0F and i + 6 <= n and 0x80 <= b[i + 1] <= 0x8F:  # jcc rel32
            keep[i + 2:i + 6] = [False] * 4
            i += 6
        else:
            i += 1
    for i in range(n - 3):
        if IMAGE_LO <= int.from_bytes(b[i:i + 4], "little") < IMAGE_HI:
            keep[i:i + 4] = [False] * 4
    return keep


def find(hay, needle, keep, most=2):
    """where needle matches hay on its kept bytes, anchored on its longest kept run; at most `most`"""
    best, run, start = (0, 0), 0, 0
    for k, x in enumerate(keep + [False]):
        if x:
            start = k if run == 0 else start
            run += 1
        else:
            best = (start, run) if run > best[1] else best
            run = 0
    s, n = best
    if n < 6:
        return []
    anchor, hits, pos = bytes(needle[s:s + n]), [], -1
    while len(hits) < most:
        pos = hay.find(anchor, pos + 1)
        if pos < 0:
            break
        o = pos - s
        if 0 <= o and o + len(needle) <= len(hay) and all(not keep[k] or hay[o + k] == needle[k] for k in range(len(needle))):
            hits.append(o)
    return hits


def functions(gen):
    src = (Path(gen) / "x86_func_mapping.cpp").read_text()
    return sorted({int(a, 16) for a in re.findall(r"\{0x([0-9A-Fa-f]+)u?,\s*sub_", src)})


def build_map(usa_xbe, usa_gen, eu_xbe, eu_gen, out):
    ux, ex = Xbe(usa_xbe), Xbe(eu_xbe)
    ua, ea = functions(usa_gen), functions(eu_gen)

    def bodies(x, addrs):  # each function's bytes up to the next one, at most 8 KB
        out = {}
        for i, a in enumerate(addrs):
            b = x.read(a, min((addrs[i + 1] if i + 1 < len(addrs) else a + 64) - a, 8192))
            if b:
                out[a] = b
        return out

    def key(b):
        keep = mask(b)
        return hashlib.sha1(bytes(c if k else 0 for c, k in zip(b, keep)) + len(b).to_bytes(4, "little")).hexdigest()

    ub, eb = bodies(ux, ua), bodies(ex, ea)
    uk, ek = {a: key(b) for a, b in ub.items()}, {a: key(b) for a, b in eb.items()}
    by_u, by_e = collections.defaultdict(list), collections.defaultdict(list)
    for a, k in uk.items():
        by_u[k].append(a)
    for a, k in ek.items():
        by_e[k].append(a)
    fmap = {by_u[k][0]: by_e[k][0] for k in by_u if len(by_u[k]) == 1 and len(by_e.get(k, [])) == 1}
    seq_u, seq_e = sorted(ub), sorted(eb)
    anchors = sorted(fmap.items())
    for (u0, e0), (u1, e1) in zip(anchors, anchors[1:]):
        gu = [a for a in seq_u[bisect.bisect_right(seq_u, u0):bisect.bisect_left(seq_u, u1)]]
        ge = [a for a in seq_e[bisect.bisect_right(seq_e, e0):bisect.bisect_left(seq_e, e1)]]
        if len(gu) == len(ge):
            for a, b in zip(gu, ge):
                if uk[a] == ek[b]:
                    fmap[a] = b
    votes = collections.defaultdict(collections.Counter)
    for u, e in fmap.items():
        bu, be = ub[u], eb[e]
        if len(bu) != len(be):
            continue
        keep = mask(bu)
        for i in range(len(bu) - 3):
            if not keep[i] and (i == 0 or keep[i - 1]):
                vu, ve = int.from_bytes(bu[i:i + 4], "little"), int.from_bytes(be[i:i + 4], "little")
                if IMAGE_LO <= vu < IMAGE_HI and IMAGE_LO <= ve < IMAGE_HI:
                    votes[vu][ve] += 1
    data = {}
    for vu, c in votes.items():
        (ve, n), = c.most_common(1)
        data[f"{vu:#x}"] = {"eu": f"{ve:#x}", "votes": n, "others": sum(c.values()) - n}
    _, tu, _, _, tsize = ux.section(".text")
    in_text = [a for a in ua if tu <= a < tu + tsize]
    print(f"functions: USA {len(ua)}, EU {len(ea)}, matched {len(fmap)} "
          f"({sum(1 for a in in_text if a in fmap)} of {len(in_text)} in .text); data values {len(data)}")
    Path(out).write_text(json.dumps({"functions": {f"{u:#x}": f"{e:#x}" for u, e in sorted(fmap.items())},
                                     "data": dict(sorted(data.items(), key=lambda kv: int(kv[0], 16)))}, indent=0))


def find_addresses(map_json, usa_xbe, eu_xbe, args):
    m = json.loads(Path(map_json).read_text())
    fm = {int(k, 16): int(v, 16) for k, v in m["functions"].items()}
    dm = {int(k, 16): (int(v["eu"], 16), v["votes"], v["others"]) for k, v in m["data"].items()}
    keys = sorted(dm)
    ux, ex = Xbe(usa_xbe), Xbe(eu_xbe)
    (_, uc), (_, ec) = ux.code(), ex.code()
    for arg in args:
        a, _, kind = arg.partition(":")
        a = int(a, 16)
        if kind == "f":
            e = fm.get(a)
            if not e:  # changed a little: its first bytes
                for n in (64, 40, 24):
                    nd = ux.read(a, n)
                    tva, _ = ex.code()
                    hits = find(ec, nd, mask(nd))
                    if len(hits) == 1:
                        e = tva + hits[0]
                        break
            print(f"{a:#010x} function -> " + (f"{e:#010x}" + ("" if a in fm else " (its first bytes)") if e else "not found"))
            continue
        if a in dm:
            e, v, o = dm[a]
            print(f"{a:#010x} data -> {e:#010x} (named {v}x by matched functions, {o} disagree)")
            continue
        deltas = collections.Counter()
        for p in {((v >> 8) & 0xFFFFFF) for v in range(a - 0x80, a + 0x81)}:
            for hit in re.finditer(re.escape(p.to_bytes(3, "little")), uc):
                i = hit.start() - 1
                v = int.from_bytes(uc[i:i + 4], "little") if i >= 0 else -1
                if not a - 0x80 <= v <= a + 0x80:
                    continue
                lo, hi = max(0, i - 24), min(len(uc), i + 28)
                win = uc[lo:hi]
                hits = find(ec, win, mask(win))
                if len(hits) == 1:
                    deltas[int.from_bytes(ec[hits[0] + i - lo:hits[0] + i - lo + 4], "little") - v] += 1
        if deltas:
            (d, n), = deltas.most_common(1)
            print(f"{a:#010x} data -> {a + d:#010x} (from {n} of {sum(deltas.values())} instructions near it)")
            continue
        i = bisect.bisect_left(keys, a)
        lo = next((keys[j] for j in range(i - 1, max(i - 40, -1), -1) if dm[keys[j]][1] >= 2), None)
        hi = next((keys[j] for j in range(i, min(i + 40, len(keys))) if dm[keys[j]][1] >= 2), None)
        dl = dm[lo][0] - lo if lo else None
        dh = dm[hi][0] - hi if hi else None
        d = dl if dl is not None else dh
        print(f"{a:#010x} data -> " + (f"{a + d:#010x} (no instruction names it: its neighbours' shift "
                                        f"{'agrees' if dl == dh else 'differs'}, check it in play)" if d is not None else "not found"))


def main():
    if len(sys.argv) >= 7 and sys.argv[1] == "map":
        build_map(*sys.argv[2:7])
    elif len(sys.argv) >= 6 and sys.argv[1] == "find":
        find_addresses(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
