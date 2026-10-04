#!/usr/bin/env python3
"""edworld surface dumps (edworld_dumps/*.raw, name: <stamp>_<id>_<w>x<h>_f<dxgi>_pitch<n>.raw) -> PNG.
8-bit RGBA formats only (DXGI 27..29, 87..91 as BGRA). Writes <name>.png (premultiplied colour over black).
Usage: dump_to_png.py <raw files or a directory>"""
import os, re, sys
import numpy as np
from PIL import Image

NAME = re.compile(r"_(\d+)x(\d+)_f(\d+)_pitch(\d+)\.raw$")

def convert(path):
    m = NAME.search(path)
    if not m:
        return print("skip (name)", path)
    w, h, fmt, pitch = map(int, m.groups())
    if fmt not in (27, 28, 29, 87, 88, 90, 91):
        return print(f"skip (format {fmt})", path)
    data = np.fromfile(path, dtype=np.uint8)
    if data.size < pitch * h:
        return print("skip (short)", path)
    img = data[: pitch * h].reshape(h, pitch)[:, : w * 4].reshape(h, w, 4)
    if fmt in (87, 88, 90, 91):
        img = img[:, :, [2, 1, 0, 3]]
    # interface surfaces are premultiplied: the colour channels are shown as they are, over black
    Image.fromarray(np.ascontiguousarray(img[:, :, :3]), "RGB").save(path[:-4] + ".png")
    print("wrote", path[:-4] + ".png", f"{w}x{h} f{fmt} alpha>0: {float((img[:, :, 3] > 0).mean()):.0%}")

for arg in sys.argv[1:]:
    paths = [os.path.join(arg, f) for f in sorted(os.listdir(arg)) if f.endswith(".raw")] if os.path.isdir(arg) else [arg]
    for p in paths:
        convert(p)
