#!/usr/bin/env python3
# The catalog of the game's shaders (edworld_eht, shader_catalog; doc/developers.md), read offline.
#   shader_catalog.py names <build dir> <PlanetShaders dir>   the catalog's shaders found whole in the game's .csa
#                                                             archives: their archive and block (names.tsv in the
#                                                             build dir, and a summary)
#   shader_catalog.py diff <old build dir> <new build dir>    what an update changed: the same bytes, the same shape
#                                                             under other bytes, new, gone; the watched shaders first
#   shader_catalog.py --self-test
# A build dir is edloader\logs\edworld_shaders\<build> (catalog.tsv and the bytecode). Prints counts, hashes and
# shapes only; reads the game's files, writes only names.tsv into the build dir.
import os
import struct
import sys
from collections import defaultdict

# the shaders edworld and EHT stand on: the panels' VS family, the holograms' models, the HUD's composition
WATCHED = {
    "81216C77F90DEDD6": "panels (VS family)",
    "1989E6D3B405FDE0": "panels, GUI effects off",
    "925ACEDA0153AA5B": "panels near a war settlement",
    "5B0068AF5630F96B": "holograms' models",
    "5559BD94B6852E83": "HUD's composition (holograms laid over)",
}
FIELDS = ["hash", "stage", "size", "model", "inputs", "outputs", "decls", "instructions", "chunks"]


def fnv1a64(data):
    """edworld's hash (panel_math.h), EDVR's names: FNV-1a 64 with EDVR's offset, one digit short of FNV's own"""
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def read_catalog(build_dir):
    rows = {}
    with open(os.path.join(build_dir, "catalog.tsv"), encoding="latin-1") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 8:
                row = dict(zip(FIELDS, parts + [""] * (len(FIELDS) - len(parts))))
                rows[row["hash"]] = row
    return rows


def shape(row):
    """what tells a shader again under other bytes: the stage, the signatures and the declarations"""
    return (row["stage"], row["model"], row["inputs"], row["outputs"], row["decls"])


def dxbc_blocks(data):
    """each DXBC container whole in the bytes: (offset, bytes)"""
    at = data.find(b"DXBC")
    while at >= 0:
        if at + 32 <= len(data):
            total = struct.unpack_from("<I", data, at + 24)[0]
            if 32 <= total <= len(data) - at:
                yield at, data[at:at + total]
        at = data.find(b"DXBC", at + 4)


def csa_blocks(path):
    with open(path, "rb") as f:
        return list(dxbc_blocks(f.read()))


def names(build_dir, planet_dir):
    rows = read_catalog(build_dir)
    found = {}
    for name in sorted(os.listdir(planet_dir)):
        if not name.lower().endswith(".csa"):
            continue
        for k, (offset, blob) in enumerate(csa_blocks(os.path.join(planet_dir, name))):
            h = "%016X" % fnv1a64(blob)
            found.setdefault(h, []).append("%s#%d" % (name, k))
    named = {h: where for h, where in found.items() if h in rows}
    with open(os.path.join(build_dir, "names.tsv"), "w") as f:
        for h in sorted(named):
            f.write("%s\t%s\t%s\n" % (h, rows[h]["stage"], " ".join(named[h])))
    print("blocks in the archives: %d distinct; in the catalog: %d (names.tsv)" % (len(found), len(named)))
    for h in sorted(named):
        print("  %s %s %s" % (h, rows[h]["stage"], " ".join(named[h])))


def diff(old_dir, new_dir):
    old, new = read_catalog(old_dir), read_catalog(new_dir)
    by_shape = defaultdict(list)
    for h, row in old.items():
        by_shape[shape(row)].append(h)
    same = [h for h in new if h in old]
    moved, ambiguous, fresh = {}, {}, []
    for h, row in new.items():
        if h in old:
            continue
        candidates = [o for o in by_shape.get(shape(row), []) if o not in new]
        if len(candidates) == 1:
            moved[h] = candidates[0]
        elif candidates:
            ambiguous[h] = candidates
        else:
            fresh.append(h)
    gone = [h for h in old if h not in new and h not in moved.values()]
    print("old %d, new %d: the same bytes %d, the same shape under other bytes %d (more than one old with that shape %d), "
          "new %d, gone %d" % (len(old), len(new), len(same), len(moved), len(ambiguous), len(fresh), len(gone)))
    for h, what in WATCHED.items():
        if h in new:
            state = "kept"
        elif h in old:
            nh = [n for n, o in moved.items() if o == h]
            amb = [n for n, c in ambiguous.items() if h in c]
            state = ("MOVED to " + nh[0]) if nh else ("ONE OF " + " ".join(amb)) if amb else "GONE"
        else:
            state = "not in the old catalog"
        print("  watched %s %s: %s" % (h, what, state))
    stages = defaultdict(lambda: [0, 0, 0])
    for h in fresh:
        stages[new[h]["stage"]][0] += 1
    for h in gone:
        stages[old[h]["stage"]][1] += 1
    for h in moved:
        stages[new[h]["stage"]][2] += 1
    for s in sorted(stages):
        print("  %s: new %d, gone %d, moved %d" % (s, *stages[s]))


def self_test():
    assert fnv1a64(b"") == 1469598103934665603
    assert fnv1a64(b"a") == 0x44BD8AD473CD9906  # as panel_math.h has it
    blob = b"DXBC" + b"\0" * 20 + struct.pack("<II", 40, 0) + b"\0" * 8
    assert [o for o, _ in dxbc_blocks(b"xx" + blob + b"yy")] == [2]
    a = {"stage": "vs", "model": "5.0", "inputs": "POSITION0", "outputs": "SV_POSITION0", "decls": "cb0[12]"}
    assert shape(a) == shape(dict(a)) and shape(a) != shape(dict(a, decls="cb0[8]"))
    print("self-test PASSED")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
    elif len(sys.argv) == 4 and sys.argv[1] == "names":
        names(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == "diff":
        diff(sys.argv[2], sys.argv[3])
    else:
        sys.exit("usage: shader_catalog.py names <build dir> <PlanetShaders dir> | diff <old> <new> | --self-test")
