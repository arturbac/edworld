#!/usr/bin/env python3
# The digits' templates of src/pad_text_read.h: from the notice box's samples edworld writes to edworld_padtext
# (settings: pad_text_samples = 1, pad_text_unsure_sample = 1; 410x81 RGBA8, the row pitch in the name, the data
# source's pad in "src<n>"), each pad notice whose data source named the pad gives its number's glyphs that digit.
# A digit seen from afar (2026-10-25): its glyph's box in the game's own pixels cut into 4 x 6 zones, each zone's share
# of lit pixels in per mille, the area of pixels a zone's edge cuts counted in part; a template is the mean of its
# digit's glyphs. A pixel more or less at a stroke's edge (the box is 12, 13 or 14 px wide by where the number stands)
# then moves a zone a little, where scaling the box to a grid moved a whole column. The same steps and integer
# arithmetic as pad_text_read.h. Reads the samples only; writes the header whole.
# Usage: python3 -I tools/pad_text_digits.py <out header> <edworld_padtext dir>...
#        python3 -I tools/pad_text_digits.py --check <edworld_padtext dir>...   (all on all, and each pad's notices read
#        by the templates of the other pads, as a pad not seen yet: ok / unsure / WRONG, the least margin)
import sys
from collections import Counter, defaultdict
from pathlib import Path

W, H = 410, 81
BRIGHT = 170
ZX, ZY = 4, 6      # pad_zones_x, pad_zones_y
SURE = 20          # pad_digit_sure_percent


def bitmap(path):
    pitch = int(path.name.rsplit("pitch", 1)[1].split(".")[0])
    data = path.read_bytes()
    return [[max(data[y * pitch + x * 4:y * pitch + x * 4 + 3]) >= BRIGHT for x in range(W)] for y in range(H)]


def runs(flags, join):
    """[start, end) runs of True, gaps shorter than `join` joined"""
    out, start, gap, end = [], None, 0, 0
    for i, f in enumerate(flags + [False] * join):
        if f:
            start = i if start is None else start
            gap, end = 0, i + 1
        elif start is not None:
            gap += 1
            if gap >= join:
                out.append((start, end))
                start = None
    return out


def glyphs(bm, top, bottom):
    out = []
    for a, b in runs([any(bm[y][x] for y in range(top, bottom)) for x in range(W)], 1):
        ys = [y for y in range(top, bottom) if any(bm[y][x] for x in range(a, b))]
        out.append((a, b, min(ys), max(ys) + 1))
    return out


def number_glyphs(bm):
    """the number's glyphs of a pad notice, else None"""
    lines = [(a, b) for a, b in runs([any(r) for r in bm], 3) if b - a >= 8]
    if len(lines) != 2:
        return None
    (a1, b1), (a2, b2) = lines
    h1, h2 = b1 - a1, b2 - a2
    g1, g2 = glyphs(bm, a1, b1), glyphs(bm, a2, b2)
    if len(g1) < 3:
        return None
    dash = g1[-1]
    if not (10 * (dash[3] - dash[2]) < 3 * h1 and dash[1] - dash[0] >= 2 * (dash[3] - dash[2])):
        return None
    if 2 * (dash[0] - g1[-2][1]) <= h1:
        return None
    first = len(g1) - 2
    while first > 0 and 2 * (g1[first][0] - g1[first - 1][1]) <= h1:
        first -= 1
    digits = len(g1) - 1 - first
    if first == 0 or not 1 <= digits <= 2:
        return None
    colon = g2[1] if len(g2) == 4 else None
    if colon is None or 4 * (colon[1] - colon[0]) > h2:
        return None
    if all(any(bm[y][x] for x in range(colon[0], colon[1])) for y in range(colon[2], colon[3])):
        return None
    return g1[first:len(g1) - 1]


def zones(bm, g):
    """the glyph's zones' shares of lit pixels, per mille, row by row (pad_text_zones)"""
    x0, x1, y0, y1 = g
    w, h = x1 - x0, y1 - y0
    out = []
    for j in range(ZY):
        for i in range(ZX):
            lit = 0
            for y in range(h):
                oy = min((y + 1) * ZY, (j + 1) * h) - max(y * ZY, j * h)
                if oy <= 0:
                    continue
                for x in range(w):
                    ox = min((x + 1) * ZX, (i + 1) * w) - max(x * ZX, i * w)
                    if ox > 0 and bm[y0 + y][x0 + x]:
                        lit += ox * oy
            out.append(lit * 1000 // (w * h))
    return tuple(out)


def labelled(dirs):
    """(pad, [zones of its digits]) of each pad notice whose data source named the pad; counts of samples, notices"""
    out, samples, notices = [], 0, 0
    for path in sorted(p for d in dirs for p in Path(d).glob("pad_*_src*_*.raw")):
        samples += 1
        pad = int(path.name.split("_src", 1)[1].split("_", 1)[0])
        bm = bitmap(path)
        number = number_glyphs(bm)
        if number is None:
            continue
        notices += 1
        if pad and len(number) == len(str(pad)):
            out.append((pad, [zones(bm, g) for g in number]))
    return out, samples, notices


def templates(data):
    acc = defaultdict(list)
    for pad, digits in data:
        for z, d in zip(digits, str(pad)):
            acc[d].append(z)
    return {d: tuple((sum(z[k] for z in zs) + len(zs) // 2) // len(zs) for k in range(ZX * ZY)) for d, zs in acc.items()}


def read_digit(tpl, z):
    """(digit, margin %) as pad_text_digit: the nearest template; '?' when the next is not SURE % further"""
    best = sorted((sum(abs(a - b) for a, b in zip(z, t)), d) for d, t in tpl.items())
    d1, d2 = best[0][0], best[1][0]
    margin = 100 * (d2 - d1) // d2 if d2 else 0
    return (best[0][1] if 100 * (d2 - d1) >= SURE * d2 and d2 else "?"), margin


def check(dirs):
    data = labelled(dirs)[0]
    pads = sorted({p for p, _ in data})
    runs_ = [("all on all", [(templates(data), data)])]
    runs_.append(("each pad by the others", [(templates([x for x in data if x[0] != p]), [x for x in data if x[0] == p])
                                             for p in pads]))
    for name, parts in runs_:
        res, least, bad = Counter(), {}, Counter()
        for tpl, test in parts:
            if len(tpl) != 10:
                print(f"  {name}: templates for {''.join(sorted(tpl))} only, pad left unread")
                continue
            for pad, digits in test:
                reads = [read_digit(tpl, z) for z in digits]
                number = "".join(r[0] for r in reads)
                k = "unsure" if "?" in number else ("ok" if number == str(pad) else "WRONG")
                res[k] += 1
                if k != "ok":
                    bad[f"{pad}->{number}"] += 1
                for (d, mg), ch in zip(reads, str(pad)):
                    least[f"{pad}/{ch}"] = min(least.get(f"{pad}/{ch}", 999), mg)
        weakest = sorted(least.items(), key=lambda kv: kv[1])[:6]
        print(f"{name}: ok {res['ok']}, unsure {res['unsure']}, WRONG {res['WRONG']}; least margin % (pad/digit): "
              + " ".join(f"{k}:{v}" for k, v in weakest)
              + ("; not ok: " + " ".join(f"{k} x{n}" for k, n in sorted(bad.items())) if bad else ""))


def main(out, dirs):
    data, samples, notices = labelled(dirs)
    tpl = templates(data)
    if sorted(tpl) != list("0123456789"):
        sys.exit(f"pad_text_digits: templates for {''.join(sorted(tpl))} only - no header written")
    count = Counter(d for pad, _ in data for d in str(pad))
    rows = [f"    {{'{d}', {{{', '.join(str(v) for v in tpl[d])}}}}},  // {count[d]} glyph(s)\n" for d in sorted(tpl)]
    text = ("// Generated by tools/pad_text_digits.py from the notice box's samples of edworld_padtext; not edited by hand.\n"
            "// A digit's zones (4 x 6, row by row): the share of lit pixels in per mille (src/pad_text_read.h).\n"
            "#pragma once\n\nnamespace edworld\n  {\n  inline constexpr pad_digit_t pad_digits[]{\n" + "".join(rows)
            + "  };\n  }  // namespace edworld\n")
    out = Path(out)
    tmp = out.with_suffix(".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(out)
    print(f"pad_text_digits: {samples} sample(s), {notices} pad notice(s), {len(data)} labelled -> {out}")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--check":
        check(sys.argv[2:])
    elif len(sys.argv) >= 3:
        main(sys.argv[1], sys.argv[2:])
    else:
        sys.exit("usage: python3 -I tools/pad_text_digits.py <out header> <edworld_padtext dir>...\n"
                 "       python3 -I tools/pad_text_digits.py --check <edworld_padtext dir>...")
