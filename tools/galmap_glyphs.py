#!/usr/bin/env python3
# The glyphs' templates of src/galmap.h: from a whole-frame dump on the galaxy map with the HUD text's VS in dump_vs
# (its draws' vertex buffer 0, stride 24: x, y, ?, ?, u, v as floats, and the font at PS t1, 2048x2048 A8), each glyph's
# rectangle in the font named by a file of `u0 v0 u1 v1 char` lines (font pixels; `glow` for the blurred copy drawn
# under a letter). The file may name more rectangles than the dump's draws use: glyphs of the same font and size the
# game put in its font for other text (a letter no label showed yet), cut out of the font by hand. A glyph: the box
# of its ink within its rectangle (pixels over 60; of the rows with ink the longest run without a gap, so a bit of a
# neighbour's glyph over or under it is left out; the columns with ink in those rows), cut into 6 x 9 zones, each
# zone's mean coverage 0..255, the zone's edges at x0 + w * i / 6 (integer division, at least a pixel) - the same
# steps and integer arithmetic as galmap.h, so a template is what edworld computes for that glyph wherever the game
# puts it. Reads the dump only; writes the header whole.
# Glyphs known only by the zones edworld logged for them come from tools/galmap_glyph_zones.tsv beside this script
# (`char` and 54 hex zones a line), added as they are.
# Usage: python3 -I tools/galmap_glyphs.py <out header> <dump dir> <stamp> <names tsv>
#        python3 -I tools/galmap_glyphs.py --check <dump dir> <stamp> <names tsv>   (each glyph against the others:
#        the nearest other character's distance, the least of them; a glyph of the same character apart from it)
import struct
import sys
from pathlib import Path

ZX, ZY = 6, 9  # galmap_zones_x, galmap_zones_y
INK = 60       # galmap_ink
FONT = 2048


def font_of(d, stamp):
    files = sorted(d.glob(f"{stamp}_a*_ps1_2048x2048_f65_pitch2048.raw"))
    if not files:
        sys.exit(f"no font (PS t1 2048x2048 f65) in {d} for {stamp}")
    return files[0].read_bytes()


def ink_box(font, u0, v0, u1, v1):
    rows = [any(font[y * FONT + x] > INK for x in range(u0, u1)) for y in range(v0, v1)]
    best, start = (0, 0), None
    for i, lit in enumerate(rows + [False]):
        if lit and start is None:
            start = i
        elif not lit and start is not None:
            if i - start > best[1] - best[0]:
                best = (start, i)
            start = None
    if best[1] == best[0]:
        return None
    y0, y1 = v0 + best[0], v0 + best[1]
    cols = [x for x in range(u0, u1) if any(font[y * FONT + x] > INK for y in range(y0, y1))]
    return cols[0], y0, cols[-1] + 1, y1


def zones(font, u0, v0, u1, v1):
    box = ink_box(font, u0, v0, u1, v1)
    if box is None:
        return [0] * (ZX * ZY)
    u0, v0, u1, v1 = box
    w, h = u1 - u0, v1 - v0
    out = []
    for zy in range(ZY):
        y0 = v0 + h * zy // ZY
        y1 = max(y0 + 1, v0 + h * (zy + 1) // ZY)
        for zx in range(ZX):
            x0 = u0 + w * zx // ZX
            x1 = max(x0 + 1, u0 + w * (zx + 1) // ZX)
            total = sum(font[y * FONT + x] for y in range(y0, y1) for x in range(x0, x1))
            out.append(total // ((y1 - y0) * (x1 - x0)))
    return out


def rects_used(d, stamp):
    """the glyphs' rectangles the dump's text draws use, as edworld rounds them"""
    used = set()
    for vb in sorted(d.glob(f"{stamp}_a*_vb0_s24.bin")):
        a = vb.name.split("_a")[1].split("_")[0]
        ib = d / f"{stamp}_a{a}_ib_u16.bin"
        data, idx = vb.read_bytes(), ib.read_bytes()
        indices = struct.unpack(f"<{len(idx) // 2}H", idx)
        for q in range(len(indices) // 6):
            corners = sorted(set(indices[q * 6:q * 6 + 6]))
            if len(corners) != 4 or (corners[-1] + 1) * 24 > len(data):
                continue
            uv = [struct.unpack_from("<6f", data, i * 24)[4:6] for i in corners]
            used.add((round(min(u for u, _ in uv) * FONT), round(min(v for _, v in uv) * FONT),
                      round(max(u for u, _ in uv) * FONT), round(max(v for _, v in uv) * FONT)))
    return used


def named(path):
    names = {}
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        u0, v0, u1, v1, c = line.split("\t")
        names[(int(u0), int(v0), int(u1), int(v1))] = c
    return names


def main():
    check = sys.argv[1] == "--check"
    args = sys.argv[2:] if check else sys.argv[2:]
    out = None if check else Path(sys.argv[1])
    d, stamp, names_path = Path(args[0]), args[1], args[2]
    font = font_of(d, stamp)
    names = named(names_path)
    used = rects_used(d, stamp)
    missing = used - set(names)
    if missing:
        sys.exit(f"{len(missing)} glyph(s) of the dump not named in {names_path}")
    glyphs = [(names[r], zones(font, *r), r) for r in sorted(names)]
    logged = Path(__file__).with_name("galmap_glyph_zones.tsv")
    if logged.exists():
        for line in logged.read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                c, hexes = line.split("\t")
                glyphs.append((c, [int(h, 16) for h in hexes.split()], None))
    if check:
        least = None
        for c, z, r in glyphs:
            near = min((sum(abs(a - b) for a, b in zip(z, oz)), oc) for oc, oz, orr in glyphs if (orr != r or r is None) and oc != c)
            same = [sum(abs(a - b) for a, b in zip(z, oz)) for oc, oz, orr in glyphs if orr != r and oc == c]
            print(f"{c:5} {r}  nearest other {near[1]!r} at {near[0]}" + (f", same at {min(same)}" if same else ""))
            least = near[0] if least is None else min(least, near[0])
        print(f"least distance between two characters: {least}")
        return
    lines = [
        "// Generated by tools/galmap_glyphs.py from a whole-frame dump on the galaxy map; not edited by hand.",
        f"// A glyph's zones ({ZX} x {ZY}, row by row): the mean coverage of the font's pixels, 0..255 (src/galmap.h);",
        "// '\\0' is the blurred copy the game draws under a letter.",
        "#pragma once",
        "",
        "namespace edworld",
        "  {",
        "  inline constexpr galmap_glyph_t galmap_glyphs[]{",
    ]
    for c, z, r in glyphs:
        ch = "'\\0'" if c == "glow" else ("'\\''" if c == "'" else f"'{c}'")
        where = f"{r[0]},{r[1]} {r[2] - r[0]}x{r[3] - r[1]}" if r else "from the log (galmap_glyph_zones.tsv)"
        lines.append(f"    {{{ch}, {{{', '.join(str(v) for v in z)}}}}},  // {where}")
    lines += ["  };", "  }  // namespace edworld", ""]
    out.write_text("\n".join(lines))
    print(f"{len(glyphs)} glyph(s), {sum(1 for c, _, _ in glyphs if c != 'glow')} letters, to {out}")


if __name__ == "__main__":
    main()
