#!/usr/bin/env python3
"""Live view of what edworld publishes (the share file, default /dev/shm/edworld).

Prints, a few times a second, every cockpit panel draw of the latest frame: interface surface size, draw
arguments and where the panel's local origin lands on screen (NDC, x right, y up, -1..1).
Usage: edworld_watch.py [path] [--once]"""
import mmap, struct, sys, time

PANEL = struct.Struct("<Q 10I 48f 4f")  # surface_id, 10 u32/i32 fields, cb0[12][4], anchor_clip[4]
HEAD = struct.Struct("<4I Q Q q 2I")
MAGIC, VERSION, MAX_PANELS = 0x44575745, 1, 64

def read(buf):
    for _ in range(100):
        s1 = struct.unpack_from("<I", buf, 12)[0]
        if s1 % 2:
            continue
        head = HEAD.unpack_from(buf, 0)
        panels = [PANEL.unpack_from(buf, HEAD.size + i * PANEL.size) for i in range(min(head[8], MAX_PANELS))]
        if struct.unpack_from("<I", buf, 12)[0] == s1:
            return head, panels
    return None, []

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    path = args[0] if args else "/dev/shm/edworld"
    with open(path, "rb") as f:
        buf = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    while True:
        head, panels = read(buf)
        if head is None or head[0] != MAGIC or head[1] != VERSION:
            print("no valid record yet")
        else:
            age = time.time() * 1000 - head[6]
            print(f"frame {head[4]} (drawn {head[5]}), {head[8]} panel(s), age {age:.0f} ms")
            for i, p in enumerate(panels):
                sid, w, h, fmt, vs, n, inst, si, bv, sinst, ordinal = p[:11]
                clip = p[59:63]
                where = f"ndc {clip[0]/clip[3]:+.4f} {clip[1]/clip[3]:+.4f} w {clip[3]:.3f}" if clip[3] > 1e-4 else "behind"
                print(f"  #{ordinal:2} vs{vs} surf {sid & 0xffffffff:08x} {w}x{h} f{fmt} n{n} x{inst} inst{sinst} {where}")
        if "--once" in sys.argv:
            return
        time.sleep(0.25)

if __name__ == "__main__":
    main()
