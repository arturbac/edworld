#!/usr/bin/env python3
"""Reads the geometry dumps edworld writes beside its surface dumps (edworld_dumps/<stamp>_g<draw>_<what>.bin, the
draw's arguments in the log) and says, for each panel draw, which rectangle of its surface the draw shows and where
its corners land in clip space - by the arithmetic of the game's panel vertex shader as EDVR transcribed it
(src/d3d11/fss_panel_vs.h): the atlas coordinate is the LOCAL vertex projected on (n, b) through cb2[6..7]; the
vertex is placed by its instance record (t33, stride 336: scale, unorm16x4 quaternion, position) less cb1[275] and
projected by cb0[4..7].
Usage: geometry_dump.py <edworld log> <edworld_dumps dir> [stamp]   (the last dump in the log when no stamp)"""
import re
import struct
import sys
from pathlib import Path


def floats(data, row, count=4):
    return struct.unpack_from(f"<{count}f", data, row * 16)


def decode_pos(x, y, z):
    fmt = (z >> 24) & 0x7F
    if fmt == 64:
        lx, hx, ly, hy = x & 0xFFFF, x >> 16, y & 0xFFFF, y >> 16
        s = 2.0 ** (hy * 0.000244) - 1.0
        return tuple((v * 0.000031 - 1.0) * s for v in (lx, hx, ly))
    sh = z >> 24
    scl = 1.0 / ((1 << ((20 - fmt) & 31)) - 1)
    offs = float(1 << (sh & 31))
    xb = x & 0x1FFFFF
    yb = (x >> 21) + ((y & 0x3FF) << 11)
    zb = (y >> 10) & 0x1FFFFF
    return (xb * scl - offs, yb * scl - offs, zb * scl - offs)


def unit(v):
    return v * 0.007874 - 1.0


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def quat_rotate(q, p):
    c = (q[1] * p[2] - p[1] * q[2], q[2] * p[0] - p[2] * q[0], q[0] * p[1] - p[0] * q[1])
    d = dot(q[:3], p)
    t = q[3] * (q[3] + q[3])
    r = [t * p[i] - p[i] for i in range(3)]
    r = [d * (q[i] + q[i]) + r[i] for i in range(3)]
    return [c[i] * (q[3] + q[3]) + r[i] for i in range(3)]


def main():
    log = Path(sys.argv[1]).read_text(errors="replace")
    dumps = Path(sys.argv[2])
    stamps = re.findall(r"dump: armed \((\S+)\)", log)
    stamp = sys.argv[3] if len(sys.argv) > 3 else stamps[-1]
    part = log[log.index(f"dump: armed ({stamp})"):]
    draws = re.findall(r"dump: g(\d+) draw: indices (\d+) from (\d+), base vertex (-?\d+), instances (\d+) from (\d+), "
                       r"surface (\w+) (\d+)x(\d+)", part)
    seen = set()
    for g, n_idx, _, _, n_inst, _, surf, sw, sh in draws:
        if g in seen:
            break
        seen.add(g)
        sw, sh = int(sw), int(sh)
        f = lambda what: dumps / f"{stamp}_g{g}_{what}"
        ib_files = list(dumps.glob(f"{stamp}_g{g}_ib_u*.bin"))
        vb_files = sorted(dumps.glob(f"{stamp}_g{g}_vb[1-3]_s*.bin"))
        print(f"g{g}: surface {surf} {sw}x{sh}, {n_idx} indices, {n_inst} instance(s)")
        if not ib_files or not vb_files:
            print("  no index or vertex buffer")
            continue
        ib = ib_files[0].read_bytes()
        idx = struct.unpack(f"<{len(ib) // 2}H", ib) if ib_files[0].name.endswith("u16.bin") else \
            struct.unpack(f"<{len(ib) // 4}I", ib)
        vb = vb_files[0].read_bytes()
        stride = int(re.search(r"_s(\d+)\.bin", vb_files[0].name).group(1))
        cb0 = f("cb0.bin").read_bytes() if f("cb0.bin").exists() else None
        cb1 = f("cb1.bin").read_bytes() if f("cb1.bin").exists() else None
        cb2 = f("cb2.bin").read_bytes() if f("cb2.bin").exists() else None
        if cb2 is None or len(cb2) < 128 or stride < 48:
            print(f"  vertex stride {stride}, cb2 {'missing' if cb2 is None else len(cb2)} - not a panel family draw")
            continue
        a6, a7 = floats(cb2, 6), floats(cb2, 7)
        print(f"  cb2[6] {a6}\n  cb2[7] {a7}")
        uvs, locals_ = [], []
        for i in sorted(set(idx)):
            x, y, z, w = struct.unpack_from("<4I", vb, i * stride)
            p = decode_pos(x, y, z)
            n = (unit(w & 255), unit((w >> 8) & 255), unit((w >> 16) & 255))
            t = (unit(w & 255), unit((z >> 8) & 255), unit((z >> 16) & 255))
            b = cross(t, n)
            pr = (dot(p, n), dot(p, b))
            uv = (pr[0] * a7[0] + a6[0], pr[1] * a7[1] + a6[1])
            uvs.append(uv)
            locals_.append(p)
            print(f"  v{i}: p ({p[0]:.4f} {p[1]:.4f} {p[2]:.4f}) n ({n[0]:.3f} {n[1]:.3f} {n[2]:.3f}) "
                  f"b ({b[0]:.3f} {b[1]:.3f} {b[2]:.3f}) uv ({uv[0]:.4f} {uv[1]:.4f}) = px ({uv[0] * sw:.0f} {uv[1] * sh:.0f})")
        u0, u1 = min(u for u, _ in uvs), max(u for u, _ in uvs)
        v0, v1 = min(v for _, v in uvs), max(v for _, v in uvs)
        print(f"  shows surface x {u0 * sw:.0f}..{u1 * sw:.0f}, y {v0 * sh:.0f}..{v1 * sh:.0f}")
        vb0 = f(f"vb0_s8.bin")
        t33 = f("t33.bin")
        if cb0 and cb1 and vb0.exists() and t33.exists() and len(cb1) >= 276 * 16:
            record = struct.unpack_from("<I", vb0.read_bytes(), 0)[0]
            pool = t33.read_bytes()
            if (record + 1) * 336 <= len(pool):
                scale = struct.unpack_from("<f", pool, record * 336 + 4)[0]
                xy, zw = struct.unpack_from("<2I", pool, record * 336 + 8)
                q = [(xy & 0xFFFF) * 0.000031 - 1.0, (xy >> 16) * 0.000031 - 1.0,
                     (zw & 0xFFFF) * 0.000031 - 1.0, (zw >> 16) * 0.000031 - 1.0]
                pos = struct.unpack_from("<3f", pool, record * 336 + 16)
                rebase = floats(cb1, 275)
                rows = [floats(cb0, r) for r in (4, 5, 6, 7)]
                print(f"  record {record}: scale {scale:.4f} q ({q[0]:.3f} {q[1]:.3f} {q[2]:.3f} {q[3]:.3f})")
                for p, uv in zip(locals_, uvs):
                    world = [r * scale + pos[k] - rebase[k] for k, r in enumerate(quat_rotate(q, p))] + [1.0]
                    clip = [dot(row, world) for row in rows]
                    print(f"    px ({uv[0] * sw:.0f} {uv[1] * sh:.0f}) -> ndc ({clip[0] / clip[3]:.4f} {clip[1] / clip[3]:.4f})")
            else:
                print(f"  record {record} past the pool ({len(pool)} bytes)")


if __name__ == "__main__":
    main()
