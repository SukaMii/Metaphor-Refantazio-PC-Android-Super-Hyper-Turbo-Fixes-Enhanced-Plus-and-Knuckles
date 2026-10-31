"""Summarise a [ABTest] Compare=quality log: the effect of each variant against the base.

Every profiler window is tagged [Qn name] (transitions are left out). For each metric:
    value = b0 + b1 * time + sum_v effect_v * (window runs variant v)
fitted over the windows, so slow drift (heating up, battery) is factored out. With
--steady, only windows at the most common GPU clock are used (a throttle step in the
middle of a run is a jump, not a drift).

Usage: python summarize_variants.py <log> [--steady] [--skip-min N]
"""
import argparse
import math
import re

TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
REPORT = re.compile(r"---- CPU report \[Q(\d+) ([\w.-]+)( transition)?\]: [\d.]+ s window, \d+ threads, process total (\d+)%")
WATT = re.compile(r"battery ([\d.]+) W")
GPU = re.compile(r"GPU ([\d.]+) MHz")
GBUSY = re.compile(r"busy ([\d.]+)%")
FRAMES = re.compile(r"frames: ([\d.]+) fps \| frame time avg ([\d.]+) ms, sd ([\d.]+) ms")
MAINCPU = re.compile(r"main thread CPU per frame: ([\d.]+) ms")
GPUMS = re.compile(r"GPU time per pass \(\d+ frames, timer [\d.]+ MHz\): busy ([\d.]+) ms/frame")
QUAL = re.compile(r"Quality: scene passes ([\d.]+) draws/frame \(([\d.]+)M indices\): characters ([\d.]+) \(([\d.]+)M, "
                  r"(\d+)% of indices\), outlines ([\d.]+) \(([\d.]+)M\)")


def secs(line):
    m = TS.match(line)
    if not m:
        return None
    h, mi, s, ms = map(int, m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def parse(path):
    wins, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = REPORT.search(line)
        if m:
            if cur:
                wins.append(cur)
            cur = {"t": secs(line), "v": int(m.group(1)), "name": m.group(2), "tr": bool(m.group(3)),
                   "cpu": float(m.group(4))}
            continue
        if not cur:
            continue
        for key, rx in (("W", WATT), ("mhz", GPU), ("busy", GBUSY), ("main", MAINCPU), ("gpums", GPUMS)):
            mm = rx.search(line)
            if mm and key not in cur:
                cur[key] = float(mm.group(1))
        mm = FRAMES.search(line)
        if mm:
            cur["fps"], cur["sd"] = float(mm.group(1)), float(mm.group(3))
        mm = QUAL.search(line)
        if mm:
            cur["draws"], cur["chars"], cur["charpct"] = float(mm.group(1)), float(mm.group(3)), float(mm.group(5))
    if cur:
        wins.append(cur)
    for w in wins:
        if "W" in w and w.get("fps"):
            w["mJ"] = w["W"] * 1000.0 / w["fps"]
        if "busy" in w and "mhz" in w and w.get("fps"):  # busy time scaled by the clock: comparable across clocks
            w["work"] = w["busy"] / 100.0 * 1000.0 / w["fps"] * w["mhz"] / 600.0
    return wins


def solve(a, b):
    """Gaussian elimination for the small normal equations."""
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def inverse(a):
    n = len(a)
    cols = []
    for j in range(n):
        e = [1.0 if i == j else 0.0 for i in range(n)]
        x = solve(a, e)
        if x is None:
            return None
        cols.append(x)
    return [[cols[j][i] for j in range(n)] for i in range(n)]


def fit(wins, key, variants):
    rows = [w for w in wins if key in w]
    if len(rows) < len(variants) + 3:
        return None
    t0 = rows[0]["t"]
    X, y = [], []
    for w in rows:
        X.append([1.0, (w["t"] - t0) / 60.0] + [1.0 if w["v"] == v else 0.0 for v in variants[1:]])
        y.append(w[key])
    p = len(X[0])
    xtx = [[sum(r[i] * r[j] for r in X) for j in range(p)] for i in range(p)]
    xty = [sum(r[i] * yy for r, yy in zip(X, y)) for i in range(p)]
    beta = solve(xtx, xty)
    inv = inverse(xtx)
    if beta is None or inv is None:
        return None
    res = [yy - sum(bi * xi for bi, xi in zip(beta, r)) for r, yy in zip(X, y)]
    dof = max(1, len(rows) - p)
    s2 = sum(r * r for r in res) / dof
    base = sum(w[key] for w in rows if w["v"] == variants[0]) / max(1, sum(1 for w in rows if w["v"] == variants[0]))
    out = {}
    for i, v in enumerate(variants[1:]):
        se = math.sqrt(max(0.0, s2 * inv[2 + i][2 + i]))
        out[v] = (beta[2 + i], 2.0 * se)
    return base, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--steady", action="store_true", help="only windows at the most common GPU clock")
    ap.add_argument("--skip-min", type=float, default=0.0, help="leave out the first N minutes")
    args = ap.parse_args()
    wins = [w for w in parse(args.log) if not w["tr"] and w.get("fps", 0) > 5]
    if not wins:
        print("no [Q..] windows found")
        return
    t_first = wins[0]["t"]
    wins = [w for w in wins if w["t"] - t_first >= args.skip_min * 60]
    if args.steady:
        clocks = {}
        for w in wins:
            if "mhz" in w:
                clocks[round(w["mhz"] / 20) * 20] = clocks.get(round(w["mhz"] / 20) * 20, 0) + 1
        if clocks:
            mode = max(clocks, key=clocks.get)
            wins = [w for w in wins if "mhz" in w and abs(w["mhz"] - mode) <= 20]
            print(f"steady windows: GPU {mode} MHz")
    names, first = {}, {}
    for w in wins:  # a variant that comes back in the round counts as one
        w["v"] = first.setdefault(w["name"], w["v"])
        names[w["v"]] = w["name"]
    variants = sorted(names)
    counts = {v: sum(1 for w in wins if w["v"] == v) for v in variants}
    print(f"{len(wins)} windows: " + ", ".join(f"{names[v]} {counts[v]}" for v in variants))
    metrics = [("W", "battery power W"), ("mJ", "energy per frame mJ"), ("fps", "fps"), ("mhz", "GPU MHz"),
               ("busy", "GPU busy %"), ("work", "GPU work ms at 600 MHz"), ("gpums", "GPU ms/frame (timing)"), ("cpu", "game CPU % of a core"), ("main", "main thread ms/frame"),
               ("sd", "frame time sd ms"), ("chars", "character draws/frame")]
    wide = len(variants) > 6  # many variants: one block per metric, a line per variant
    if not wide:
        print(f"   {'metric':24} {'base':>8}  " + "  ".join(f"{names[v][:18]:>24}" for v in variants[1:]))
    for key, label in metrics:
        r = fit(wins, key, variants)
        if not r:
            continue
        base, eff = r
        if wide:
            print(f"   {label}: {names[variants[0]]} {base:.2f}")
            for v in variants[1:]:
                e, ci = eff[v]
                print(f"      {names[v]:28} {e:+7.2f} ({(e * 100.0 / base if base else 0.0):+4.0f}%) +-{ci:5.2f}")
            continue
        cells = []
        for v in variants[1:]:
            e, ci = eff[v]
            pct = e * 100.0 / base if base else 0.0
            cells.append(f"{e:+7.2f} ({pct:+4.0f}%) +-{ci:5.2f}")
        print(f"   {label:24} {base:8.2f}  " + "  ".join(f"{c:>24}" for c in cells))


if __name__ == "__main__":
    main()
