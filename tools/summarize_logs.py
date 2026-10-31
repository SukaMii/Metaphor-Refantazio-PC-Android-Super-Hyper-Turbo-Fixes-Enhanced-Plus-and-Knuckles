"""Summarise MetaphorCPU logs for power / pacing tests.

Window selection: by default the LAST minutes of each log (up to 12 for A/B sessions, 4
for single runs), ignoring the final 20 s (quitting) and the first 3 minutes after the
game starts drawing (title, loading, walking to the spot). Stand still at the test
spot, then quit; no need to note times. Override with --last N (minutes).

A/B session logs ([ABTest] Enabled=1): every metric is fitted as
    value = b0 + b1 * time + effect * (side B)
over the per-period averages, so slow drift (heating up, battery draining) is
factored out. Prints A, B, the B-A effect and its 95% confidence interval.

Separate runs (several logs): averages per log, plus power adjusted to a common
battery temperature using the within-run slope of power vs. temperature.

Usage:
  python summarize_logs.py session_AB.log
  python summarize_logs.py 1_C.log 2_D.log 3_D.log 4_C.log [--last 4]
"""
import argparse
import re
import statistics

TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]")
REPORT = re.compile(r"---- CPU report(?: \[AB ([AB])( transition)?\])?: [\d.]+ s window, \d+ threads, "
                    r"process total (\d+)% of one core")
DEVICE = re.compile(r"device: (.*)")
WATT = re.compile(r"battery ([\d.]+) W")
BTEMP = re.compile(r"battery ([\d.]+) C")
GPU = re.compile(r"GPU ([\d.]+) MHz")
GBUSY = re.compile(r"busy ([\d.]+)%")
CLUSTER = re.compile(r"(cpu\d+-\d+) ([\d.]+) GHz")
ZONE = re.compile(r"\| ([A-Za-z][\w-]*) ([\d.]+) C")
FRAMES = re.compile(r"frames: ([\d.]+) fps \| frame time avg ([\d.]+) ms, sd ([\d.]+) ms, worst ([\d.]+) ms")
SPREAD = re.compile(r"frame-time spread: .*?36-50 ([\d.]+)% \| 50-100 ([\d.]+)% \| >100 ([\d.]+)%")
LINUX = re.compile(r"linux threads: \d+, total (\d+)% of a core, (\d+) wake-ups/s")
MAINCPU = re.compile(r"main thread CPU per frame: ([\d.]+) ms")
WAITS = re.compile(r"main thread per frame \([\d.]+ ms\): CPU [\d.]+ \| sleeps ([\d.]+) \([\d.]+x\) \| event waits "
                   r"([\d.]+) \([\d.]+x\) \| critical sections ([\d.]+) \([\d.]+x\) \| yields ([\d.]+) \([\d.]+x\) \| "
                   r"other ([\d.]+) ms")
SETTINGS = re.compile(r"Settings: (.*)")
MODE = re.compile(r"Mode: (\w+)")

METRICS = [  # key, label, format, lower-is-better
    ("W", "battery power W", "{:.2f}", True),
    ("mJ", "energy per frame mJ", "{:.0f}", True),
    ("gpu", "GPU MHz", "{:.0f}", True),
    ("gbusy", "GPU busy %", "{:.0f}", True),
    ("proc", "game CPU % of a core", "{:.0f}", True),
    ("linux", "linux CPU % of a core", "{:.0f}", True),
    ("wake", "wake-ups/s", "{:.0f}", True),
    ("maincpu", "main thread ms/frame", "{:.1f}", True),
    ("w_sleep", "main: sleeps ms/frame", "{:.1f}", True),
    ("w_event", "main: event waits ms/frame", "{:.1f}", True),
    ("w_cs", "main: crit. sections ms/frame", "{:.1f}", True),
    ("w_yield", "main: yields ms/frame", "{:.1f}", True),
    ("w_other", "main: other ms/frame", "{:.1f}", True),
    ("fps", "fps", "{:.2f}", False),
    ("sd", "frame time sd ms", "{:.2f}", True),
    ("slow", "frames >36 ms %", "{:.1f}", True),
]

T95 = [12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23, 2.20, 2.18, 2.16, 2.14, 2.13, 2.12, 2.11,
       2.10, 2.09, 2.09, 2.08, 2.07, 2.07, 2.06, 2.06, 2.06, 2.05, 2.05, 2.05, 2.04]


def t95(df):
    return T95[df - 1] if 1 <= df <= len(T95) else 1.96


def parse(path):
    """Returns (windows, info). Each window: dict with time, side, transition and metrics."""
    windows, info = [], {"mode": "?", "settings": ""}
    cur, day, last_t = None, 0, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TS.match(line)
            if not m:
                continue
            h, mi, s, ms = (int(x) for x in m.groups())
            t = h * 3600 + mi * 60 + s + ms / 1000 + day
            if last_t is not None and t < last_t - 3600:
                day += 86400
                t += 86400
            last_t = t
            if (x := MODE.search(line)):
                info["mode"] = x.group(1)
            if (x := SETTINGS.search(line)):
                info["settings"] = x.group(1)
            if (x := REPORT.search(line)):
                cur = {"t": t, "side": x.group(1), "transition": bool(x.group(2)), "proc": float(x.group(3)),
                       "clusters": {}, "zones": {}}
                windows.append(cur)
                continue
            if cur is None:
                continue
            if (x := DEVICE.search(line)):
                d = x.group(1)
                if (y := WATT.search(d)) and "CHARGING" not in d:
                    cur["W"] = float(y.group(1))
                if (y := BTEMP.search(d)):
                    cur["btemp"] = float(y.group(1))
                if (y := GPU.search(d)):
                    cur["gpu"] = float(y.group(1))
                if (y := GBUSY.search(d)):
                    cur["gbusy"] = float(y.group(1))
                for name, ghz in CLUSTER.findall(d):
                    cur["clusters"][name] = float(ghz)
                for name, c in ZONE.findall(d):
                    if name != "battery":
                        cur["zones"][name] = float(c)
            elif (x := FRAMES.search(line)):
                cur["fps"], cur["sd"], cur["worst"] = float(x.group(1)), float(x.group(3)), float(x.group(4))
            elif (x := SPREAD.search(line)):
                cur["slow"] = sum(float(v) for v in x.groups())
            elif (x := LINUX.search(line)):
                cur["linux"], cur["wake"] = float(x.group(1)), float(x.group(2))
            elif (x := MAINCPU.search(line)):
                cur["maincpu"] = float(x.group(1))
            elif (x := WAITS.search(line)):
                for key, v in zip(("w_sleep", "w_event", "w_cs", "w_yield", "w_other"), x.groups()):
                    cur[key] = float(v)
    for w in windows:  # energy per frame: the fair efficiency metric when fps is below the cap
        if "W" in w and w.get("fps"):
            w["mJ"] = w["W"] * 1000.0 / w["fps"]
    return windows, info


def select(windows, last_min, skip_min=3.0):
    """Windows in the last `last_min` minutes, ignoring the final 20 s (quitting) and the
    first `skip_min` minutes after the first frame (title screen, loading, walking)."""
    framed = [w for w in windows if "fps" in w]
    if not framed:
        return []
    end = framed[-1]["t"] - 20
    start = max(end - last_min * 60, framed[0]["t"] + skip_min * 60)
    return [w for w in framed if start <= w["t"] <= end]


def mean(vals):
    return statistics.mean(vals) if vals else None


def fmt(v, f):
    return f.format(v) if v is not None else "-"


def solve3(a, b):
    """Solve a 3x3 linear system (Gaussian elimination). Returns None if singular."""
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(3):
        p = max(range(c, 3), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for r in range(3):
            if r != c:
                k = m[r][c] / m[c][c]
                for j in range(c, 4):
                    m[r][j] -= k * m[c][j]
    return [m[i][3] / m[i][i] for i in range(3)]


def ols_effect(points):
    """points: (t, isB, y). Fit y = b0 + b1 t + b2 isB. Returns (b2, half-width 95% CI) or None."""
    n = len(points)
    if n < 5 or len({p[1] for p in points}) < 2:
        return None
    t0 = statistics.mean(p[0] for p in points)
    xs = [(1.0, (p[0] - t0) / 60.0, float(p[1])) for p in points]
    ys = [p[2] for p in points]
    xtx = [[sum(x[i] * x[j] for x in xs) for j in range(3)] for i in range(3)]
    xty = [sum(x[i] * y for x, y in zip(xs, ys)) for i in range(3)]
    beta = solve3(xtx, xty)
    if beta is None:
        return None
    rss = sum((y - sum(b * xi for b, xi in zip(beta, x))) ** 2 for x, y in zip(xs, ys))
    df = n - 3
    s2 = rss / df if df > 0 else 0.0
    e = [0.0, 0.0, 1.0]
    col = solve3(xtx, e)  # third column of (X'X)^-1
    if col is None:
        return None
    se = (s2 * col[2]) ** 0.5
    return beta[2], t95(df) * se


def periods(windows):
    """Group consecutive non-transition windows of the same side into periods."""
    out, cur = [], None
    for w in windows:
        if w["side"] is None or w["transition"]:
            cur = None
            continue
        if cur is None or cur["side"] != w["side"]:
            cur = {"side": w["side"], "w": []}
            out.append(cur)
        cur["w"].append(w)
    return out


def ab_report(path, windows, last_min):
    sel = select(windows, last_min)
    ps = periods(sel)
    na = sum(1 for p in ps if p["side"] == "A")
    nb = sum(1 for p in ps if p["side"] == "B")
    print(f"\n== {path}: A/B session, last {last_min} min, {na} A periods + {nb} B periods "
          f"({sum(len(p['w']) for p in ps)} windows)")
    if na < 2 or nb < 2:
        print("   not enough periods: stand still longer (at least 4 switches) or use --last")
    keys = [(k, l, f, lo) for k, l, f, lo in METRICS]
    clusters = sorted({c for p in ps for w in p["w"] for c in w["clusters"]})
    for c in clusters:
        keys.insert(3, ("cl:" + c, f"{c} GHz", "{:.2f}", True))
    print(f"   {'metric':24} {'A (as-is)':>10} {'B (fixes)':>10} {'B-A':>9} {'95% CI':>17}  result")
    for key, label, f, lower_better in keys:
        pts, a_vals, b_vals = [], [], []
        for p in ps:
            if key.startswith("cl:"):
                vals = [w["clusters"][key[3:]] for w in p["w"] if key[3:] in w["clusters"]]
            else:
                vals = [w[key] for w in p["w"] if key in w]
            if not vals:
                continue
            m = statistics.mean(vals)
            t = statistics.mean(w["t"] for w in p["w"])
            pts.append((t, p["side"] == "B", m))
            (b_vals if p["side"] == "B" else a_vals).append(m)
        if not a_vals or not b_vals:
            continue
        eff = ols_effect(pts)
        if eff:
            d, hw = eff
            lo, hi = d - hw, d + hw
            scale = abs(mean(a_vals)) + 1.0
            if abs(d) < 1e-6 * scale and hw < 1e-6 * scale:
                verdict = "no difference"
            elif lo > 0 or hi < 0:
                better = (d < 0) == lower_better
                verdict = "B better" if better else "B worse"
            else:
                verdict = "no clear difference"
            print(f"   {label:24} {fmt(mean(a_vals), f):>10} {fmt(mean(b_vals), f):>10} {fmt(d, '{:+.2f}'):>9} "
                  f"[{lo:+7.2f},{hi:+7.2f}]  {verdict}")
        else:
            print(f"   {label:24} {fmt(mean(a_vals), f):>10} {fmt(mean(b_vals), f):>10}")


def sessions_table(ab_logs, last_min):
    """Side-by-side A and B averages of several A/B sessions (e.g. without / with the fast
    semaphore), so effects that are fixed per session can be compared."""
    print(f"\n== A/B sessions side by side (last {last_min} min, transitions excluded)")
    print("   session                          side      W   mJ/frame   fps   GPU MHz  busy%  game%  wake/s  batt C")
    for path, windows in ab_logs:
        sel = [w for w in select(windows, last_min) if w["side"] and not w["transition"]]
        for side in ("A", "B"):
            ws = [w for w in sel if w["side"] == side]
            if not ws:
                continue
            def m(k, f):
                return fmt(mean([w[k] for w in ws if k in w]), f)
            name = path.replace("\\", "/").split("/")[-1]
            print(f"   {name:32} {side:>4} {m('W', '{:6.2f}')} {m('mJ', '{:8.0f}')} {m('fps', '{:6.2f}')} "
                  f"{m('gpu', '{:8.0f}')} {m('gbusy', '{:6.0f}')} {m('proc', '{:6.0f}')} {m('wake', '{:7.0f}')} "
                  f"{m('btemp', '{:6.1f}')}")


def runs_report(paths, parsed, last_min):
    rows = []
    for path, (windows, info) in zip(paths, parsed):
        sel = select(windows, last_min)
        r = {"name": path.replace("\\", "/").split("/")[-1], "info": info, "sel": sel}
        for key, *_ in METRICS:
            r[key] = mean([w[key] for w in sel if key in w])
        r["btemp"] = mean([w["btemp"] for w in sel if "btemp" in w])
        r["btemp_end"] = sel[-1].get("btemp") if sel else None
        r["worst"] = max((w["worst"] for w in sel if "worst" in w), default=None)
        rows.append(r)
    print(f"\n== separate runs, last {last_min} min of each")
    print("   " + " | ".join(["log", "mode", "win", "W", "mJ/frame", "batt C avg/end", "GPU MHz", "busy%", "game%",
                             "linux%", "wake/s", "main ms", "fps", "sd", "worst", ">36ms%"]))
    for r in rows:
        print("   " + " | ".join([
            r["name"], r["info"]["mode"], str(len(r["sel"])), fmt(r["W"], "{:.2f}"), fmt(r["mJ"], "{:.0f}"),
            f"{fmt(r['btemp'], '{:.1f}')}/{fmt(r['btemp_end'], '{:.1f}')}", fmt(r["gpu"], "{:.0f}"),
            fmt(r["gbusy"], "{:.0f}"), fmt(r["proc"], "{:.0f}"), fmt(r["linux"], "{:.0f}"), fmt(r["wake"], "{:.0f}"),
            fmt(r["maincpu"], "{:.1f}"), fmt(r["fps"], "{:.2f}"), fmt(r["sd"], "{:.2f}"), fmt(r["worst"], "{:.1f}"),
            fmt(r["slow"], "{:.1f}")]))
    # temperature adjustment: pooled within-run slope of power vs battery temperature
    num = den = 0.0
    for r in rows:
        pts = [(w["btemp"], w["W"]) for w in r["sel"] if "btemp" in w and "W" in w]
        if len(pts) < 3:
            continue
        tm = statistics.mean(p[0] for p in pts)
        wm = statistics.mean(p[1] for p in pts)
        num += sum((t - tm) * (wv - wm) for t, wv in pts)
        den += sum((t - tm) ** 2 for t, _ in pts)
    temps = [r["btemp"] for r in rows if r["btemp"] is not None and r["W"] is not None]
    if den > 0 and temps and len(rows) > 1:
        slope = num / den
        tref = statistics.mean(temps)
        if abs(slope) > 1.0:
            print(f"   power vs temperature slope {slope:+.2f} W/C is implausible; no adjustment made")
            return
        print(f"   power adjusted to a common battery temperature of {tref:.1f} C "
              f"(within-run slope {slope:+.3f} W per C):")
        ref = None
        for r in rows:
            if r["W"] is None or r["btemp"] is None:
                continue
            adj = r["W"] - slope * (r["btemp"] - tref)
            ref = adj if ref is None else ref
            print(f"      {r['name']:32} {adj:6.2f} W   ({adj - ref:+.2f} W vs {rows[0]['name']})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--last", type=float, default=None, help="minutes analysed at the end of each log")
    a = ap.parse_args()
    parsed = [parse(p) for p in a.logs]
    singles, ab_logs = [], []
    for path, (windows, info) in zip(a.logs, parsed):
        if any(w["side"] for w in windows):
            ab_report(path, windows, a.last or 12)
            ab_logs.append((path, windows))
        else:
            singles.append((path, (windows, info)))
    if len(ab_logs) > 1:
        sessions_table(ab_logs, a.last or 12)
    if singles:
        runs_report([s[0] for s in singles], [s[1] for s in singles], a.last or 4)


if __name__ == "__main__":
    main()
