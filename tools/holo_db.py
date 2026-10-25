#!/usr/bin/env python3
# The hologram models' descriptions (PoC, 2026-10-23): the samples edworld_eht writes to edworld_models\ (the model's
# indices and vertices, the picture of its hologram) gathered into one JSON file a person describes in a page of a
# browser, so each model of holo_models.h stands on a picture someone looked at (doc/developers.md).
#   holo_db.py scan <edworld_models dir> <db dir> [journal dir] [--journal-tool <path>]   new samples into
#                                         <db dir>/models.json: the game's picture as PNG, three views of the mesh
#                                         (top, front, side), the journal's events around the sample as hints; a
#                                         description written by a person stays. With EHT's journal_context the
#                                         place before and after a sample its context places in the journal
#   holo_db.py form <db dir> [seconds]    the page to describe the models, served on 127.0.0.1 only (a random port and
#                                         a key in the address) until saved and closed, or the time is up (3600 s);
#                                         each save keeps the previous file as models.json.bak
#   holo_db.py models-header <db dir>     the models confirmed in the form into src/holo_models_generated.h, each on the
#                                         builds it was seen on, with its fingerprint (the owner's script that builds them in)
#   holo_db.py header <db dir>            the pads of the confirmed models into src/holo_pads_generated.h (the same script)
#   holo_db.py --self-test
# The pads (2026-10-23): scan also reads edworld_eht's logs beside <edworld_models dir> and the journal: at each Docked
# after a grant the log told (`state: pad <n> given`), the hologram's axes in the minute after it give the ship's up and
# nose in the model's axes - docked, the ship holds still on its pad. One row a model, a station and a pad, its looks
# put together. A pad is no one's to confirm (2026-10-24): it stands on the model the hologram showed, so it goes to
# edworld_eht once that model is confirmed in the form with a role of a place to dock at (DOCK_ROLES), and is left out when
# the model is confirmed as anything else (a ship, a planet); a docking off the model's planes is left out on its own,
# and of noses that differ the most dockings' is taken. scan also keeps each sample's fingerprint from the log
# ("hologram model fingerprint"), so `models-header` gives edworld_eht the confirmed models on their builds (2026-10-24).
# One row is one model: the samples of the same mesh (vertices, triangles, the box's proportions) on any build of the
# game, so a model an update renumbered stays one row. A row with no picture waits for one: it cannot be confirmed
# and gives no header row. Reads the samples and the journals, writes only into <db dir>.
import hmac
import html
import json
import math
import os
import re
import secrets
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import zlib
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

HEADER = Path(__file__).resolve().parent.parent / "src" / "holo_models.h"
PLANES_HEADER = Path(__file__).resolve().parent.parent / "src" / "hologram_math.h"
PADS_HEADER = Path(__file__).resolve().parent.parent / "src" / "holo_pads_generated.h"
MODELS_HEADER = Path(__file__).resolve().parent.parent / "src" / "holo_models_generated.h"
LOG_SAMPLE = re.compile(r"^(\S+)Z hologram model sample: (\d+) indices \((.*?) surface\), .*?, build (\S+?):")
LOG_FINGERPRINT = re.compile(r"^\S+Z hologram model fingerprint (\d+): (\{\d+u, \d+u, .*\}\}\})\s*$")
LOG = re.compile(r"^edworld_eht(\.\d{8}T\d{6}Z)?\.log$")
LOG_GRANT = re.compile(r"^(\S+)Z state: pad (\d+) given")
LOG_AXES = re.compile(r"^(\S+)Z hologram: (\d+) indices, scale \S+, q [^|]*\| X (\S+) (\S+) (\S+) Y (\S+) (\S+) (\S+) Z (\S+) (\S+) (\S+)")
PAD_GRANT_S = (-5, 15)   # the log's grant around the journal's
PAD_LOOK_S = (1, 60)     # the looks after Docked
PLANE_SNAP_DEG = 6.0     # a measured plane this near one of the model's is that one
NOSE_ALIKE_DEG = 30.0    # two dockings' noses this near are the same way on the pad
# the roles of models a ship docks at: their pads go to edworld_eht; any other role confirmed leaves them out
DOCK_ROLES = ("carrier", "port-orbital", "port-surface", "settlement", "construction-orbital",
              "operations-carrier", "operations", "station")
# the pad a ship needs, by the journal's Ship: the pads granted tell a construction site's layout (Artur 2026-10-23)
SHIP_PAD = {**{s: "S" for s in ("sidewinder", "eagle", "empire_eagle", "hauler", "adder", "viper", "viper_mkiv", "cobramkiii",
                                "cobramkiv", "diamondback", "diamondbackxl", "empire_courier", "vulture", "dolphin",
                                "smallcombat01_nx")},
            **{s: "M" for s in ("python", "python_nx", "krait_mkii", "krait_light", "asp", "asp_scout", "federation_dropship",
                                "federation_dropship_mkii", "federation_gunship", "type6", "type8", "typex", "typex_2",
                                "typex_3", "ferdelance", "mandalay", "independant_trader", "lakonminer", "corsair", "mamba")},
            **{s: "L" for s in ("anaconda", "federation_corvette", "cutter", "type7", "type9", "type9_military",
                                "belugaliner", "orca", "panthermkii", "explorer_nx")}}
SAMPLE = re.compile(r"^(\d+-[0-9a-f]+)_(\d{8}T\d{6}_\d{3}Z)_(\d+)_(holo\.pgm|ib_u16\.bin|ib_u32\.bin|vb_s40\.bin|context\.json)$")
VERTEX_STRIDE = 40
VIEW = 256  # one view's side in pixels
# what a model is (Artur 2026-10-23); station and other kept for the rows described before; the header takes ship as a
# ship and every other as a station
ROLES = ("", "ship", "carrier", "port-orbital", "port-surface", "settlement", "construction-orbital",
         "operations-carrier", "operations", "installation", "space-entity", "poi", "pad", "station", "other")
ROLE_TITLES = {
    "ship": "players' ships",
    "carrier": "carriers: the squadron's, players' fleet carriers, the colonisation ship",
    "port-orbital": "orbital ports, T1/T2/T3",
    "port-surface": "surface ports (a crater outpost)",
    "settlement": "planetary settlements, also under construction (one hologram: on-foot settlements and planetary "
                  "construction sites, medium; the larger for large settlements and surface ports)",
    "construction-orbital": "the orbital construction site",
    "operations-carrier": "Operations' small carrier with 4 pads",
    "operations": "Operations, PowerPlay, Thargoids: the large ship that shows like a carrier and other such",
    "installation": "orbital installations and planetary hubs: no pads, no docking",
    "space-entity": "stars, planets, moons, orbits with their moons",
    "poi": "points of interest on planets without pads (a human construction, a hidden stash)",
    "pad": "the pads' own model after a grant",
    "station": "a station, before the roles above",
    "other": "none of the above",
}
# the kinds of each role (2026-10-24): the build types of Raven Colonial's page (Artur saved it, tabela.tsv: 55 types,
# 110 sub-types) where the type or its sub-type tells the hologram apart - the sub-types of the ports (each its own
# hologram; a surface outpost's sub-types also lay their pads out apart), "Type: subtype unknown" when it cannot be told;
# the construction site, the carriers from the game and Artur (2026-10-22); a settlement's one hologram for all: none
SUBTYPE_UNKNOWN = "subtype unknown"


def kinds(types):
    """'Type: sub-type' for each sub-type, then 'Type: subtype unknown' for each type of more than none"""
    return ([f"{name}: {sub}" for name, subs in types for sub in subs]
            + [f"{name}: {SUBTYPE_UNKNOWN}" for name, subs in types if subs])


# the one model the HUD shows at every T1 outpost, docked or without a target (3576 indices on 2026-10-24's build; Artur
# 2026-10-24 on six pictures of five stations: one model from other sides)
T1_ANY = "Outpost: any (the model of every T1)"
TYPES = {
    "port-orbital": kinds([
        ("Commercial Outpost", ["plutus"]), ("Industrial Outpost", ["vulcan"]), ("Pirate Outpost", ["dysnomia"]),
        ("Civilian Outpost", ["vesta"]), ("Scientific Outpost", ["prometheus"]), ("Military Outpost", ["nemesis"]),
        ("Coriolis Starport", ["no_truss", "dual_truss", "quad_truss"]), ("Asteroid Starport", ["asteroid"]),
        ("Dodec Starport", ["dodec", "quint_truss", "dec_truss"]), ("Ocellus Starport", ["ocellus"]),
        ("Orbis Starport", ["apollo", "artemis"])]) + [T1_ANY],
    "port-surface": kinds([
        ("Civilian Surface Outpost", ["atropos", "clotho", "decima", "hestia", "lachesis", "nona"]),
        ("Industrial Surface Outpost", ["bia", "hephaestus", "mefitis", "opis", "ponos", "tethys"]),
        ("Scientific Surface Outpost", ["ananke", "antevorta", "fauna", "necessitas", "porrima", "providentia"]),
        ("Large Planetary Port", ["aphrodite", "hera", "poseidon", "zeus"])]),
    "construction-orbital": ["Orbital Construction Site"],
    "carrier": ["Fleet Carrier", "Squadron Carrier", "Colonisation Ship"],
    "installation": ["Satellite Installation", "Communication Installation", "Space Farm", "Pirate Base Installation",
                     "Mining/Industrial Installation", "Relay Installation", "Military Installation",
                     "Security Installation", "Government Installation", "Medical Installation", "Research Installation",
                     "Tourist Installation", "Space Bar Installation", "Extraction Hub", "Civilian Hub", "Exploration Hub",
                     "Outpost Hub", "Scientific Hub", "Military Hub", "Refinery Hub", "High Tech Hub", "Industrial Hub"],
}
MAX_BODY = 4 << 20

# journal events around a sample: (event, seconds before the sample, seconds after) and how each is told
EVENTS = {
    "Loadout": (10, 2, lambda e: f"Loadout {e.get('Ship')}"),
    "DockingRequested": (120, 2, lambda e: f"DockingRequested {e.get('StationName')} ({e.get('StationType')}, pads "
                                           + ", ".join(f"{k} {v}" for k, v in (e.get('LandingPads') or {}).items()) + ")"),
    "DockingGranted": (120, 2, lambda e: f"DockingGranted pad {e.get('LandingPad')} at {e.get('StationName')}"),
    "Docked": (120, 60, lambda e: f"Docked {e.get('StationName')} ({e.get('StationType')})"),
    "Undocked": (10, 10, lambda e: f"Undocked {e.get('StationName')}"),
    "SupercruiseExit": (60, 2, lambda e: f"SupercruiseExit at {e.get('Body')} ({e.get('BodyType')})"),
    "ApproachSettlement": (60, 2, lambda e: f"ApproachSettlement {e.get('Name')}"),
    "Touchdown": (2, 30, lambda e: "Touchdown"),
    "Liftoff": (10, 10, lambda e: "Liftoff"),
    "LoadGame": (60, 2, lambda e: f"LoadGame {e.get('Ship')}"),
    "BookTaxi": (120, 2, lambda e: f"BookTaxi to {e.get('DestinationSystem')} {e.get('DestinationLocation') or ''}".rstrip()),
    "Embark": (10, 10, lambda e: "Embark" + (" taxi" if e.get("Taxi") else " SRV" if e.get("SRV") else "")
                                 + f" at {e.get('StationName') or e.get('Body')}"),
    "Disembark": (10, 10, lambda e: "Disembark" + (" taxi" if e.get("Taxi") else "") + f" at {e.get('StationName') or e.get('Body')}"),
}
# which event says most of what a hologram shows, for the description's draft
TELLING = ("Loadout", "Embark", "Disembark", "DockingGranted", "Docked", "Undocked", "DockingRequested", "Touchdown", "Liftoff", "ApproachSettlement",
           "SupercruiseExit", "target", "BookTaxi")


def decode_pos(x, y, z):
    """a vertex's local position from its first three words (panel_math.h decode_local, EDVR's edvrDecodePos)"""
    fmt = (z >> 24) & 0x7F
    if fmt == 64:
        lx, hx, ly, hy = x & 0xFFFF, x >> 16, y & 0xFFFF, y >> 16
        s = 2.0 ** (hy * 0.000244) - 1.0
        return tuple((v * 0.000031 - 1.0) * s for v in (lx, hx, ly))
    sh = (z >> 24) & 31
    scl = 1.0 / ((1 << ((20 - fmt) & 31)) - 1)
    offs = float(1 << sh)
    xb = x & 0x1FFFFF
    yb = (x >> 21) + ((y & 0x3FF) << 11)
    zb = (y >> 10) & 0x1FFFFF
    return (xb * scl - offs, yb * scl - offs, zb * scl - offs)


def read_mesh(ib_path, vb_path):
    """(positions by index, triangles as index triples); None when an index reaches past the vertices written"""
    ib, vb = ib_path.read_bytes(), vb_path.read_bytes()
    size = 2 if ib_path.name.endswith("_ib_u16.bin") else 4
    indices = struct.unpack(f"<{len(ib) // size}{'H' if size == 2 else 'I'}", ib[: len(ib) // size * size])
    have = len(vb) // VERTEX_STRIDE
    if not indices or max(indices) >= have:
        return None
    pos = {}
    for i in set(indices):
        pos[i] = decode_pos(*struct.unpack_from("<3I", vb, i * VERTEX_STRIDE))
    tris = [indices[k:k + 3] for k in range(0, len(indices) - 2, 3)]
    return pos, tris


def mesh_key(pos, tris):
    """the model across builds: its vertices, triangles and the box's half-sides over the largest"""
    lo = [min(p[k] for p in pos.values()) for k in range(3)]
    hi = [max(p[k] for p in pos.values()) for k in range(3)]
    half = [(hi[k] - lo[k]) / 2 for k in range(3)]
    top = max(half) or 1.0
    extent = [round(h / top, 2) for h in half]
    return f"v{len(pos)}-t{len(tris)}-e{extent[0]:.2f},{extent[1]:.2f},{extent[2]:.2f}", extent


def png(path, width, height, rows):
    """an 8-bit grey PNG; rows are bytes of width each"""
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    tmp = path.with_suffix(".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def png_black(path):
    """a PNG of png() all black (its rows' filter bytes are 0 too): the hologram's surface copied while it was not on
    screen (a taxi, 2026-10-22); False when it cannot be read"""
    try:
        data = Path(path).read_bytes()
        at = data.index(b"IDAT")
        size = struct.unpack(">I", data[at - 4:at])[0]
        return not any(zlib.decompress(data[at + 4:at + 4 + size]))
    except (OSError, ValueError, zlib.error, struct.error):
        return False


def pgm_to_png(src, dst):
    data = src.read_bytes()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m or int(m.group(3)) > 255:
        return False
    w, h = int(m.group(1)), int(m.group(2))
    pixels = data[m.end():m.end() + w * h]
    if len(pixels) != w * h:
        return False
    png(dst, w, h, [pixels[r * w:(r + 1) * w] for r in range(h)])
    return True


def render_views(pos, tris, dst):
    """top (looking down -Y), front (along -Z), side (along -X), side by side; nearer faces lighter, edge-on darker"""
    lo = [min(p[k] for p in pos.values()) for k in range(3)]
    hi = [max(p[k] for p in pos.values()) for k in range(3)]
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    scale = (VIEW - 8) / (max(hi[k] - lo[k] for k in range(3)) or 1.0)
    # (screen right axis, screen up axis, towards the viewer axis, its sign)
    views = ((0, 2, 1, 1.0), (0, 1, 2, 1.0), (2, 1, 0, -1.0))
    image = [bytearray(VIEW * 3) for _ in range(VIEW)]
    for n, (ax, ay, az, sign) in enumerate(views):
        depth = [[-1e30] * VIEW for _ in range(VIEW)]
        x0 = n * VIEW
        for t in tris:
            a, b, c = pos[t[0]], pos[t[1]], pos[t[2]]
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            nrm = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            length = (nrm[0] ** 2 + nrm[1] ** 2 + nrm[2] ** 2) ** 0.5
            if length == 0:
                continue
            shade = int(60 + 195 * abs(nrm[az]) / length)
            pts = [((p[ax] - centre[ax]) * scale * (sign if ax != 1 else 1) + VIEW / 2,
                    VIEW / 2 - (p[ay] - centre[ay]) * scale, (p[az] - centre[az]) * sign) for p in (a, b, c)]
            (px, py, pz), (qx, qy, qz), (rx, ry, rz) = pts
            area = (qx - px) * (ry - py) - (qy - py) * (rx - px)
            xmin, xmax = max(int(min(px, qx, rx)), 0), min(int(max(px, qx, rx)) + 1, VIEW - 1)
            ymin, ymax = max(int(min(py, qy, ry)), 0), min(int(max(py, qy, ry)) + 1, VIEW - 1)
            if abs(area) < 1e-9:
                for (sx, sy, sz) in pts:  # edge-on: its corners only
                    ix, iy = int(sx), int(sy)
                    if 0 <= ix < VIEW and 0 <= iy < VIEW and sz > depth[iy][ix]:
                        depth[iy][ix] = sz
                        image[iy][x0 + ix] = 60
                continue
            for y in range(ymin, ymax + 1):
                cy = y + 0.5
                for x in range(xmin, xmax + 1):
                    cx = x + 0.5
                    w0 = ((qx - cx) * (ry - cy) - (qy - cy) * (rx - cx)) / area
                    w1 = ((rx - cx) * (py - cy) - (ry - cy) * (px - cx)) / area
                    w2 = 1.0 - w0 - w1
                    if w0 < 0 or w1 < 0 or w2 < 0:
                        continue
                    z = w0 * pz + w1 * qz + w2 * rz
                    if z > depth[y][x]:
                        depth[y][x] = z
                        image[y][x0 + x] = shade
    for y in range(VIEW):  # a line between the views
        image[y][VIEW - 1] = image[y][2 * VIEW - 1] = 40
    png(dst, VIEW * 3, VIEW, image)


def registry():
    """holo_models.h's models: {(build, count): (name, role, status)}"""
    text = HEADER.read_text(encoding="utf-8")
    builds = dict(re.findall(r'inline constexpr char (holo_build_\w+)\[\]\{"([^"]+)"\}', text))
    rows = {}
    for name, role, count, status, build in re.findall(
            r'\{"([^"]+)", holo_role_e::(\w+), (\d+)u, holo_status_e::(\w+), (\w+|"[^"]*")', text):
        key = builds.get(build, build.strip('"'))
        rows[(key, int(count))] = (name, role, status)
    return rows


def stamp_time(stamp):
    return datetime.strptime(stamp, "%Y%m%dT%H%M%S_%fZ").replace(tzinfo=timezone.utc)


def read_journals(directory, days):
    """the events of EVENTS from the journals of the days around the samples, by time"""
    out = []
    if not directory:
        return out
    for f in sorted(Path(directory).glob("Journal.*.log")):
        m = re.match(r"Journal\.(\d{4}-\d{2}-\d{2})T", f.name)
        if not m or m.group(1) not in days:
            continue
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("event") in EVENTS and "timestamp" in e:
                out.append((datetime.strptime(e["timestamp"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc), e))
    out.sort(key=lambda p: p[0])
    return out


def nearest(events, when):
    """the nearest of each kind of event within its window around the sample, (seconds from it, event) in time order"""
    best = {}
    for t, e in events:
        before, after, _ = EVENTS[e["event"]]
        d = (t - when).total_seconds()
        if -before <= d <= after and (e["event"] not in best or abs(d) < abs(best[e["event"]][0])):
            best[e["event"]] = (d, e)
    return sorted(best.values(), key=lambda p: p[0])


def told_events(near):
    return [f"{EVENTS[e['event']][2](e)} ({abs(d):.0f} s {'before' if d <= 0 else 'after'})" for d, e in near]


def hints(events, when):
    return told_events(nearest(events, when))


# the fields of the events near a sample the deductions read
KEEP = ("Ship", "StationName", "StationType", "LandingPad", "LandingPads", "Taxi", "SRV", "Body", "BodyType", "Name")


def deductions(row):
    """what the samples of one model say together, each with how many it stands on; one is weak"""
    seen = [s for s in row["sightings"] if s.get("stamp")]
    n = len(seen)
    out = []

    def weak(k):
        return " - weak, one sample" if k == 1 else ""

    builds = {}
    for s in row["sightings"]:
        builds.setdefault(s["build"], set()).add(s["count"])
    if len(builds) > 1:
        counts = sorted({c for cs in builds.values() for c in cs})
        out.append(f"on {len(builds)} builds of the game: " + ("the same count, the id kept" if len(counts) == 1
                                                                else f"counts {counts}, renumbered"))
    near = [s.get("near") or {} for s in seen]
    loaded = sum(1 for x in near if "LoadGame" in x)
    if loaded:
        out.append(f"at the game's load: {loaded} of {n} sample(s), whatever was shown then{weak(loaded)}")
    ships = {}
    for x in near:
        if "Loadout" in x and "LoadGame" not in x:  # a Loadout at the load is no change of ship
            ships[x["Loadout"].get("Ship")] = ships.get(x["Loadout"].get("Ship"), 0) + 1
    for ship, k in sorted(ships.items(), key=lambda p: str(p[0])):
        out.append(f"right after Loadout {ship}: {k} of {n} sample(s), the own ship's model{weak(k)}")
    taxi = sum(1 for x in near if (x.get("Embark") or {}).get("Taxi"))
    if taxi:
        out.append(f"at boarding a taxi: {taxi} of {n} sample(s){weak(taxi)}")
    # a station the sample belongs to: asked for or granted before it, not docked yet
    stations = {}
    for x in near:
        if (x.get("Docked") or {}).get("dt", 1) <= 0:
            continue
        for ev in ("DockingGranted", "DockingRequested"):
            if (x.get(ev) or {}).get("StationName"):
                name = x[ev]["StationName"]
                stations[name] = (x.get("DockingRequested") or {}).get("StationType") or stations.get(name)
                break
    types = sorted({str(v) for v in stations.values() if v})
    if len(stations) >= 2 and len(types) == 1:
        out.append(f"the same model at {len(stations)} stations, all {types[0]}: the type's model, not one station's")
    elif len(stations) == 1:
        (name, kind), = stations.items()
        out.append(f"at one station only ({name}, {kind}): the station's or its type's, not told yet")
    elif stations:
        out.append(f"at {len(stations)} stations of {len(types)} types ({', '.join(types)})")
    pads = sorted({(x["DockingGranted"].get("StationName"), x["DockingGranted"].get("LandingPad"),
                    (x.get("Loadout") or {}).get("Ship")) for x in near
                   if "DockingGranted" in x and (x.get("Docked") or {}).get("dt", 1) > 0}, key=str)
    if pads:
        out.append("pads granted: " + "; ".join(f"pad {p} at {st}" for st, p, _ in pads))
    docked = sum(1 for x in near if "DockingGranted" in x and (x.get("Docked") or {}).get("dt", -1) > 0)
    if docked:
        out.append(f"between the grant and Docked: {docked} of {n} sample(s){weak(docked)}")
    left = sorted({str(x["Undocked"].get("StationName")) for x in near if "Undocked" in x})
    undocked = sum(1 for x in near if "Undocked" in x)
    if undocked:
        out.append(f"at Undocked (from {', '.join(left)}): {undocked} of {n} sample(s){weak(undocked)}")
    placed = [s["place"] for s in seen if s.get("place")]
    for when in ("before", "after"):
        kinds = {}
        for p in placed:
            kinds[p[when]["where"]] = kinds.get(p[when]["where"], 0) + 1
        for kind, k in sorted(kinds.items()):
            out.append(f"{'at the sample' if when == 'before' else 'a minute later'}: {kind}, {k} of {len(placed)} "
                       f"placed sample(s){weak(k)}")
    targets = sorted({s["context"]["target"] for s in seen if (s.get("context") or {}).get("target")})
    if targets:
        out.append("target chosen: " + "; ".join(targets))
    return out


def read_context(path):
    """edworld_eht's context of a sample (sample_context.h): the journal's place and time, and what Status.json said -
    the target chosen (Destination) and the body; None without one"""
    try:
        c = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    status, journal = c.get("status") or {}, c.get("journal") or {}
    last = journal.get("last_line") or {}
    dest = status.get("Destination") or {}
    out = {"journal": journal.get("file"), "offset": journal.get("size"), "last_event": last.get("event"),
           "last_time": last.get("timestamp"), "target": dest.get("Name_Localised") or dest.get("Name"),
           "target_body": dest.get("Body"), "target_system": dest.get("System"), "body": status.get("BodyName"),
           "flags": status.get("Flags"), "flags2": status.get("Flags2")}
    return {k: v for k, v in out.items() if v is not None}


def hints_at_place(journal_dir, ctx):
    """the events around the sample by its place in the journal (the context's byte): before it what came before, from
    it what came after, the times on the journal's own clock from its last event before the sample; None when the
    journal is not at hand"""
    if not journal_dir or not ctx.get("journal") or ctx.get("offset") is None or not ctx.get("last_time"):
        return None
    path = Path(journal_dir) / ctx["journal"]
    if not path.is_file():
        return None
    anchor = datetime.strptime(ctx["last_time"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    events, at = [], 0
    for line in path.read_bytes().splitlines(keepends=True):
        start, at = at, at + len(line)
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if e.get("event") not in EVENTS or "timestamp" not in e:
            continue
        t = datetime.strptime(e["timestamp"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        after = start >= ctx["offset"]
        # an event after the place is at least a moment after the sample, one before it at most at the anchor's time
        events.append((max(t, anchor + timedelta(seconds=1)) if after else min(t, anchor), e))
    return nearest(events, anchor + timedelta(milliseconds=500))


def context_hints(ctx):
    told = []
    if ctx.get("target"):
        told.append(f"target {ctx['target']}")
    if ctx.get("body"):
        told.append(f"near {ctx['body']}")
    if ctx.get("journal"):
        told.append(f"journal {ctx['journal']} at byte {ctx.get('offset')}, last {ctx.get('last_event')} {ctx.get('last_time')}")
    return told


def pad_planes():
    """hologram_math.h's pad_planes: {model: (up_axis, about_axis, towards_axis, [angles])}"""
    text = PLANES_HEADER.read_text(encoding="utf-8")
    block = text[text.index("pad_planes_t pad_planes[]"):]
    block = block[:block.index("};")]
    out = {}
    for model, up, about, towards, n, angles in re.findall(r"\{(\d+)u, (\d), (\d), (\d), (\d+)u, \{([^}]*)\}", block):
        out[int(model)] = (int(up), int(about), int(towards), [float(a.rstrip("f")) for a in angles.split(",")][: int(n)])
    return out


def log_time(text):
    return datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%f").replace(tzinfo=timezone.utc)


def dockings(journal_dir, days):
    """(Docked's time, the grant's time, pad, StationName, StationType, the station's pads as S/M/L counts, the ship) of
    the journals of these days"""
    out, grants, layouts, ship = [], {}, {}, ""
    for f in sorted(Path(journal_dir).glob("Journal.*.log")):
        m = re.match(r"Journal\.(\d{4}-\d{2}-\d{2})T", f.name)
        if not m or m.group(1) not in days:
            continue
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            if not any(k in line for k in ('"DockingGranted"', '"Docked"', '"DockingRequested"', '"Loadout"', '"LoadGame"')):
                continue
            try:
                e = json.loads(line)
                t = datetime.strptime(e["timestamp"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
            except (ValueError, KeyError):
                continue
            if e.get("event") in ("Loadout", "LoadGame") and e.get("Ship"):
                ship = e["Ship"].lower()
            elif e.get("event") == "DockingRequested":
                p = e.get("LandingPads") or {}
                layouts[e.get("MarketID")] = f"S{p.get('Small', 0)} M{p.get('Medium', 0)} L{p.get('Large', 0)}" if p else ""
            elif e.get("event") == "DockingGranted":
                grants[e.get("MarketID")] = (t, e.get("LandingPad"))
            elif e.get("event") == "Docked" and not e.get("Taxi") and e.get("MarketID") in grants:
                gt, pad = grants.pop(e["MarketID"])
                if pad and (t - gt).total_seconds() < 900:
                    out.append((t, gt, int(pad), e.get("StationName") or "", e.get("StationType") or "",
                                layouts.get(e["MarketID"], ""), ship))
    return out


def read_log(path, models):
    """an edworld_eht log's grants told ([(time, pad)]) and the hologram's axes of `models` ([(time, model, up, nose)]):
    the model's X, Y, Z in the hologram camera's frame, so the ship's up in the model's axes is their second words and its
    nose their third"""
    grants, axes = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if " state: pad " in line:
                g = LOG_GRANT.match(line)
                if g:
                    grants.append((log_time(g.group(1)), int(g.group(2))))
            elif " indices, scale " in line:
                a = LOG_AXES.match(line)
                if a and int(a.group(2)) in models:
                    x, y, z = ([float(v) for v in a.group(k, k + 1, k + 2)] for k in (3, 6, 9))
                    axes.append((log_time(a.group(1)), int(a.group(2)), (x[1], y[1], z[1]), (x[2], y[2], z[2])))
    return grants, axes


def log_samples(logs_dir):
    """from edworld_eht's logs: {(build, count): the fingerprint as holo_models.h writes it} - each "hologram model
    fingerprint" line right after the sample's line that names the build - and [(build, count, time, surface)] of each
    sample's line, the surface "target", "own" or "other" (2026-10-24: the parts of a station's hologram)"""
    out, surfaces = {}, []
    logs = sorted(f for f in Path(logs_dir).iterdir() if LOG.match(f.name)) if Path(logs_dir).is_dir() else []
    for f in logs:
        sample = None
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if " hologram model " not in line:
                    continue
                s = LOG_SAMPLE.match(line)
                if s:
                    sample = (s.group(4), int(s.group(2)))
                    where = "target" if s.group(3).startswith("target") else "own" if s.group(3).startswith("own") else "other"
                    try:
                        surfaces.append((s.group(4), int(s.group(2)), log_time(s.group(1)), where))
                    except ValueError:
                        pass
                    continue
                m = LOG_FINGERPRINT.match(line)
                if m and sample and sample[1] == int(m.group(1)):
                    out[sample] = m.group(2)
                sample = None
    return out, surfaces


def pad_looks(logs_dir, journal_dir, models):
    """the looks at docked pads in edworld_eht's logs: {(model, station, pad): [look]}; a docking counts only where the
    log told the same pad given at the grant - the log is this account's - and the hologram showed a model of `models`"""
    logs = sorted(f for f in Path(logs_dir).iterdir() if LOG.match(f.name)) if Path(logs_dir).is_dir() else []
    if not logs or not journal_dir:
        return {}
    spans = []
    for f in logs:  # a log's name tells when it was set aside, not begun: its first line tells the start
        with open(f, encoding="utf-8", errors="replace") as fh:
            first = re.match(r"^(\S+)Z ", fh.readline())
        end = datetime.fromtimestamp(f.stat().st_mtime, timezone.utc)
        try:
            start = log_time(first.group(1)) if first else end
        except ValueError:
            start = end
        spans.append((start, end, f))
    days = {(t + timedelta(days=d)).strftime("%Y-%m-%d") for a, b, _ in spans for t in (a, b) for d in (-1, 0, 1)}
    found, parsed = {}, {}
    for docked, granted, pad, station, kind, layout, ship in dockings(journal_dir, days):
        for start, end, f in spans:
            if not start <= docked <= end + timedelta(seconds=PAD_LOOK_S[1]):
                continue
            if f not in parsed:
                parsed[f] = read_log(f, models)
            grants_told, axes = parsed[f]
            told = any(p == pad and PAD_GRANT_S[0] <= (t - granted).total_seconds() <= PAD_GRANT_S[1] for t, p in grants_told)
            # the first model after Docked and its looks until another shows: a target chosen next (a carrier after a
            # construction site, 2026-10-23) is no look at this pad
            model, seen = None, []
            for t, m, up, nose in axes:
                if PAD_LOOK_S[0] <= (t - docked).total_seconds() <= PAD_LOOK_S[1]:
                    if model is not None and m != model:
                        break
                    model = m
                    seen.append((up, nose))
            if told and model is not None:
                if len(seen) >= 2:
                    up, nose = seen[-1]
                    found.setdefault((model, station, pad), []).append(
                        {"docked": docked.strftime("%Y-%m-%dT%H:%M:%SZ"), "type": kind, "layout": layout, "ship": ship,
                         "ship_pad": SHIP_PAD.get(ship, "?"), "up": [round(v, 3) for v in up],
                         "nose": [round(v, 3) for v in nose], "log": f.name})
            break
    return found


def unit(v):
    n = sum(c * c for c in v) ** 0.5 or 1.0
    return [c / n for c in v]


def angle_between(a, b):
    a, b = unit(a), unit(b)
    return math.degrees(math.acos(max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))))


def look_plane(look, model, planes):
    """the model's plane a look's up lies on (within PLANE_SNAP_DEG), else None; None for a model without planes"""
    if model not in planes:
        return None
    up_axis, _, towards, angles = planes[model]
    measured = math.degrees(math.atan2(look["up"][towards], look["up"][up_axis]))
    best = min(angles, key=lambda a: abs(a - measured))
    return best if abs(best - measured) <= PLANE_SNAP_DEG else None


def kept_looks(row, planes):
    """the looks a pad stands on, and what was left out: the dockings off the model's planes, then of the noses that
    differ the most dockings' way (none when two ways tie)"""
    on = [l for l in row["looks"] if look_plane(l, row["model"], planes) is not None]
    off = len(row["looks"]) - len(on)
    groups = []
    for l in on:
        group = next((g for g in groups if angle_between(g[0]["nose"], l["nose"]) <= NOSE_ALIKE_DEG), None)
        if group is None:
            groups.append([l])
        else:
            group.append(l)
    groups.sort(key=len, reverse=True)
    if len(groups) >= 2 and len(groups[0]) == len(groups[1]):
        return [], off, len(on)
    return (groups[0] if groups else []), off, len(on) - (len(groups[0]) if groups else 0)


def pad_summary(row, planes, kept=None):
    """what a pad's looks say: the up and nose put together, their spread, the model's plane and the nose's end; from the
    looks kept (kept_looks), or from all when none is kept"""
    looks = kept or row["looks"]
    up = unit([sum(l["up"][k] for l in looks) for k in range(3)])
    nose = unit([sum(l["nose"][k] for l in looks) for k in range(3)])
    spread = max(max(angle_between(l["up"], up), angle_between(l["nose"], nose)) for l in looks)
    along = sum(a * b for a, b in zip(nose, up))  # the nose in the pad's plane: the hologram's camera looks a little down
    nose = unit([n - along * u for n, u in zip(nose, up)])
    out = {"up": [round(v, 3) for v in up], "nose": [round(v, 3) for v in nose], "spread_deg": round(spread, 1),
           "plane_deg": None, "measured_deg": None}
    if row["model"] in planes:
        up_axis, about, towards, angles = planes[row["model"]]
        measured = math.degrees(math.atan2(up[towards], up[up_axis]))
        out["measured_deg"] = round(measured, 1)
        best = min(angles, key=lambda a: abs(a - measured))
        if abs(best - measured) <= PLANE_SNAP_DEG:
            out["plane_deg"] = best
    return out


def pad_header_ready(row):
    return row.get("plane_deg") is not None


def confirmed(r):
    """confirmed in the form, or with another part of the same station's hologram (siblings)"""
    return r["confirmed"] or bool(r.get("confirmed_with"))


def docked(ctx):
    """whether a sample's Status.json said docked (Flags bit 0)"""
    return bool(ctx and (ctx.get("flags") or 0) & 1)


def docked_only(row):
    """a row whose every sample with Status.json was taken docked: what the HUD showed there means nothing - but for a
    ship's own hologram (a ship swapped at the shipyard, a taxi before it leaves), which the form does not mark"""
    flagged = [s for s in row["sightings"] if (s.get("context") or {}).get("flags") is not None]
    return bool(flagged) and len(flagged) == len(row["sightings"]) and all(docked(s["context"]) for s in flagged)


def same_moment(rows, key, stamp, seconds=5):
    """the other rows with a picture taken within `seconds` of `stamp`: parts of the one hologram drawn then, whose
    game's pictures are the same (Artur 2026-10-24: three parts at Brescia Platform within a second, one picture)"""
    at = stamp_time(stamp)
    return sorted(k for k, r in rows.items() if k != key and any(
        s.get("picture") and s.get("stamp") and abs((stamp_time(s["stamp"]) - at).total_seconds()) <= seconds
        for s in r["sightings"]))


def station_visits(rows):
    """the parts of one station's hologram (2026-10-24): the samples drawn to the target's surface, the same target named
    by Status.json, each within 5 s of the last, none taken docked - [{"target", "keys"}], in time"""
    items = sorted((stamp_time(s["stamp"]), s["context"]["target"], k) for k, r in rows.items() for s in r["sightings"]
                   if s.get("stamp") and s.get("surface") == "target" and (s.get("context") or {}).get("target")
                   and not docked(s["context"]))
    visits = []
    for when, target, k in items:
        if visits and visits[-1]["target"] == target and (when - visits[-1]["last"]).total_seconds() <= 5:
            visits[-1]["keys"].add(k)
            visits[-1]["last"] = when
        else:
            visits.append({"target": target, "keys": {k}, "last": when})
    return visits


def siblings(rows):
    """a part not confirmed, its role and type unset (and not the person's since), takes the role and type of a part of
    the same station's hologram confirmed in the form, and counts as confirmed with it (`confirmed_with`) - one
    confirmation a station (Artur 2026-10-24); parts confirmed otherwise at one visit tell nothing, nor a part of a role
    no station has (the own ship after a swap, the planet before undocking elsewhere: drawn to the target's surface)"""
    for visit in station_visits(rows):
        sources = {(rows[k]["role"], rows[k].get("type", "")): k for k in sorted(visit["keys"])
                   if rows[k]["confirmed"] and rows[k]["role"] in STATION_ROLES}
        if len(sources) != 1:
            continue
        (role, kind), source = next(iter(sources.items()))
        for k in visit["keys"]:
            r = rows[k]
            if r["confirmed"] or r["role"] or r.get("type") or "suggested_from" in r or r.get("confirmed_with"):
                continue
            r["role"], r["type"], r["confirmed_with"] = role, kind, source


def model_roles(data):
    """{count: role} of the models confirmed in the form, by the counts they were seen with on any build"""
    out = {}
    for r in data["models"].values():
        if confirmed(r):
            for s in r["sightings"]:
                out.setdefault(s["count"], r["role"])
    return out


def site_kind(name):
    """hologram_math.h's site_kind_of: a construction site by the ": " in its name, a key of the game ("$...") none"""
    if not name or name.startswith("$"):
        return "unknown"
    return "construction_site" if ": " in name else "other"


KIND_TITLES = {"construction_site": "construction sites", "other": "stations and settlements", "unknown": "unknown places"}


def kind_rules(pads):
    """{(model, kind, pad): rule} from the pads for edworld_eht (usable): alike when two stations or more of the kind have the
    pad on the same plane with noses within PLANE_SNAP_DEG of each other - then one row for any station of the kind"""
    groups = {}
    for k in sorted(pads):
        p = pads[k]
        kind = site_kind(p["station"])
        if p.get("usable") and kind != "unknown":
            groups.setdefault((p["model"], kind, p["pad"]), []).append(p)
    rules = {}
    for key, rows in groups.items():
        alike = (len(rows) >= 2 and len({r["plane_deg"] for r in rows}) == 1
                 and all(angle_between(a["nose"], b["nose"]) <= PLANE_SNAP_DEG for a in rows for b in rows))
        nose = unit([sum(r["nose"][i] for r in rows) for i in range(3)])
        rules[key] = {"stations": [r["station"] for r in rows], "alike": alike, "plane_deg": rows[0]["plane_deg"],
                      "nose": [round(v, 3) for v in nose], "looks": sum(len(r["looks"]) for r in rows)}
    return rules


def merge_pads(data, found, planes):
    """new looks into data["pads"]; each row's looks kept, summary, notes and whether it goes to edworld_eht (usable) again:
    its model confirmed with a role of DOCK_ROLES, a plane of the model's, a nose the most dockings agree on"""
    pads = data.setdefault("pads", {})
    roles = model_roles(data)
    for (model, station, pad), looks in found.items():
        row = pads.setdefault(f"{model}|{station}|{pad}", {"model": model, "station": station, "pad": pad, "looks": []})
        have = {l["docked"]: l for l in row["looks"]}
        for l in looks:  # a look read before the layout and the ship were kept gets them
            if l["docked"] in have:
                for k in ("layout", "ship", "ship_pad"):
                    have[l["docked"]].setdefault(k, l[k])
        row["looks"] += [l for l in looks if l["docked"] not in have]
        row["looks"].sort(key=lambda l: l["docked"])
    by_station = {}
    for row in pads.values():
        row.pop("confirmed", None)  # by hand until 2026-10-24
        kept, off, other_way = kept_looks(row, planes)
        row.update(pad_summary(row, planes, kept))
        row["kept"] = len(kept)
        row["type"] = row["looks"][-1]["type"] if row["looks"] else ""
        row["layout"] = next((l["layout"] for l in reversed(row["looks"]) if l.get("layout")), "")
        row["ships"] = sorted({f"{l['ship']} ({l['ship_pad']})" for l in row["looks"] if l.get("ship")})
        row["notes"] = []
        role = roles.get(row["model"])
        if len(row["looks"]) == 1:
            row["notes"].append("one docking only - weak")
        if row["model"] not in planes:
            row["notes"].append("a model without pad_planes in hologram_math.h - not for the header")
        elif row["plane_deg"] is None:
            row["notes"].append(f"measured {row['measured_deg']} degrees: on none of the model's planes")
        if off:
            row["notes"].append(f"{off} docking(s) off the model's planes left out")
        if other_way:
            row["notes"].append(f"{other_way} docking(s) with the nose another way left out" if kept
                                else "the dockings' noses split evenly: left out until another docking tells")
        if role is None:
            row["notes"].append("its model not confirmed in the form yet: waits")
        elif role not in DOCK_ROLES:
            row["notes"].append(f"its model confirmed as {role or 'no role'}: not a place to dock, left out")
        row["usable"] = bool(kept) and row["plane_deg"] is not None and role in DOCK_ROLES
        if row["spread_deg"] > PLANE_SNAP_DEG:
            row["notes"].append(f"the dockings differ by up to {row['spread_deg']} degrees")
        by_station.setdefault((row["model"], row["station"]), []).append(row)
    for rows in by_station.values():
        if len(rows) >= 3 and all(angle_between(r["up"], rows[0]["up"]) < 1 and angle_between(r["nose"], rows[0]["nose"]) < 1
                                  for r in rows):
            for r in rows:
                r["notes"].append(f"all {len(rows)} pads of this station read alike: the pads parallel, or the "
                                  "hologram's own pose rather than the pad")


def load(db):
    path = db / "models.json"
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    return {"format": 1, "models": {}}


def save(db, data):
    """the file whole or not at all; the previous one kept as models.json.bak"""
    path = db / "models.json"
    if path.exists():
        shutil.copy2(path, db / "models.json.bak")
    fd, tmp = tempfile.mkstemp(dir=db, prefix="models.", suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)


def has_picture(row):
    return bool(row.get("views")) or any(s.get("picture") for s in row["sightings"])


def journal_place(tool, journal_dir, ctx):
    """EHT's journal_context at the sample's place in the journal: the place before it, after it and the steps
    between; None without the tool, the journal or a place"""
    if not tool or not journal_dir or not ctx.get("journal") or ctx.get("offset") is None:
        return None
    path = Path(journal_dir) / ctx["journal"]
    if not path.is_file():
        return None
    try:
        run = subprocess.run([str(tool), str(path), str(ctx["offset"]), "60"], capture_output=True, text=True,
                             timeout=30, check=False)
        return json.loads(run.stdout) if run.returncode == 0 else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def place_hints(place):
    def where(p):
        at = p.get("station") or p.get("settlement") or p.get("body") or p.get("system") or "?"
        return f"{p.get('where')} at {at}" + (f", pad {p['pad']}" if p.get("pad") else "")
    told = [f"before: {where(place['before'])}"]
    told += [f"then {s['event']} {s['what']} (+{s['seconds']} s)" for s in place.get("steps", [])[:4]]
    told.append(f"after a minute: {where(place['after'])}")
    return told


def scan(models_dir, db, journal_dir=None, journal_tool=None):
    models_dir, db = Path(models_dir), Path(db)
    (db / "png").mkdir(parents=True, exist_ok=True)
    data = load(db)
    rows = data["models"]
    known = {(s["build"], s["stamp"], s["count"]) for r in rows.values() for s in r["sightings"]}
    docked_left = data.setdefault("docked_left_out", [])
    known |= {tuple(k) for k in docked_left}
    samples = {}
    for f in models_dir.iterdir():
        m = SAMPLE.match(f.name)
        if m:
            samples.setdefault((m.group(1), m.group(2), int(m.group(3))), {})[m.group(4)] = f
    # docked samples last: a model's row made by a sample not docked keeps them, whatever the order of builds
    fresh = sorted((k for k in samples if k not in known),
                   key=lambda k: (docked(read_context(samples[k]["context.json"]) if "context.json" in samples[k] else None), k))
    days = set()
    stamps = [k[1] for k in fresh] + [s["stamp"] for r in rows.values() for s in r["sightings"] if s.get("stamp")]
    for stamp in stamps:
        t = stamp_time(stamp)
        days |= {(t + timedelta(days=d)).strftime("%Y-%m-%d") for d in (-1, 0, 1)}
    events = read_journals(journal_dir, days)
    reg = registry()
    added = 0
    for build, stamp, count in fresh:
        files = samples[(build, stamp, count)]
        ctx = read_context(files["context.json"]) if "context.json" in files else None
        ib = files.get("ib_u16.bin") or files.get("ib_u32.bin")
        vb = files.get("vb_s40.bin")
        mesh = read_mesh(ib, vb) if ib and vb else None
        if mesh:
            key, extent = mesh_key(*mesh)
        else:
            key, extent = f"b{build}-c{count}", None
        # docked, the HUD's hologram tells nothing (Artur 2026-10-24: stars, the model of every T1, even with the station
        # one is docked at chosen): a docked sample makes no row; a row of the model keeps it
        if docked(ctx) and key not in rows:
            docked_left.append([build, stamp, count])
            continue
        row = rows.setdefault(key, {"description": "", "description_by": "scan", "role": "", "confirmed": False,
                                    "registry": None, "vertices": len(mesh[0]) if mesh else None,
                                    "triangles": len(mesh[1]) if mesh else None, "extent": extent, "views": None,
                                    "sightings": []})
        stem = f"{build}_{stamp}_{count}"
        picture = None
        if "holo.pgm" in files and pgm_to_png(files["holo.pgm"], db / "png" / f"{stem}_game.png"):
            picture = f"png/{stem}_game.png"
        if mesh and not row["views"]:
            name = re.sub(r"[^0-9a-zA-Z.,-]", "_", key) + "_views.png"
            render_views(*mesh, db / "png" / name)
            row["views"] = f"png/{name}"
        sighting = {"build": build, "stamp": stamp, "count": count, "picture": picture, "hints": []}
        if ctx:
            sighting["context"] = ctx
        row["sightings"].append(sighting)
        row["sightings"].sort(key=lambda s: s["stamp"])
        if (build, count) in reg:
            row["registry"] = reg[(build, count)][0]
            if not row["role"]:
                row["role"] = reg[(build, count)][1]
        added += 1
    # the journal's events around every sample again (the events known may have grown), and what they say together
    for row in rows.values():
        for s in row["sightings"]:
            if not s.get("stamp"):
                continue
            ctx = s.get("context")
            near = hints_at_place(journal_dir, ctx) if ctx else None
            if near is None:
                near = nearest(events, stamp_time(s["stamp"]))
            place = journal_place(journal_tool, journal_dir, ctx) if ctx else None
            if place:
                s["place"] = place
            s["hints"] = (place_hints(place) if place else told_events(near)) + (context_hints(ctx) if ctx else [])
            s["near"] = {e["event"]: {**{k: e[k] for k in KEEP if k in e}, "dt": round(d, 1)} for d, e in near}
        row["deductions"] = deductions(row)
    # a registry model no sample shows yet: a row waiting for its picture; gone once a sample names it, or a model
    # confirmed with its count on another build stands for it (2026-10-24: the T1 stations after the update)
    named = {r["registry"] for k, r in rows.items() if not k.startswith("registry:") and r["registry"]}
    covered = {s["count"]: r for k, r in rows.items() if not k.startswith("registry:") and r["confirmed"]
               for s in r["sightings"]}
    for (build, count), (name, role, _) in reg.items():
        key = f"registry:{name}"
        if name in named or count in covered:
            placeholder = rows.pop(key, None)
            if placeholder and placeholder["description_by"] == "person":
                target = next((r for k, r in rows.items() if r["registry"] == name and not k.startswith("registry:")),
                              covered.get(count))
                if target["description_by"] != "person":
                    target["description"], target["description_by"] = placeholder["description"], "person"
        elif key not in rows:
            rows[key] = {"description": "", "description_by": "scan", "role": role, "confirmed": False, "registry": name,
                         "vertices": None, "triangles": None, "extent": None, "views": None,
                         "sightings": [{"build": build, "stamp": None, "count": count, "picture": None, "hints": []}]}
    for row in rows.values():  # a draft for the person: the registry's name, else the journal's telling event
        if row["description_by"] == "scan":
            told = [h for s in row["sightings"] for h in s["hints"]]
            told.sort(key=lambda h: next((n for n, e in enumerate(TELLING) if h.startswith(e + " ")), len(TELLING)))
            row["description"] = row["registry"] or (told[0] if told else "")
    suggest(rows)
    for row in rows.values():  # a picture read once for being all black
        for s in row["sightings"]:
            if s.get("picture") and "black" not in s:
                s["black"] = png_black(db / s["picture"])
    fingerprints, surfaces = log_samples(models_dir.parent)
    for row in rows.values():  # a fingerprint and a surface once read stay: the logs are set aside in time
        for s in row["sightings"]:
            fp = fingerprints.get((s["build"], s["count"]))
            if fp and (row["vertices"] is None or fp.startswith(f"{{{row['vertices']}u, ")):
                row.setdefault("fingerprints", {})[f"{s['build']}|{s['count']}"] = fp
            if s.get("stamp") and "surface" not in s:
                when = stamp_time(s["stamp"])
                s.update({"surface": where for b, c, t, where in surfaces
                          if b == s["build"] and c == s["count"] and abs((t - when).total_seconds()) <= 2})
    siblings(rows)
    planes = pad_planes()
    found = pad_looks(models_dir.parent, journal_dir, set(planes))
    merge_pads(data, found, planes)
    save(db, data)
    waiting = sum(1 for r in rows.values() if not has_picture(r))
    pads = data.get("pads", {})
    print(f"holo_db: {added} new sample(s), {len(rows)} model(s), {waiting} waiting for a picture; "
          f"{sum(len(v) for v in found.values())} docking look(s), {len(pads)} pad(s), "
          f"{sum(1 for p in pads.values() if p.get('usable'))} for edworld_eht -> {db / 'models.json'}")
    return data


STATION_ROLES = ("port-orbital", "port-surface", "settlement", "carrier", "construction-orbital")


def t1_counts():
    """the hologram models of hologram_math.h's T1 table: their parts carry the pads' table"""
    text = PLANES_HEADER.read_text(encoding="utf-8")
    block = text[text.index("t1_pad_t t1_pads[]"):]
    return {int(c) for c in re.findall(r"\{(\d+)u, \d+u,", block[:block.index("};")])}


def station_title(rows, keys, seen, t1):
    """a station's name in the menu with the count of its parts on the page (`keys`); at a T1 station whether the part
    with the pads' table, of all the parts seen with it (`seen`), is among them"""
    mark = f" - {len(keys)} part(s)"
    if any(s["count"] in t1 for k in seen for s in rows[k]["sightings"]):
        shown = any(s["count"] in t1 for k in keys for s in rows[k]["sightings"])
        mark += ", pads' part \u2714" if shown else ", pads' part NOT confirmed"
    return mark


def page_views(rows, t1=None):
    """the menu's pages (2026-10-24): (view, title, the models' keys on it); the pads' page has none. Under each role of
    a place to dock at its stations by the target's name the samples were taken with, each with its parts confirmed
    with that role only (Artur 2026-10-24: a role is a pigeonhole - after a reload each page shows what has its role now,
    so a wrong role shows at a glance; whatever is not confirmed is on "to confirm")"""
    t1 = t1_counts() if t1 is None else t1
    with_picture = [k for k in rows if has_picture(rows[k])]
    views = [("pending", "to confirm", [k for k in with_picture if not confirmed(rows[k])]),
             ("waiting", "waiting for a picture", [k for k in rows if not has_picture(rows[k])])]
    seen_at = {}
    for k, r in rows.items():
        for s in r["sightings"]:
            target = (s.get("context") or {}).get("target")
            if target and s.get("surface", "target") == "target" and not docked(s.get("context")):
                seen_at.setdefault(target, set()).add(k)
    for o in ROLES[1:] + ("",):
        views.append((f"role:{o}", o or "no role", [k for k in with_picture if rows[k]["role"] == o]))
        if o in STATION_ROLES:
            for n in sorted(seen_at):
                keys = sorted(k for k in seen_at[n] if confirmed(rows[k]) and rows[k]["role"] == o)
                if keys:
                    views.append((f"station:{o}:{n}", n + station_title(rows, keys, seen_at[n], t1), keys))
            # the same parts by their type (Artur 2026-10-24): one sub-type is one model, so its page shows at a glance
            # whether every part filed under it looks like the one station
            types = {}
            for k in with_picture:
                if confirmed(rows[k]) and rows[k]["role"] == o and rows[k].get("type"):
                    types.setdefault(rows[k]["type"], []).append(k)
            views += [(f"type:{o}:{ty}", f"type: {ty}", sorted(keys)) for ty, keys in sorted(types.items())]
    return views


def page(data, key, view="pending"):
    """the form: a menu on the left (models to confirm, waiting for a picture, each role, the pads), on the right the
    page it names - so one kind of model can be looked at again without scrolling through all; Save sends only the
    rows of the page shown"""
    q = f"?k={key}"
    rows = data["models"]
    views = page_views(rows)
    if view != "pads" and view not in {v for v, _, _ in views}:
        view = "pending"
    out = ["<!doctype html><meta charset='utf-8'><title>edworld hologram models</title><style>"
           "body{font:14px sans-serif;background:#1d1f21;color:#ddd;margin:0}table{border-collapse:collapse}"
           "td{border-top:1px solid #444;padding:6px;vertical-align:top}textarea{width:30em;height:5em;background:#2b2d30;"
           "color:#eee}small{color:#999}img{background:#000;margin:2px}#bar{position:sticky;top:0;background:#1d1f21;"
           "padding:6px}.wait{color:#c96}#nav{position:fixed;top:0;left:0;bottom:0;width:15em;overflow:auto;"
           "background:#25272a;padding:8px;box-sizing:border-box}#nav a{display:block;color:#9bd;text-decoration:none;"
           "padding:2px 4px}#nav a.sub{padding-left:16px;font-size:12px;color:#8ab}#nav a.on{background:#3a3d42;color:#fff}#nav hr{border:0;border-top:1px solid #444}"
           "main{margin-left:15em;padding:0 8px}</style><nav id='nav'>"]
    def link(v, title, n):
        cls = " ".join(c for c in ("on" if v == view else "", "sub" if v.startswith(("station:", "type:")) else "") if c)
        on = f" class='{cls}'" if cls else ""
        count = "" if n is None else f" ({n})"
        return (f"<a{on} href='/?k={html.escape(key, quote=True)}&amp;view={html.escape(v, quote=True)}'>"
                f"{html.escape(title)}{count}</a>")
    out += [link(v, title, len(keys)) for v, title, keys in views[:2]] + ["<hr>"]
    out += [link(v, title, len(keys)) for v, title, keys in views[2:]] + ["<hr>", link("pads", "pads", len(data.get("pads", {})))]
    out.append("</nav><main>")
    if view != "pads":
        out.append("<div id='bar'><button onclick='save()'>Save</button> <span id='state'></span></div><table>")
    shown = set(next((keys for v, _, keys in views if v == view), []))
    order = sorted(shown, key=lambda k: (rows[k]["description_by"] == "person", rows[k]["confirmed"], k))
    for k in order:
        r = rows[k]
        pic = has_picture(r)
        counts = ", ".join(sorted({f"{s['count']} on {s['build']}" for s in r["sightings"]}))
        seen = [s for s in r["sightings"] if s["stamp"]]
        told = "<br>".join(html.escape(h) for s in seen[-3:] for h in s["hints"][:4])
        roles = "".join(f"<option{' selected' if r['role'] == o else ''} title='{html.escape(ROLE_TITLES.get(o, ''), quote=True)}'>"
                        f"{o}</option>" for o in ROLES)
        out.append(f"<tr data-key='{html.escape(k, quote=True)}'><td><textarea>{html.escape(r['description'])}</textarea><br>"
                   f"role <select class=role>{roles}</select> type <select class=type>"
                   + "".join(f"<option{' selected' if r.get('type', '') == o else ''}>{html.escape(o)}</option>"
                             for o in ("",) + tuple(TYPES.get(r["role"], ())))
                   + f"</select> <label><input type='checkbox'{' checked' if r['confirmed'] else ''}"
                   f"{'' if pic else ' disabled'} title='this part shows in the hologram of the first picture (what the "
                   f"description says)'> confirmed</label> <small>this part shows in the hologram of the first picture</small>"
                   f"{'' if pic else ' <span class=wait>waiting for a picture</span>'}<br>"
                   + (f"<small class=wait>suggested from: {html.escape(r['suggested_from'])} - confirm or correct</small><br>"
                      if r.get("suggested_from") else "")
                   + (f"<small>confirmed with the station's part {html.escape(r['confirmed_with'])} - correct if wrong</small><br>"
                      if r.get("confirmed_with") else "")
                   + ("<small class=wait>docked only: every sample was taken docked, where the hologram means nothing"
                      "</small><br>" if docked_only(r) and r["role"] != "ship" else "")
                   + f"<small>{html.escape(k)}<br>registry: {html.escape(r['registry'] or '-')}<br>indices: "
                   f"{html.escape(counts)}<br>seen {len(seen)} time(s)<br>{told}"
                   + "".join(f"<br>&#8658; {html.escape(d)}" for d in r.get("deductions") or []) + "</small></td><td>")
        shot = next((s for s in reversed(r["sightings"]) if s.get("picture")), None)
        if shot:
            out.append(f"<img src='/{shot['picture']}{q}' width='256' title='the hologram as the game drew it'>")
            ctx = shot.get("context") or {}
            where = ctx.get("target") or "no target"
            when = stamp_time(shot["stamp"]).strftime("%Y-%m-%d %H:%M:%S") if shot.get("stamp") else "-"
            out.append(f"<br><small>the whole hologram at {html.escape(where)}, {when}Z - this part only on the views"
                       "</small>")
            if shot.get("stamp"):
                twins = same_moment(rows, k, shot["stamp"])
                if twins:
                    out.append("<br><small class=wait>the same picture as: " + ", ".join(html.escape(x) for x in twins)
                               + " (the same moment)</small>")
            if shot.get("black"):
                out.append("<br><small class=wait>game's picture black: the hologram was not on screen (e.g. in a taxi)"
                           "</small><br>")
        if r.get("views"):
            out.append(f"<img src='/{r['views']}{q}' height='256' title='top, front, side'><br><small>top, front, side: "
                       "only the part with these indices - one of the hologram's draws; it need not look like the whole"
                       "</small>")
        out.append("</td></tr>")
    if view != "pads":
        if not order:
            out.append("<tr><td><small>none</small></td></tr>")
        out.append("</table>")
    pads = data.get("pads", {}) if view == "pads" else {}
    if view == "pads":
        out.append("<h3>Pads: the ship's up and nose on a docked pad, in the model's axes - nothing to confirm: "
                   "models-header and header take them once their model is confirmed with a role of a place to dock at</h3><table>")
    names = {s["count"]: (r["description"] or k).splitlines()[0] for k, r in rows.items() for s in r["sightings"] if r["description"]}
    rules = kind_rules(pads)
    for k in sorted(pads, key=lambda k: (not pads[k].get("usable"), pads[k]["station"], pads[k]["pad"])):
        p = pads[k]
        ready = pad_header_ready(p)
        kind = site_kind(p["station"])
        rule = rules.get((p["model"], kind, p["pad"]))
        if rule is None:
            agree = ""
        elif rule["alike"]:
            agree = (f"<br>pad {p['pad']} alike at {len(rule['stations'])} {KIND_TITLES[kind]} of the model: "
                     f"written for any of them")
        else:
            agree = (f"<br><span class=wait>pad {p['pad']} at {len(rule['stations'])} {KIND_TITLES[kind]} of the "
                     f"model: {'one only' if len(rule['stations']) == 1 else 'they differ'} - for each station only</span>")
        plane = f"{p['plane_deg']:+.0f} deg (measured {p['measured_deg']:+.1f})" if ready else f"measured {p['measured_deg']}"
        state = "for edworld" if p.get("usable") else "<span class=wait>left out</span>"
        out.append(f"<tr data-pad='{html.escape(k, quote=True)}'><td>{state}</td><td>"
                   f"{html.escape(p['station'])} <small>({html.escape(p.get('type') or '')}"
                   f"{', pads ' + html.escape(p['layout']) if p.get('layout') else ''})</small><br>pad <b>{p['pad']}</b>, "
                   f"model {p['model']} <small>{html.escape(names.get(p['model'], ''))}<br>kind: {KIND_TITLES[kind]}"
                   f"{agree}</small></td><td><small>plane {plane}<br>"
                   f"up {p['up']}<br>nose {p['nose']}<br>ship(s): {html.escape(', '.join(p.get('ships') or []) or '-')}<br>"
                   f"{len(p['looks'])} docking(s), {p.get('kept', 0)} kept, spread {p['spread_deg']} deg, last "
                   f"{html.escape(p['looks'][-1]['docked'] if p['looks'] else '-')}"
                   + "".join(f"<br><span class=wait>{html.escape(n)}</span>" for n in p.get("notes") or [])
                   + "</small></td></tr>")
    if view == "pads":
        out.append("</table>")
    out.append(f"</main><script>const K={json.dumps(key)};const T={json.dumps(TYPES)};"
               "document.addEventListener('change',e=>{if(!e.target.classList.contains('role'))return;"
               "const s=e.target.closest('tr').querySelector('select.type');s.replaceChildren();"
               "for(const o of [''].concat(T[e.target.value]||[])){const x=document.createElement('option');x.textContent=o;s.append(x)}});"
               "async function save(){const rows={};for(const tr of document.querySelectorAll('tr[data-key]')){"
               "rows[tr.dataset.key]={description:tr.querySelector('textarea').value,role:tr.querySelector('select.role').value,"
               "type:tr.querySelector('select.type').value,confirmed:tr.querySelector('input').checked}}"
               "const r=await fetch('/save?k='+encodeURIComponent(K),{method:'POST',headers:{'Content-Type':'application/json'},"
               "body:JSON.stringify({models:rows})});const j=await r.json();"
               "document.getElementById('state').textContent=r.ok?'saved '+j.saved:'NOT saved: '+j.error}</script>")
    return "".join(out).encode()


def apply_form(data, sent):
    """the page's rows onto the file; a description changed by the person is theirs from then on; a type one of its
    role's (TYPES), a page from before without types keeps a row's type while it fits the role. Pads are no one's to
    confirm (2026-10-24): a page from before that sends them is saved, its pads not read. The pads' state follows the
    models' confirmation and roles at once"""
    rows = data["models"]
    if not isinstance(sent, dict) or not isinstance(sent.get("models"), dict):
        raise ValueError("not an object of models")
    sent = sent["models"]
    for k, v in sent.items():
        if k not in rows or not isinstance(v, dict):
            raise ValueError(f"no model {k!r}")
        d, role, confirmed = v.get("description"), v.get("role"), v.get("confirmed")
        if not isinstance(d, str) or len(d) > 2000 or role not in ROLES or not isinstance(confirmed, bool):
            raise ValueError(f"{k}: description, role or confirmed not as expected")
        if confirmed and not has_picture(rows[k]):
            raise ValueError(f"{k}: waiting for a picture, cannot be confirmed")
        if "type" in v and v["type"] != "" and v["type"] not in TYPES.get(role, ()):
            raise ValueError(f"{k}: type {v['type']!r} is not one of the role {role!r}")
    for k, v in sent.items():
        r = rows[k]
        if v["description"] != r["description"]:
            r["description"], r["description_by"] = v["description"], "person"
        if (r.get("suggested_from") or r.get("confirmed_with")) and (
                v["role"] != r["role"] or v.get("type", r.get("type", "")) != r.get("type", "")):
            r["suggested_from"] = ""  # the person's own choice now
            r.pop("confirmed_with", None)
        if v["confirmed"]:
            r.pop("confirmed_with", None)
        r["role"], r["confirmed"] = v["role"], v["confirmed"]
        kind = v.get("type", r.get("type", ""))
        r["type"] = kind if kind in TYPES.get(r["role"], ()) else ""
    siblings(rows)
    suggest(rows)
    merge_pads(data, {}, pad_planes())


def suggest(rows):
    """a row not confirmed, its role and type unset, takes the role and type of a model confirmed with one of its
    counts (on any build), as a suggestion to confirm or correct (2026-10-24): `suggested_from` names that model; once
    only - a row the person changed keeps the key, empty, and is not suggested to again"""
    confirmed = {}
    for k, r in sorted(rows.items()):
        if r["confirmed"] and r["role"] and not k.startswith("registry:"):
            for s in r["sightings"]:
                confirmed.setdefault(s["count"], r)
    for r in rows.values():
        if r["confirmed"]:
            r.pop("suggested_from", None)
            continue
        if r["role"] or r.get("type") or "suggested_from" in r:
            continue
        source = next((confirmed[s["count"]] for s in r["sightings"] if s["count"] in confirmed), None)
        if source is not None:
            r["role"], r["type"] = source["role"], source.get("type", "")
            r["suggested_from"] = source.get("type") or (source["description"].splitlines()[0] if source["description"] else "?")


def server(db, key):
    db = Path(db)
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def send(self, code, body, ctype="application/json; charset=utf-8"):
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; "
                             "script-src 'unsafe-inline'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(data)

        def allowed(self, url):
            # the Host against DNS rebinding, the key against any other page and process
            if self.headers.get("Host") != f"127.0.0.1:{self.server.server_port}":
                return False
            return hmac.compare_digest(parse_qs(url.query).get("k", [""])[0].encode(), key.encode())

        def do_GET(self):
            url = urlparse(self.path)
            if not self.allowed(url):
                return self.send(403, {"error": "no access"})
            if url.path == "/":
                with lock:
                    view = parse_qs(url.query).get("view", ["pending"])[0]
                    return self.send(200, page(load(db), key, view), "text/html; charset=utf-8")
            m = re.fullmatch(r"/png/([0-9A-Za-z_.,-]+\.png)", url.path)
            if m and (db / "png" / m.group(1)).is_file():
                return self.send(200, (db / "png" / m.group(1)).read_bytes(), "image/png")
            self.send(404, {"error": "not found"})

        def do_POST(self):
            url = urlparse(self.path)
            if not self.allowed(url) or url.path != "/save":
                return self.send(403, {"error": "no access"})
            origin = self.headers.get("Origin")
            if origin is not None and origin != f"http://127.0.0.1:{self.server.server_port}":
                return self.send(403, {"error": "another origin"})
            try:
                length = int(self.headers.get("Content-Length", 0))
            except ValueError:
                return self.send(400, {"error": "bad length"})
            if length <= 0 or length > MAX_BODY:
                return self.send(413, {"error": "empty or too large"})
            with lock:
                data = load(db)
                try:
                    apply_form(data, json.loads(self.rfile.read(length)))
                except ValueError as e:
                    return self.send(400, {"error": str(e)})
                save(db, data)
            self.send(200, {"saved": datetime.now().strftime("%H:%M:%S")})

        def log_message(self, fmt, *args):
            sys.stderr.write("[holo_db] " + re.sub(r"k=[^& \"]+", "k=***", fmt % args) + "\n")

    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.daemon_threads = True
    return srv


def form(db, seconds=3600):
    key = secrets.token_urlsafe(24)
    srv = server(db, key)
    timer = threading.Timer(seconds, srv.shutdown)
    timer.daemon = True
    timer.start()
    print(f"holo_db: open http://127.0.0.1:{srv.server_port}/?k={key}  (Ctrl+C ends; closes itself in {seconds} s)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()


def write_header(out, text):
    out = Path(out)
    tmp = out.with_suffix(".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, out)


def place_of(role, type_):
    """edworld's kind of place (holo_place_e) for a model's role and type: whether a grant's hologram shows pads and which
    rule gives their plane (D, Artur 2026-10-24); an orbital port is a T1 outpost by its type's "Outpost", a port of
    another type a big station, one without a type none (not known which)"""
    if role == "port-orbital":
        return ("outpost_t1" if "Outpost" in type_ else "big_station") if type_ else "none"
    return {"port-surface": "planet", "settlement": "planet", "carrier": "carrier", "operations-carrier": "carrier",
            "construction-orbital": "construction_orbital"}.get(role, "none")


# a model's name in the public header by its role when the form gives it no type and the registry no name (F,
# 2026-10-25): the person's description stays in models.json only
ROLE_NAMES = {"ship": "ship", "carrier": "carrier", "port-orbital": "orbital port", "port-surface": "surface port",
              "settlement": "settlement or planetary construction site", "construction-orbital": "orbital construction site",
              "operations-carrier": "Operations' small carrier", "operations": "Operations' large ship",
              "installation": "installation", "space-entity": "space entity", "poi": "point of interest", "pad": "pad",
              "station": "station", "other": "other"}


def header_name(r):
    """a model's name in holo_models_generated.h: its type from the form, else its registry name, else its role's
    (ROLE_NAMES); never the description, which is the person's own words; printable ASCII only"""
    name = (r.get("type") or r.get("registry") or ROLE_NAMES.get(r.get("role") or "", "other")).strip()[:80]
    name = name.replace("\\", "/").replace('"', "'")
    if not name.isascii() or not name.isprintable():
        raise ValueError(f"holo_db: a header name not printable ASCII: {name!r}")
    return name


def models_header(db, out=MODELS_HEADER):
    """the models confirmed in the form as holo_models_generated.h: one entry for each build and count a model was seen
    with, its fingerprint when a log gave it, a ship's role ship and any other role other (no order rule: a carrier's
    hologram is drawn to the own surface at its pad), its kind of place (place_of); a build and count holo_models.h's
    measured entries have is theirs. A file written whole, the same text for the same models"""
    data = load(Path(db))
    measured = registry()
    rows, taken = [], set()
    for k, r in sorted(data["models"].items()):
        if not confirmed(r) or not has_picture(r):
            continue
        name = header_name(r)
        role = "ship" if r["role"] == "ship" else "other"
        place = place_of(r["role"], (r.get("type") or "").strip())
        for build, count in sorted({(s["build"], s["count"]) for s in r["sightings"]}):
            if (build, count) in measured or (build, count) in taken:
                continue
            taken.add((build, count))
            fp = (r.get("fingerprints") or {}).get(f"{build}|{count}")
            rows.append(f'    {{"{name}", holo_role_e::{role}, {count}u, holo_status_e::confirmed, "{build}", '
                        f'{"true" if fp else "false"}, {fp or "{}"}, holo_place_e::{place}}},  // {r["role"] or "no role"}, {k}')
    text = ("// Generated by tools/holo_db.py models-header from the models confirmed in holo_db's form (models.json); not\n"
            "// edited by hand. The names: the form's type, the registry's name or the role's. The table ends with an empty row.\n#pragma once\n\n"
            "namespace edworld\n  {\n  inline constexpr holo_model_t holo_models_confirmed[]{\n" + "".join(r + "\n" for r in rows)
            + '    {"", holo_role_e::other, 0u, holo_status_e::confirmed, "", false, {}},\n  };\n  }  // namespace edworld\n')
    write_header(out, text)
    print(f"holo_db: {len(rows)} confirmed model entr(y/ies) -> {out}")
    return len(rows)


def header(db, out=PADS_HEADER):
    """the pads for edworld_eht (merge_pads: usable) as holo_pads_generated.h: a file written whole, the same text for the
    same pads; each a station's own, and one for any station of a kind of place (kind_rules) where two stations or more of
    the kind have the pad alike and none of them otherwise - one hologram (13083) shows construction sites of 5 and 7 pads and settlements,
    their pads numbered apart, so a pad alike at two of them may yet differ at a third (2026-10-24)"""
    pads = load(Path(db)).get("pads", {})
    usable = [pads[k] for k in sorted(pads, key=lambda k: (pads[k]["model"], pads[k]["station"], pads[k]["pad"]))
              if pads[k].get("usable")]
    rows = []
    for p in usable:
        station = p["station"].replace("\\", "").replace('"', "")
        nose = ", ".join(f"{v:.3f}f" for v in p["nose"])
        rows.append(f'    {{{p["model"]}u, "{station}", {p["pad"]}u, {p["plane_deg"]:.1f}f, {{{nose}}}}},'
                    f"  // {p['kept']} of {len(p['looks'])} docking(s), last {p['looks'][-1]['docked']}")
    for (model, kind, pad), rule in sorted(kind_rules(pads).items()):
        if rule["alike"]:
            nose = ", ".join(f"{v:.3f}f" for v in rule["nose"])
            rows.append(f'    {{{model}u, "", {pad}u, {rule["plane_deg"]:.1f}f, {{{nose}}}, site_kind_e::{kind}}},'
                        f"  // any of the {KIND_TITLES[kind]}: {len(rule['stations'])} stations, {rule['looks']} docking(s)")
    text = ("// Generated by tools/holo_db.py header from the pads of the models confirmed in holo_db's form (models.json); not\n"
            "// edited by hand.\n"
            "// The pads: holo_db.py header. The table ends with an empty row.\n#pragma once\n\nnamespace edworld\n  {\n"
            "  inline constexpr pad_learned_t pad_learned[]{\n" + "".join(r + "\n" for r in rows)
            + '    {0u, "", 0u, 0.f, {0.f, 0.f, 0.f}},\n  };\n  }  // namespace edworld\n')
    write_header(out, text)
    print(f"holo_db: {len(rows)} pad(s) -> {out}")
    return len(rows)


def self_test():
    def encode(p):  # decode_pos's plain format, z's top byte 0: 21 bits a coordinate over -1..1
        b = [round((c + 1.0) * ((1 << 20) - 1)) for c in p]
        return struct.pack("<3I", b[0] | (b[1] & 0x7FF) << 21, b[1] >> 11 | b[2] << 10, 0) + bytes(VERTEX_STRIDE - 12)

    corners = [(x, y, z) for x in (-0.5, 0.5) for y in (-0.25, 0.25) for z in (-1.0, 1.0)]
    for c in corners:
        assert all(abs(a - b) < 1e-5 for a, b in zip(decode_pos(*struct.unpack("<3I", encode(c)[:12])), c)), c
    faces = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1), (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4),
             (1, 5, 7), (1, 7, 3)]
    work = Path(tempfile.mkdtemp(prefix="holo_db_test_"))
    models, journals, db = work / "models", work / "journals", work / "db"
    models.mkdir()
    journals.mkdir()
    old, new = "103482368-01dd60867a106e8a", "103481344-01dd62ef6d758059"
    for build, stamp in ((old, "20261023T040530_946Z"), (new, "20261023T150956_098Z")):
        stem = models / f"{build}_{stamp}_3924"
        Path(f"{stem}_ib_u16.bin").write_bytes(struct.pack(f"<{len(faces) * 3}H", *[i for f in faces for i in f]))
        Path(f"{stem}_vb_s40.bin").write_bytes(b"".join(encode(c) for c in corners))
    Path(models / f"{new}_20261023T150956_098Z_3924_holo.pgm").write_bytes(b"P5\n4 2\n255\n" + bytes(range(0, 255, 32)))
    first = '{"timestamp":"2026-10-23T15:09:53Z","event":"Loadout","Ship":"sidewinder"}\r\n'
    (journals / "Journal.2026-10-23T170745.01.log").write_bytes(
        (first + '{"timestamp":"2026-10-23T15:09:55Z","event":"Embark","Taxi":true,"StationName":"Fukuda Town"}\r\n'
         '{"timestamp":"2026-10-23T15:30:00Z","event":"Loadout","Ship":"panthermkii"}\r\n').encode())
    # as edworld_eht writes it (sample_context.h): the journal's place is the end of its first line
    (models / f"{new}_20261023T150956_098Z_3924_context.json").write_text(json.dumps(
        {"sample": f"{new}_20261023T150956_098Z_3924", "status": {"Flags": 16842765, "BodyName": "Bleia Eohn PW-D b32-1 7",
         "Destination": {"System": 1, "Body": 7, "Name": "Bischoff Legacy"}},
         "journal": {"file": "Journal.2026-10-23T170745.01.log", "size": len(first), "last_line": json.loads(first)}}),
        encoding="utf-8")
    fake = work / "journal_context"
    fake.write_text("#!/bin/sh\necho '" + json.dumps(
        {"last_before": "2026-10-23T15:09:53Z", "steps": [{"seconds": 2, "event": "Embark", "what": "taxi"}],
         "before": {"where": "docked", "station": "Fukuda Town", "pad": 0},
         "after": {"where": "taxi", "station": "Fukuda Town", "pad": 0}}) + "'\n", encoding="utf-8")
    fake.chmod(0o755)
    # the pads: edworld_eht's log beside edworld_models (here: work) and the journal's grants and Docked
    def axes_line(t, count, x, y, z):
        return (f"2026-10-23T{t}Z hologram: {count} indices, scale 0.01300, q +0 +0 +0 +1 | X {x[0]:+.3f} {x[1]:+.3f} {x[2]:+.3f} "
                f"Y {y[0]:+.3f} {y[1]:+.3f} {y[2]:+.3f} Z {z[0]:+.3f} {z[1]:+.3f} {z[2]:+.3f}\n")
    wing = ((0.917, -0.381, 0.115), (0.388, 0.920, -0.050), (-0.087, 0.090, 0.992))   # PRNH pad 18, 2026-10-23
    ground = ((-0.996, 0.0, -0.087), (0.008, 0.996, -0.090), (0.087, -0.090, -0.992))  # Bischoff Legacy pad 1
    (work / "edworld_eht.log").write_text(
        "2026-10-23T18:00:00.000Z edworld_eht v-test\n"
        f"2026-10-23T18:00:01.000Z hologram model sample: 3924 indices (target's surface), 8 vertices, 12 triangles, build {new}: -\n"
        "2026-10-23T18:00:01.000Z hologram model fingerprint 3924: {8u, 12u, {0.5000f, 0.2500f, 1.0000f}, {{{0.5000f, 0.2500f, "
        "1.0000f}}}, {{0x1ull}}}\n"
        "2026-10-23T18:00:02.000Z hologram model fingerprint 9999: {1u, 1u, {1.0000f, 1.0000f, 1.0000f}, {{}}, {{0x1ull}}}\n"
        "2026-10-23T18:01:09.100Z state: pad 18 given at a FleetCarrier (from the data source)\n"
        + axes_line("18:02:00.000", 84969, *wing) + axes_line("18:02:10.000", 84969, *wing) + axes_line("18:02:20.000", 84969, *wing)
        + "2026-10-23T18:10:01.000Z state: pad 3 given at a PlanetaryConstructionDepot (from the data source)\n"
        + axes_line("18:11:05.000", 13083, *ground) + axes_line("18:11:10.000", 13083, *ground)
        + axes_line("18:11:20.000", 84969, *wing) + axes_line("18:11:30.000", 84969, *wing) + axes_line("18:11:40.000", 84969, *wing)
        + axes_line("18:21:05.000", 13083, *ground) + axes_line("18:21:10.000", 13083, *ground), encoding="utf-8")
    site = "Planetary Construction Site: Bischoff Legacy"
    (journals / "Journal.2026-10-23T180000.01.log").write_text("".join(json.dumps(e) + "\r\n" for e in (
        {"timestamp": "2026-10-23T18:00:30Z", "event": "Loadout", "Ship": "PantherMkII"},
        {"timestamp": "2026-10-23T18:01:08Z", "event": "DockingRequested", "MarketID": 1, "StationName": "PRNH",
         "LandingPads": {"Small": 8, "Medium": 8, "Large": 16}},
        {"timestamp": "2026-10-23T18:01:09Z", "event": "DockingGranted", "LandingPad": 18, "MarketID": 1, "StationName": "PRNH"},
        {"timestamp": "2026-10-23T18:01:59Z", "event": "Docked", "MarketID": 1, "StationName": "PRNH", "StationType": "FleetCarrier"},
        {"timestamp": "2026-10-23T18:09:00Z", "event": "Loadout", "Ship": "Python_NX"},
        {"timestamp": "2026-10-23T18:10:00Z", "event": "DockingRequested", "MarketID": 2, "StationName": site,
         "LandingPads": {"Small": 2, "Medium": 2, "Large": 3}},
        {"timestamp": "2026-10-23T18:10:01Z", "event": "DockingGranted", "LandingPad": 3, "MarketID": 2, "StationName": site},
        {"timestamp": "2026-10-23T18:11:01Z", "event": "Docked", "MarketID": 2, "StationName": site, "StationType": "PlanetaryConstructionDepot"},
        # another account's docking: its grant not in this log
        {"timestamp": "2026-10-23T18:20:01Z", "event": "DockingGranted", "LandingPad": 1, "MarketID": 2, "StationName": site},
        {"timestamp": "2026-10-23T18:21:01Z", "event": "Docked", "MarketID": 2, "StationName": site, "StationType": "PlanetaryConstructionDepot"})),
        encoding="utf-8")
    data = scan(models, db, journals, fake)
    pads = data["pads"]
    assert sorted(pads) == ["13083|" + site + "|3", "84969|PRNH|18"], sorted(pads)
    p18, p3 = pads["84969|PRNH|18"], pads["13083|" + site + "|3"]
    assert p18["plane_deg"] == -24.0 and p18["measured_deg"] == -22.5 and len(p18["looks"]) == 1 and p18["nose"][2] > 0.99, p18
    assert p3["plane_deg"] == 0.0 and len(p3["looks"]) == 1 and p3["looks"][0]["up"] == [0.0, 0.996, -0.09], p3  # 13083's looks only
    assert "one docking only - weak" in p18["notes"] and not p18["usable"]
    assert "its model not confirmed in the form yet: waits" in p18["notes"], p18["notes"]
    assert p18["layout"] == "S8 M8 L16" and p18["ships"] == ["panthermkii (L)"], p18
    assert p3["layout"] == "S2 M2 L3" and p3["looks"][0]["ship_pad"] == "M", p3
    rows = data["models"]
    cube = [r for k, r in rows.items() if k.startswith("v8-t12-")]
    assert len(cube) == 1, rows.keys()  # one model over both builds
    cube = cube[0]
    assert len(cube["sightings"]) == 2 and cube["registry"] == "Sidewinder" and cube["role"] == "ship"
    assert cube["description"] == "Sidewinder" and cube["extent"] == [0.5, 0.25, 1.0], cube
    assert cube["sightings"][0]["hints"] == [] and "context" not in cube["sightings"][0], cube["sightings"][0]
    assert cube["sightings"][1]["hints"] == [
        "before: docked at Fukuda Town", "then Embark taxi (+2 s)", "after a minute: taxi at Fukuda Town", "target Bischoff Legacy",
        "near Bleia Eohn PW-D b32-1 7", "journal Journal.2026-10-23T170745.01.log at byte 76, last Loadout 2026-10-23T15:09:53Z"], \
        cube["sightings"][1]["hints"]
    assert cube["sightings"][1]["context"]["offset"] == len(first)
    assert cube["sightings"][1]["black"] is False  # the test's picture has grey pixels
    black = work / "black.png"
    png(black, 4, 2, [bytes(4), bytes(4)])
    assert png_black(black) and not png_black(db / cube["sightings"][1]["picture"]) and not png_black(work / "none.png")
    cube["sightings"][1]["black"] = True
    assert b"game's picture black" in page(data, "k")
    cube["sightings"][1]["black"] = False
    # the fingerprint right after its sample's line, of the build the line names; one without a sample line is no one's
    assert cube["fingerprints"] == {f"{new}|3924": "{8u, 12u, {0.5000f, 0.2500f, 1.0000f}, {{{0.5000f, 0.2500f, 1.0000f}}}, "
                                                   "{{0x1ull}}}"}, cube.get("fingerprints")
    assert cube["deductions"] == ["on 2 builds of the game: the same count, the id kept",
                                  "right after Loadout sidewinder: 1 of 2 sample(s), the own ship's model - weak, one sample",
                                  "at boarding a taxi: 1 of 2 sample(s) - weak, one sample",
                                  "at the sample: docked, 1 of 1 placed sample(s) - weak, one sample",
                                  "a minute later: taxi, 1 of 1 placed sample(s) - weak, one sample",
                                  "target chosen: Bischoff Legacy"], \
        cube["deductions"]
    station = {"sightings": [{"stamp": "x", "build": "b", "count": 1, "near": {
        "DockingRequested": {"StationName": n, "StationType": "PlanetaryConstructionDepot", "dt": -41},
        "DockingGranted": {"StationName": n, "LandingPad": 1, "dt": -41}, "Docked": {"StationName": n, "dt": 6}}}
        for n in ("Bischoff Legacy", "Biggs Base")]}
    assert deductions(station) == [
        "the same model at 2 stations, all PlanetaryConstructionDepot: the type's model, not one station's",
        "pads granted: pad 1 at Biggs Base; pad 1 at Bischoff Legacy", "between the grant and Docked: 2 of 2 sample(s)"], \
        deductions(station)
    assert "registry:Sidewinder" not in rows and "registry:Kestrel" in rows  # Kestrel: no sample, waits
    for p in [cube["views"], cube["sightings"][1]["picture"]]:
        b = (db / p).read_bytes()
        assert b.startswith(b"\x89PNG"), p
    # a second scan adds nothing and keeps a person's description
    cube["description"], cube["description_by"] = "Sidewinder, own", "person"
    data["models"]["kestrel-new"] = {"description": "Kestrel", "description_by": "person", "role": "ship", "confirmed": True,
                                     "registry": None, "vertices": None, "triangles": None, "extent": None,
                                     "views": "png/x.png", "sightings": [{"build": new, "stamp": None, "count": 27885,
                                                                          "picture": None, "hints": []}]}
    save(db, data)
    again = scan(models, db, journals, fake)["models"]
    assert "registry:Kestrel" not in again  # a model confirmed with its count stands for the registry's placeholder
    del again["kestrel-new"]
    save(db, {**load(db), "models": again})
    again = scan(models, db, journals, fake)["models"]
    assert "registry:Kestrel" in again  # without it the placeholder waits again
    assert sum(len(r["sightings"]) for r in again.values() if r["registry"] == "Sidewinder") == 2
    assert next(r for r in again.values() if r["registry"] == "Sidewinder")["description"] == "Sidewinder, own"
    # docked (Artur 2026-10-24: the hologram there means nothing): a model seen only docked makes no row, once
    stem = models / f"{new}_20261023T160000_000Z_36"
    Path(f"{stem}_ib_u16.bin").write_bytes(struct.pack(f"<{len(faces) * 3}H", *[i for f in faces for i in f]))
    Path(f"{stem}_vb_s40.bin").write_bytes(b"".join(encode((x * 2, y, z * 0.5)) for x, y, z in corners))
    Path(f"{stem}_context.json").write_text(json.dumps({"sample": stem.name, "status": {"Flags": 16842765}}), encoding="utf-8")
    from contextlib import redirect_stdout
    from io import StringIO
    with redirect_stdout(StringIO()):
        docked_scan = scan(models, db, journals, fake)
        assert scan(models, db, journals, fake)["docked_left_out"] == [[new, "20261023T160000_000Z", 36]]
    assert not any(s["count"] == 36 for r in docked_scan["models"].values() for s in r["sightings"])
    assert docked({"flags": 16842765}) and not docked({"flags": 16842764}) and not docked(None)
    assert docked_only({"sightings": [{"context": {"flags": 1}}, {"context": {"flags": 3}}]})
    assert not docked_only({"sightings": [{"context": {"flags": 1}}, {"context": {"flags": 0}}]})
    assert not docked_only({"sightings": [{"context": {"flags": 1}}, {}]}) and place_of("port-orbital", T1_ANY) == "outpost_t1"
    # the form: refused without the key and for a model waiting for a picture; a save keeps .bak
    import urllib.error
    import urllib.request
    key = "test-key"
    srv = server(db, key)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"
    ck = next(k for k, r in again.items() if r["registry"] == "Sidewinder")

    def post(body, k=key):
        req = urllib.request.Request(f"{base}/save?k={k}", json.dumps(body).encode(), {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code

    try:
        urllib.request.urlopen(f"{base}/")
        raise AssertionError("served without the key")
    except urllib.error.HTTPError as e:
        assert e.code == 403
    with urllib.request.urlopen(f"{base}/?k={key}") as r:  # to confirm: the Sidewinder with its picture, not yet confirmed
        body = r.read()
        assert b"only the part with these indices" in body and b"this part shows in the hologram of the first picture" in body
        assert b"class='on' href='/?k=test-key&amp;view=pending'>to confirm (1)" in body and b"data-key='registry:Kestrel'" not in body
    with urllib.request.urlopen(f"{base}/?k={key}&view=waiting") as r:  # Kestrel: no sample, waits
        body = r.read()
        assert b"waiting for a picture" in body and b"data-key='registry:Kestrel'" in body and b"data-key='v8-t12-" not in body
    with urllib.request.urlopen(f"{base}/?k={key}&view=role:ship") as r:
        body = r.read()
        assert b"data-key='v8-t12-" in body and b"data-key='registry:Kestrel'" not in body and b"data-pad=" not in body
    assert post({"models": {ck: {"description": "x", "role": "ship", "confirmed": True}}}, "wrong") == 403
    assert post({"models": {"registry:Kestrel": {"description": "", "role": "ship", "confirmed": True}}}) == 400
    assert post({ck: {"description": "x", "role": "ship", "confirmed": True}}) == 400  # the old shape
    (db / "models.json.bak").unlink(missing_ok=True)
    assert post({"models": {}, "pads": {"no|such|pad": {"confirmed": True}}}) == 200  # a page from before: pads not read
    assert post({"models": {ck: {"description": "x", "role": "planet", "confirmed": False}}}) == 400  # no such role
    assert set(ROLE_TITLES) == set(ROLES) - {""} and "space-entity" in ROLES
    assert post({"models": {ck: {"description": "Sidewinder (own)", "role": "ship", "confirmed": True}},
                 "pads": {"84969|PRNH|18": {"confirmed": True}}}) == 200
    with urllib.request.urlopen(f"{base}/?k={key}&view=pads") as r:
        assert b"data-pad='84969|PRNH|18'" in r.read()
    srv.shutdown()
    srv.server_close()
    saved = load(db)["models"][ck]
    assert saved["confirmed"] and saved["description"] == "Sidewinder (own)" and (db / "models.json.bak").exists()
    from contextlib import redirect_stdout
    from io import StringIO
    models_out = work / "holo_models_generated.h"
    with redirect_stdout(StringIO()):
        assert models_header(db, models_out) == 1  # the old build's 3924 is holo_models.h's measured Sidewinder
    text = models_out.read_text(encoding="utf-8")
    assert "Sidewinder (own)" not in text  # the description never names a model in the header (F, 2026-10-25)
    assert (f'{{"Sidewinder", holo_role_e::ship, 3924u, holo_status_e::confirmed, "{new}", true, {{8u, 12u, ' in text
            and text.rstrip().endswith("}  // namespace edworld")), text
    # the type (2026-10-24): one of the role's, kept by a page from before while it fits, the model's name for edworld
    import copy
    typed = copy.deepcopy(load(db))
    apply_form(typed, {"models": {ck: {"description": "x", "role": "port-orbital", "type": "Coriolis Starport: quad_truss",
                                       "confirmed": True}}})
    assert typed["models"][ck]["type"] == "Coriolis Starport: quad_truss"
    apply_form(typed, {"models": {ck: {"description": "x", "role": "port-orbital", "confirmed": True}}})  # a page from before
    assert typed["models"][ck]["type"] == "Coriolis Starport: quad_truss"
    assert "Scientific Surface Outpost: subtype unknown" in TYPES["port-surface"] and "settlement" not in TYPES
    assert "installation" in ROLES and "installation" not in DOCK_ROLES and "Industrial Hub" in TYPES["installation"]
    typed_db = work / "typed"
    typed_db.mkdir()
    save(typed_db, typed)
    with redirect_stdout(StringIO()):
        models_header(typed_db, work / "typed.h")
    typed_text = (work / "typed.h").read_text(encoding="utf-8")
    assert '{"Coriolis Starport: quad_truss", holo_role_e::other, 3924u' in typed_text
    assert "holo_place_e::big_station},  // port-orbital" in typed_text, typed_text
    # the header's names (F, 2026-10-25): the type, the registry's name, the role's; a description never; ASCII only
    assert (header_name({"type": "Fleet Carrier", "registry": "x", "role": "carrier", "description": "y"}) == "Fleet Carrier"
            and header_name({"type": "", "registry": "Kestrel", "role": "ship", "description": "own"}) == "Kestrel"
            and header_name({"type": None, "registry": None, "role": "ship", "description": "Adder?"}) == "ship"
            and header_name({"registry": None, "role": "space-entity", "description": "gwiazda"}) == "space entity"
            and header_name({"role": "", "description": "pad ale"}) == "other")
    try:
        header_name({"type": "Gwiazda ż", "role": "pad"})
        raise AssertionError("a name not ASCII taken")
    except ValueError:
        pass
    # the kind of place (D, 2026-10-24): an orbital outpost T1 by its type, a place on a planet, a carrier, no pads
    assert (place_of("port-orbital", "Military Outpost: nemesis") == "outpost_t1"
            and place_of("port-orbital", "Orbis Starport: apollo") == "big_station" and place_of("port-orbital", "") == "none"
            and place_of("port-surface", "Civilian Surface Outpost: lachesis") == "planet" and place_of("settlement", "") == "planet"
            and place_of("carrier", "") == "carrier" and place_of("operations-carrier", "") == "carrier"
            and place_of("construction-orbital", "") == "construction_orbital" and place_of("ship", "") == "none"
            and place_of("installation", "Industrial Hub") == "none" and place_of("pad", "") == "none")
    body = page(typed, key, "role:port-orbital").decode()
    assert "<option selected>Coriolis Starport: quad_truss</option>" in body and "select class=type" in body
    try:
        apply_form(typed, {"models": {ck: {"description": "x", "role": "ship", "type": "Orbis Starport: apollo", "confirmed": True}}})
        raise AssertionError("a type of another role taken")
    except ValueError:
        pass
    apply_form(typed, {"models": {ck: {"description": "x", "role": "ship", "confirmed": True}}})
    assert typed["models"][ck]["type"] == ""  # a page from before: the type no longer the role's
    # a new row of a confirmed model's count: its role and type suggested, not confirmed; once only
    rows_s = {"done": {"confirmed": True, "role": "port-orbital", "type": "Military Outpost: nemesis", "description": "x",
                       "sightings": [{"count": 5238}]},
              "new": {"confirmed": False, "role": "", "description": "", "sightings": [{"count": 5238}]},
              "other": {"confirmed": False, "role": "", "description": "", "sightings": [{"count": 1}]}}
    suggest(rows_s)
    assert rows_s["new"]["role"] == "port-orbital" and rows_s["new"]["type"] == "Military Outpost: nemesis"
    assert rows_s["new"]["suggested_from"] == "Military Outpost: nemesis" and not rows_s["new"]["confirmed"]
    assert rows_s["other"]["role"] == "" and "suggested_from" not in rows_s["other"]
    rows_s["new"]["role"], rows_s["new"]["type"], rows_s["new"]["suggested_from"] = "", "", ""  # the person cleared it
    suggest(rows_s)
    assert rows_s["new"]["role"] == ""
    # one confirmation a station: the parts of its hologram drawn to the target's surface within seconds, the same
    # target (Marth's Inheritance, 2026-10-24: 4 parts, the T1 table's 5478 among them); its menu entry under the role
    def part(count, stamp, target="Marth's Inheritance", surface="target", **kw):
        return {"description": "", "description_by": "scan", "role": "", "confirmed": False, "views": "png/x.png", "registry": None,
                "sightings": [{"build": new, "stamp": stamp, "count": count, "picture": "p", "hints": [], "surface": surface,
                               "context": {"target": target}}], **kw}
    marth = {"a": part(2082, "20261024T144510_392Z", role="port-orbital", type="Civilian Outpost: vesta", confirmed=True),
             "b": part(5478, "20261024T144509_360Z"), "c": part(5406, "20261024T144508_327Z"),
             "own": part(27885, "20261024T144509_000Z", surface="own"),
             "later": part(3180, "20261024T150000_000Z"),
             "other": part(948, "20261024T144511_000Z", target="Arkush City"),
             "ship": part(62688, "20261024T144510_900Z", role="ship", confirmed=True)}
    siblings(marth)
    assert marth["b"]["confirmed_with"] == "a" and marth["b"]["type"] == "Civilian Outpost: vesta" and confirmed(marth["b"])
    assert marth["c"]["confirmed_with"] == "a" and not marth["b"]["confirmed"]
    assert not confirmed(marth["own"]) and not confirmed(marth["later"]) and not confirmed(marth["other"])
    views = {v: (title, keys) for v, title, keys in page_views(marth, {5478})}
    title, keys = views["station:port-orbital:Marth's Inheritance"]
    assert set(keys) == {"a", "b", "c"} and "3 part(s)" in title and "pads' part \u2714" in title, (title, keys)
    assert not any(v.startswith("station:ship") for v in views) and "ship" in views["role:ship"][1]
    title, keys = views["type:port-orbital:Civilian Outpost: vesta"]
    assert title == "type: Civilian Outpost: vesta" and set(keys) == {"a", "b", "c"}, (title, keys)
    assert not any(v.startswith("type:ship") for v in views)
    assert "b" not in views["pending"][1] and "later" in views["pending"][1]
    apply_form({"models": marth}, {"models": {"b": {"description": "", "role": "port-orbital", "type": "Military Outpost: nemesis",
                                                 "confirmed": False}}})
    assert "confirmed_with" not in marth["b"] and not confirmed(marth["b"])  # the person's correction holds
    siblings(marth)
    assert "confirmed_with" not in marth["b"]
    station_page = page({"models": marth}, "k", "station:port-orbital:Marth's Inheritance")
    assert b"confirmed with the station's part a" in station_page
    # the game's picture is the whole hologram: the parts drawn at one moment share it, and the page says so
    assert b"the whole hologram at Marth&#x27;s Inheritance" in station_page and b"the same picture as: a, b, other, own, ship (the same moment)" in station_page, station_page
    assert same_moment(marth, "later", "20261024T150000_000Z") == []
    assert 4404 in t1_counts() and 5478 in t1_counts()
    generated = work / "holo_pads_generated.h"
    with redirect_stdout(StringIO()):
        assert header(db, generated) == 0  # PRNH's and the site's models not confirmed: their pads wait
    # the carrier's model confirmed as a carrier: its pad goes; the site's model confirmed as a ship: its pad left out
    data = load(db)
    data["models"]["carrier"] = {"description": "squadron carrier", "description_by": "person", "role": "carrier",
                                 "confirmed": True, "registry": None, "vertices": None, "triangles": None, "extent": None,
                                 "views": "png/x.png", "sightings": [{"build": new, "stamp": "s", "count": 84969, "picture": None, "hints": []}]}
    data["models"]["site"] = dict(data["models"]["carrier"], role="ship", sightings=[{"build": new, "stamp": "s", "count": 13083, "picture": None, "hints": []}])
    apply_form(data, {"models": {}})
    p18, p3 = data["pads"]["84969|PRNH|18"], data["pads"]["13083|" + site + "|3"]
    assert p18["usable"] and not p3["usable"] and "its model confirmed as ship: not a place to dock, left out" in p3["notes"]
    save(db, data)
    with redirect_stdout(StringIO()):
        assert header(db, generated) == 1
    text = generated.read_text(encoding="utf-8")
    assert '{84969u, "PRNH", 18u, -24.0f, {' in text and site not in text and text.rstrip().endswith("}  // namespace edworld"), text
    assert "for edworld" in page(data, key, "pads").decode() and "data-key=" not in page(data, key, "pads").decode() and "type='checkbox' checked> confirmed</label></td>" not in page(data, key, "pads").decode()
    # a docking off the model's planes left out on its own (Eames Genetics Lab pad 1, 2026-10-23); noses: the most dockings'
    planes = pad_planes()
    flat = {"up": [0.0, 0.996, -0.09], "nose": [-0.087, -0.09, -0.992]}
    row = {"model": 13083, "looks": [flat, {"up": [0.364, 0.429, 0.827], "nose": [0.817, 0.28, -0.504]}]}
    kept, off, other = kept_looks(row, planes)
    assert kept == [flat] and off == 1 and other == 0
    turned = {"up": [0.0, 1.0, 0.0], "nose": [1.0, 0.0, 0.0]}
    assert kept_looks({"model": 13083, "looks": [flat, flat, turned]}, planes) == ([flat, flat], 0, 1)
    assert kept_looks({"model": 13083, "looks": [flat, turned]}, planes) == ([], 0, 2)
    assert (site_kind("Planetary Construction Site: Bauer Command"), site_kind("Cardenas Plantations"),
            site_kind("$USS_DistressCall;"), site_kind("")) == ("construction_site", "other", "unknown", "unknown")
    data = load(db)  # pad 1 alike at two construction sites, pad 3 not; a settlement's pad 1 its own; Status.json's names
    def pad(station, number, nose, usable=True):
        return {"model": 13083, "station": station, "pad": number, "looks": [{"docked": "2026-10-24T06:00:00Z"}],
                "usable": usable, "kept": 1, "plane_deg": 0.0, "measured_deg": 0.1, "up": [0.0, 1.0, 0.0], "nose": nose,
                "spread_deg": 0.0}
    data["pads"] = {"a": pad("Planetary Construction Site: A", 1, [0.0, 0.0, -1.0]),
                    "b": pad("Planetary Construction Site: B", 1, [0.05, 0.0, -0.999]),
                    "c": pad("Planetary Construction Site: A", 3, [-1.0, 0.0, 0.0]),
                    "d": pad("Planetary Construction Site: C", 3, [0.0, 0.0, -1.0]),
                    "e": pad("Cardenas Plantations", 1, [1.0, 0.0, 0.0]),
                    "f": pad("Planetary Construction Site: D", 1, [1.0, 0.0, 0.0], usable=False)}
    rules = kind_rules(data["pads"])
    assert rules[(13083, "construction_site", 1)]["alike"] and not rules[(13083, "construction_site", 3)]["alike"], rules
    assert not rules[(13083, "other", 1)]["alike"] and len(rules[(13083, "construction_site", 1)]["stations"]) == 2, rules
    assert b"alike at 2 construction sites" in page(data, key, "pads") and b"they differ" in page(data, key, "pads")
    assert b"data-pad=" not in page(data, key) and b"data-pad=" not in page(data, key, "no-such-view")  # the default page
    save(db, data)
    with redirect_stdout(StringIO()):
        assert header(db, generated) == 6
    text = generated.read_text(encoding="utf-8")
    assert '{13083u, "", 1u, 0.0f, {0.025f, 0.000f, -1.000f}, site_kind_e::construction_site},' in text, text
    assert "site_kind_e::other" not in text and '{13083u, "", 3u' not in text, text
    print(f"holo_db self-test: ok ({work})")


def main(argv):
    if argv[1:2] == ["--self-test"]:
        return self_test()
    if len(argv) >= 4 and argv[1] == "scan":
        rest = argv[4:]
        tool = None
        if "--journal-tool" in rest:
            at = rest.index("--journal-tool")
            if at + 1 >= len(rest):
                sys.exit("holo_db: --journal-tool wants a path")
            tool = rest[at + 1]
            rest = rest[:at] + rest[at + 2:]
        return scan(argv[2], argv[3], rest[0] if rest else None, tool) and None
    if len(argv) in (3, 4) and argv[1] == "form":
        return form(Path(argv[2]), int(argv[3]) if len(argv) == 4 else 3600)
    if len(argv) == 3 and argv[1] == "models-header":
        return models_header(argv[2]) and None
    if len(argv) == 3 and argv[1] == "header":
        return header(argv[2]) and None
    sys.exit(__doc__ or "usage: holo_db.py scan|form|models-header|header|--self-test (see the file's head)")


if __name__ == "__main__":
    main(sys.argv)
