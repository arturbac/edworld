"""ed_perf_report.py <raw dir> <result.md>: Markdown tables of where the game's CPU time goes, by module.

Inputs in the raw dir (made by ed_perf.sh): maps.txt (/proc/pid/maps), samples.txt (perf script -F comm,tid,ip),
cpu-before.tsv / cpu-after.tsv (tid, comm, utime+stime ticks), info.txt, frames-before.txt / frames-after.txt
(the EHT overlay's "frames <n> seconds <s>"; older raw dirs have none) and edworld-frames.txt (the fallback).
Wine copies PE sections into anonymous memory; only the header page carries the file name, so anonymous
mappings that directly follow a file mapping belong to that module.
CPU of a thread group = its /proc ticks over the wall time; a module's share in it = its share of the group's
samples. Self time only (no call stacks): samples right after a hook forwards a D3D call count to the hook.
Writes only <result.md>; refuses to overwrite it.
"""
import bisect
import collections
import os
import re
import sys
from datetime import datetime

LINE_RE = re.compile(r"^\s*(.*?)\s+(\d+)\s+([0-9a-f]+)\s*$")
THREADS = ("Render", "dxvk-cs", "Worker*", "RunLoop")
UNATTRIBUTED = "unattributed (anonymous / JIT)"
KERNEL = "kernel (system calls)"
VDSO = "vdso (clock reads)"
ACCOUNTS = {
    "/ext/artur/.local/share/Steam/steamapps/compatdata/359320/pfx": "Sjona (Steam)",
    "/ext/artur/ed-frontier/compatdata/359320/pfx": "Dunkan (AltED)",
}


def read(raw, name):
    with open(os.path.join(raw, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def label(path, edloader):
    """Module group of a mapped file."""
    base = os.path.basename(path).lower()
    low = path.lower()
    if base == "elitedangerous64.exe":
        return "ED (EliteDangerous64.exe)"
    if "/edloader/plugins/" in low:
        if "edhm" in base:
            return "EDHM (" + os.path.basename(path) + ")"
        if "edvr" in base:
            return "EDVR (" + os.path.basename(path) + ")"
        if base.startswith("edworld"):
            return "edworld (" + os.path.basename(path) + ")"
        return "plugin " + os.path.basename(path)
    if "/products/" in low:
        if base == "d3d11.dll":
            return "edloader (game d3d11.dll)" if edloader else "game d3d11.dll (proxy, not edloader)"
        return "ED (other game dlls)"
    if base in ("d3d11.dll", "dxgi.dll", "d3d10core.dll", "d3d9.dll") or "/dxvk/" in low:
        return "DXVK"
    if base.startswith(("libvulkan_", "libdrm", "libllvm")) or "radeon" in base or "nvidia" in base:
        return "GPU driver"
    if "/wine/" in low or "/pfx/drive_c/windows/" in low or base.endswith((".dll", ".so")) and "wine" in low:
        return "wine / Proton"
    return "system libs (" + os.path.basename(path) + ")" if base.startswith("lib") else "other"


def module_ranges(raw):
    maps = read(raw, "maps.txt")
    edloader = "/edloader/plugins/" in maps.lower()
    ranges, cur, prev_end = [], None, None
    for line in maps.splitlines():
        parts = line.split(None, 5)
        lo, hi = (int(x, 16) for x in parts[0].split("-"))
        path = parts[5].strip() if len(parts) == 6 else ""
        if path.startswith("/"):
            cur = path
        elif path or prev_end != lo:
            cur = None
        prev_end = hi
        if cur:
            ranges.append((lo, hi, label(cur, edloader)))
        elif path == "[vdso]":
            ranges.append((lo, hi, VDSO))
    ranges.sort()
    return ranges, maps


def group_libs(name):
    # one row for all system libs, the file names are noise in the table
    return "system libs" if name.startswith("system libs") else name


def overlay_fps(raw):
    """fps from the EHT overlay's frame count read before and after the window, or None."""
    pts = []
    for name in ("frames-before.txt", "frames-after.txt"):
        path = os.path.join(raw, name)
        m = re.match(r"frames (\d+) seconds ([0-9.]+)", read(raw, name)) if os.path.exists(path) else None
        if not m:
            return None
        pts.append((int(m.group(1)), float(m.group(2))))
    dt = pts[1][1] - pts[0][1]
    return (pts[1][0] - pts[0][0]) / dt if dt > 0 and pts[1][0] >= pts[0][0] else None


def fps_of(raw, t0, t1):
    fr = re.findall(r"^(\S+) frame (\d+):", read(raw, "edworld-frames.txt"), re.M)
    pts = [(datetime.fromisoformat(t.replace("Z", "+00:00")), int(n)) for t, n in fr]
    pts = [p for p in pts if t0 <= p[0] <= t1]
    if len(pts) < 2:
        return None
    dt = (pts[-1][0] - pts[0][0]).total_seconds()
    return (pts[-1][1] - pts[0][1]) / dt if dt > 0 else None


def fps_and_source(raw, t0, t1):
    fps = overlay_fps(raw)
    if fps:
        return dict(fps=fps, fps_source="EHT overlay frame count")
    fps = fps_of(raw, t0, t1)
    return dict(fps=fps, fps_source="edworld frame lines" if fps else None)


def ticks(raw, name):
    out = {}
    for line in read(raw, name).splitlines():
        p = line.split("\t")
        if len(p) == 3 and p[2].strip():
            out[p[0]] = (p[1], int(p[2]))
    return out


def group(comm):
    return "Worker*" if comm.startswith("Worker") else comm


def analyse(raw):
    ranges, maps = module_ranges(raw)
    starts = [r[0] for r in ranges]

    def module(ip):
        if ip >= 0xFFFF800000000000:
            return KERNEL
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < ranges[i][1]:
            return group_libs(ranges[i][2])
        return UNATTRIBUTED

    samples = collections.Counter()
    mods = collections.defaultdict(collections.Counter)
    for line in read(raw, "samples.txt").splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        g = group(m.group(1))
        samples[g] += 1
        mods[g][module(int(m.group(3), 16))] += 1

    info = read(raw, "info.txt")
    wall = float(re.search(r"wall ([0-9.]+)", info).group(1))
    tck = int(re.search(r"clk_tck (\d+)", info).group(1))
    b, a = ticks(raw, "cpu-before.tsv"), ticks(raw, "cpu-after.tsv")
    load = collections.defaultdict(list)
    for t in a:
        if t in b:
            load[group(a[t][0])].append((a[t][1] - b[t][1]) / tck / wall)
    t0 = datetime.fromisoformat(re.search(r"start (\S+)", info).group(1))
    t1 = datetime.fromisoformat(re.search(r" end (\S+)", info).group(1))
    prefix = next((v for k, v in ACCOUNTS.items() if k in maps), "unknown prefix")

    # ms of CPU per second of each (thread group, module); groups without samples count to none
    ms = collections.defaultdict(collections.Counter)
    for g, n in samples.items():
        cpu = sum(load.get(g, [0])) * 1000
        for mod, c in mods[g].items():
            ms[g][mod] = c / n * cpu
    total = collections.Counter()
    for g in ms:
        total.update(ms[g])
    return dict(info=info, wall=wall, samples=samples, load=load, ms=ms, total=total,
                **fps_and_source(raw, t0, t1), account=prefix,
                pid=re.search(r"pid (\d+)", info).group(1), start=t0)


def cell(value, whole):
    if value < 0.005:
        return ""
    return f"{value:.1f} ({value * 100 / whole:.0f}%)" if whole > 0 else f"{value:.1f}"


def write_md(r, out):
    lines = []
    w = lines.append
    w(f"# ED CPU profile {r['start'].strftime('%Y-%m-%d %H:%M:%S')}")
    w("")
    w(f"- account: {r['account']}, pid {r['pid']}")
    w(f"- window: {r['wall']:.1f} s, {sum(r['samples'].values())} samples (perf, 1999 Hz per thread)")
    render_busy = sum(r["load"].get("Render", [0]))
    if r["fps"]:
        w(f"- fps {r['fps']:.1f} ({r['fps_source']}) = {1000 / r['fps']:.1f} ms per frame; "
          f"Render thread busy {render_busy * 100:.0f}% = {render_busy * 1000 / r['fps']:.1f} ms of CPU per frame")
    else:
        w(f"- fps: unknown (no EHT overlay frame count, no edworld frame lines in the window); Render thread busy {render_busy * 100:.0f}%")
    w("")
    w("## CPU by module")
    w("")
    w("ms of CPU per second of wall time (1000 = one full core); in brackets the share of that column. "
      "Worker\\* = all Worker threads together, all threads = every thread of the game.")
    w("")
    cols = [g for g in THREADS if g in r["ms"]]
    head = ["module"] + cols + ["all threads"]
    if r["fps"]:
        head.append("Render ms/frame")
    w("| " + " | ".join(head) + " |")
    w("|" + "|".join(["---"] + ["---:"] * (len(head) - 1)) + "|")
    col_sum = {g: sum(r["ms"][g].values()) for g in cols}
    all_sum = sum(r["total"].values())
    for mod, v in r["total"].most_common():
        if v < 0.5 and not any(r["ms"][g][mod] >= 0.5 for g in cols):
            continue
        row = [mod] + [cell(r["ms"][g][mod], col_sum[g]) for g in cols] + [cell(v, all_sum)]
        if r["fps"]:
            render = r["ms"].get("Render", {}).get(mod, 0)
            row.append(f"{render / r['fps']:.2f}" if render >= 0.005 else "")
        w("| " + " | ".join(row) + " |")
    row = ["**sum**"] + [f"**{col_sum[g]:.0f}**" for g in cols] + [f"**{all_sum:.0f}**"]
    if r["fps"]:
        row.append(f"**{col_sum.get('Render', 0) / r['fps']:.2f}**")
    w("| " + " | ".join(row) + " |")
    w("")
    w("## Threads by CPU load")
    w("")
    w("| thread | % of one core | threads |")
    w("|---|---:|---:|")
    for g, ls in sorted(r["load"].items(), key=lambda x: -sum(x[1]))[:12]:
        w(f"| {g} | {sum(ls) * 100:.1f} | {len(ls)} |")
    w("")
    w("Notes: self time only, no call stacks. Samples taken right after a hook (EDHM, EDVR, edworld, edloader) "
      "forwards a D3D call are counted to the hook, so hook shares can be somewhat high. Rows under 0.5 ms/s "
      "are left out of the table but counted in the sums. With the game at its frame limit, the Render thread "
      "waits for the next frame in wine (ntdll.so) and vdso clock reads, so those rows there are mostly "
      "waiting, not work.")
    w("")
    with open(out, "x", encoding="utf-8") as f:
        f.write("\n".join(lines))


if len(sys.argv) != 3:
    sys.exit("usage: ed_perf_report.py <raw dir> <result.md>")
if os.path.exists(sys.argv[2]):
    sys.exit(f"error: {sys.argv[2]} already exists")
write_md(analyse(sys.argv[1]), sys.argv[2])
