#!/usr/bin/env python3
"""analyze.py RAWDIR : the contend2 tables (markdown on stdout) from the
campaign's serial logs RAWDIR/IMG-N.log (+ .irq-before/.irq-after).

Units: ipcbench figures are its own CPU cycles. Lock figures (TSC counts) are
turned into cycles with the contended line's ruler of the same boot:
12000 / mean(ruler min, ruler max) cycles per count."""
import os, re, sys, statistics as st

raw = sys.argv[1]
CONT = "two address spaces, passive server, every other CPU making system calls"
LINES = ["a system call that does nothing", "two address spaces, passive server",
         "one address space, passive server", "two address spaces, server with its own context",
         "two address spaces, passive server, 4 KiB of IPC buffer each way", CONT]
SHORT = ["null call", "2 AS passive", "1 AS passive", "2 AS own ctx", "2 AS 4 KiB", "contended"]

def parse(path):
    b = {"lines": {}, "cpus": {}, "depth": {}}
    for l in open(path, errors="replace"):
        l = l.rstrip("\n")
        m = re.match(r"ipcbench: (.*): n=\d+x\d+ min=(\d+) p50=(\d+) p99=(\d+) worst-p50=(\d+) max=(\d+) "
                     r"cycles \(bucket (\d+); 12000 cycles took (\d+) to (\d+) counts", l)
        if m:
            b["lines"][m.group(1)] = dict(min=int(m.group(2)), p50=int(m.group(3)), p99=int(m.group(4)),
                                          worst=int(m.group(5)), max=int(m.group(6)), width=int(m.group(7)),
                                          rmin=int(m.group(8)), rmax=int(m.group(9)))
            continue
        if l.startswith("ipcbench-rounds: p50"):
            b["rp50"] = [int(x) for x in l.split()[2:]]
        elif l.startswith("ipcbench-rounds: p99"):
            b["rp99"] = [int(x) for x in l.split()[2:]]
        elif l.startswith("ipcbench-hist: width"):
            m = re.match(r"ipcbench-hist: width (\d+) from (\d+) to (\d+)", l)
            b["hw"], b["hlo"], b["hhi"] = map(int, m.groups())
            b["hseg"] = {}
        elif l.startswith("ipcbench-hist: seg") or l.startswith("ipcbench-hist: all"):
            body, above = l.split("|")
            f = body.split()
            if f[1] == "seg":
                key, vals = int(f[2]), f[3:]
            else:
                key, vals = "all", f[2:]
            b["hseg"][key] = ([int(x) for x in vals], int(above))
        elif l.startswith("lockstats: caller cpu"):
            b["caller"] = int(l.split()[-1])
        elif l.startswith("lockstats: window"):
            b["window"] = int(l.split()[2])
        elif l.startswith("lockstats: cpu"):
            f = l.split()
            c = int(f[2]); d = {}
            i = 3
            while i < len(f):
                k = f[i]
                if k == "depth":
                    d["depth"] = [tuple(map(int, x.split("/"))) for x in f[i + 1:i + 9]]
                    i += 9
                    continue
                v = f[i + 1]
                d[k] = tuple(map(int, v.split("/"))) if "/" in v else int(v)
                i += 2
            b["cpus"][c] = d
        elif l.startswith("lockdepth: caller cpu"):
            b["dcaller"] = int(l.split()[-1])
        elif l.startswith("lockdepth: seg"):
            f = l.split()
            sg, c, n = int(f[2]), int(f[4]), int(f[6])
            b["depth"].setdefault(sg, {})[c] = (n, [int(x) for x in f[8:12]])
        elif l.startswith("pin: CPU"):
            b.setdefault("pins", []).append(l)
    if CONT in b["lines"]:
        c = b["lines"][CONT]
        b["cpc"] = 12000.0 / ((c["rmin"] + c["rmax"]) / 2.0)  # cycles per TSC count
    # IRQs on host CPUs 2-5 over the boot
    try:
        def irqs(p):
            t = [0, 0, 0, 0]
            for l in open(p):
                f = l.split()
                if not f or not f[0].endswith(":") or f[0] == "CPU0":
                    continue
                for j in range(4):
                    if len(f) > 3 + j and f[3 + j].isdigit():
                        t[j] += int(f[3 + j])
            return t
        a, z = irqs(path[:-4] + ".irq-before"), irqs(path[:-4] + ".irq-after")
        b["irq"] = sum(z) - sum(a)
    except OSError:
        pass
    return b

def boots(img):
    out = []
    for f in os.listdir(raw):
        m = re.match(re.escape(img) + r"-(\d+)\.log$", f)
        if m:
            out.append((int(m.group(1)), parse(os.path.join(raw, f))))
    return sorted(out)

def spread(v):
    return 100.0 * (max(v) - min(v)) / st.median(v) if v else float("nan")

def pearson(x, y):
    if len(x) < 3 or st.pstdev(x) == 0 or st.pstdev(y) == 0:
        return float("nan")
    mx, my = st.mean(x), st.mean(y)
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (len(x) * st.pstdev(x) * st.pstdev(y))

def windows10(v):
    """spreads of every 10-boot window, in boot order"""
    return [spread(v[i:i + 10]) for i in range(0, len(v) - 9)]

def hq(counts, lo, w, q):
    """quantile q of a coarse histogram (bucket midpoints)"""
    tot = sum(counts)
    s = 0
    for i, c in enumerate(counts):
        s += c
        if s >= q * tot:
            return lo + (i + 0.5) * w
    return lo + len(counts) * w

def peaks(counts, lo, w):
    """modes of a coarse histogram: [1,2,1]-smoothed local maxima holding >= 0.5 % of
    the samples, with a dip of at least 20 % below the smaller of two neighbours"""
    tot = sum(counts)
    sm = [(counts[max(i - 1, 0)] + 2 * counts[i] + counts[min(i + 1, len(counts) - 1)]) / 4.0
          for i in range(len(counts))]
    cand = [i for i in range(len(sm)) if sm[i] >= 0.005 * tot
            and (i == 0 or sm[i] >= sm[i - 1]) and (i == len(sm) - 1 or sm[i] > sm[i + 1])]
    keep = []
    for i in cand:
        if keep:
            j = keep[-1]
            dip = min(sm[j:i + 1])
            if dip > 0.8 * min(sm[i], sm[j]):
                if sm[i] > sm[j]:
                    keep[-1] = i
                continue
        keep.append(i)
    return [(int(lo + (i + 0.5) * w), round(100.0 * sum(counts[max(i - 2, 0):i + 3]) / tot, 1)) for i in keep]

def fmt(x, d=0):
    if isinstance(x, float) and x != x:
        return "-"
    return f"{x:.{d}f}" if isinstance(x, float) else str(x)

def table(head, rows):
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for r in rows:
        print("| " + " | ".join(fmt(x) if not isinstance(x, str) else x for x in r) + " |")
    print()

def contended(b):
    return b["lines"][CONT]

def summary_line(img):
    bs = boots(img)
    v = [contended(b)["p50"] for _, b in bs if CONT in b["lines"]]
    w = [contended(b)["worst"] for _, b in bs if CONT in b["lines"]]
    return bs, v, w

# ---------------------------------------------------------------- every line
print("## Every line, per image: p50 median / min-max / spread (p99 likewise)\n")
imgs = sys.argv[2:] if len(sys.argv) > 2 else ["gate", "base"]
for img in imgs:
    bs = boots(img)
    rows = []
    for name, sh in zip(LINES, SHORT):
        p50 = [b["lines"][name]["p50"] for _, b in bs if name in b["lines"]]
        p99 = [b["lines"][name]["p99"] for _, b in bs if name in b["lines"]]
        if not p50:
            continue
        rows.append([sh, f"{st.median(p50):.0f} / {min(p50)}-{max(p50)} / {spread(p50):.1f} %",
                     f"{st.median(p99):.0f} / {min(p99)}-{max(p99)} / {spread(p99):.1f} %"])
    print(f"**{img}** ({len(bs)} boots)\n")
    table(["line", "p50", "p99"], rows)

# ---------------------------------------------------------------- item 1
def item1(img):
    bs = boots(img)
    print(f"### {img}: per boot (contended line, 128-cycle histogram)\n")
    rows = []
    allr = []
    for n, b in bs:
        c = contended(b)
        r = b.get("rp50", [])
        allr.append(r)
        h, ab = b["hseg"]["all"]
        lo, w = b["hlo"], b["hw"]
        pk = peaks(h, lo, w)
        # per-round shape: p10/p50/p90 from the round's coarse histogram
        q10 = [hq(b["hseg"][s][0], lo, w, 0.10) for s in range(20)]
        q90 = [hq(b["hseg"][s][0], lo, w, 0.90) for s in range(20)]
        rows.append([n, c["p50"], c["p99"], c["min"], c["worst"],
                     f"{min(r)}-{st.median(r):.0f}-{max(r)}", f"{spread(r):.1f} %",
                     f"{st.pstdev(r):.0f}",
                     f"{pearson(r, q10):+.2f} / {pearson(r, q90):+.2f}",
                     "; ".join(f"{p} ({s} %)" for p, s in pk), b.get("irq", "-")])
    table(["boot", "p50", "p99", "min", "worst round p50", "round p50s min-med-max",
           "round spread", "round SD", "r(round p50, round p10 / p90)",
           "modes of the pooled histogram (cycles, share within ±2 buckets)", "host IRQs CPUs 2-5"], rows)
    # variance decomposition of round p50s
    flat = [x for r in allr for x in r]
    within = st.mean([st.pvariance(r) for r in allr])
    between = st.pvariance([st.mean(r) for r in allr])
    print(f"Round p50s, all boots: {len(flat)} rounds, {min(flat)}-{max(flat)}; SD within a boot "
          f"{within ** 0.5:.0f}, SD of the boots' round means {between ** 0.5:.0f} cycles.\n")
    print("Round p50s in order, per boot:\n")
    table(["boot", "rounds 1-20"], [[n, " ".join(map(str, b["rp50"]))] for n, b in bs])

def hist_dump(img, n):
    b = dict(boots(img))[n]
    lo, w = b["hlo"], b["hw"]
    h = b["hseg"]["all"][0]
    # cut the display at the pooled p99.5 to keep it readable
    hi = 0; s = 0; tot = sum(h) + b["hseg"]["all"][1]
    for i, c in enumerate(h):
        s += c
        if s >= 0.995 * tot:
            hi = i; break
    print(f"#### {img}-{n}: histogram, 128-cycle buckets from {lo} (rounds as rows, pooled last; ‰ of the round)\n")
    head = ["round"] + [str(lo + i * w) for i in range(hi + 1)] + ["p50"]
    rows = []
    for sgi in list(range(20)) + ["all"]:
        cts = b["hseg"][sgi][0]
        t = sum(cts) + b["hseg"][sgi][1]
        rows.append([str(sgi + 1) if sgi != "all" else "all"] +
                    [str(round(1000 * cts[i] / t)) for i in range(hi + 1)] +
                    [str(b["rp50"][sgi]) if sgi != "all" else str(contended(b)["p50"])])
    table(head, rows)

# ---------------------------------------------------------------- item 2
def stats_rows(img):
    out = []
    for n, b in boots(img):
        if "cpus" not in b or "caller" not in b:
            continue
        cpc = b["cpc"]
        win = b["window"]
        cl = b["caller"]
        act = {c: d for c, d in b["cpus"].items() if d["acq"] > 0}
        busy = {c: d for c, d in act.items() if c != cl}
        hold = sum(d["hold"] for d in act.values())
        wait = sum(d["wait"] for d in act.values())
        acq = sum(d["acq"] for d in act.values())
        C = act[cl]
        def dshare(d):
            q = [x[0] for x in d["depth"]]
            t = sum(q)
            return [100.0 * x / t for x in q[:4]]
        cd = dshare(C)
        bd = [dshare(d) for d in busy.values()]
        bdm = [st.mean(x[k] for x in bd) for k in range(4)] if bd else [float("nan")] * 4
        # client's mean wait by depth (cycles)
        cwd = [cpc * w / q if q else float("nan") for q, w in C["depth"][:4]]
        r = b.get("rp50", [])
        row = dict(
            boot=n, p50=contended(b)["p50"], worst=contended(b)["worst"], rspread=spread(r) if r else float("nan"),
            cvcpu=cl, util=100.0 * hold / win,
            butil=100.0 * sum(d["hold"] for d in busy.values()) / win, wshare=100.0 * wait / (win * len(act)),
            gap=cpc * (win - hold) / acq,
            ch=cpc * C["hold"] / C["acq"], ch99=cpc * C["hold_p99"], cw=cpc * C["wait"] / C["acq"],
            cw99=cpc * C["wait_p99"],
            bh=cpc * sum(d["hold"] for d in busy.values()) / max(1, sum(d["acq"] for d in busy.values())),
            bh99=cpc * st.mean(d["hold_p99"] for d in busy.values()) if busy else float("nan"),
            bw=cpc * sum(d["wait"] for d in busy.values()) / max(1, sum(d["acq"] for d in busy.values())),
            bw99=cpc * st.mean(d["wait_p99"] for d in busy.values()) if busy else float("nan"),
            bacq=st.mean(d["acq"] for d in busy.values()) / C["acq"] if busy else 0.0,
            cd=cd, bd=bdm, cwd=cwd,
            rt=2 * cpc * (C["wait"] + C["hold"]) / C["acq"])
        out.append(row)
    return out

def item2(img):
    rows = stats_rows(img)
    if not rows:
        return
    rows.sort(key=lambda r: r["p50"])
    print(f"### {img}: per boot (lock figures in CPU cycles, window = the contended measurement)\n")
    table(["boot", "p50", "worst round", "round spread", "client vCPU", "lock utilisation (held / wall)", "held by busy CPUs / wall",
           "waiting share", "free gap per handover", "client hold mean / p99", "client wait mean / p99",
           "busy hold mean / p99", "busy wait mean / p99", "busy acq per client acq",
           "client depth 0/1/2/3 (%)", "busy depth 0/1/2/3 (%)", "client wait at depth 1/2/3",
           "2 x (client wait + hold)"],
          [[r["boot"], r["p50"], r["worst"], f"{r['rspread']:.1f} %", r["cvcpu"], f"{r['util']:.1f} %",
            f"{r['butil']:.1f} %", f"{r['wshare']:.1f} %", f"{r['gap']:.0f}", f"{r['ch']:.0f} / {r['ch99']:.0f}",
            f"{r['cw']:.0f} / {r['cw99']:.0f}", f"{r['bh']:.0f} / {r['bh99']:.0f}",
            f"{r['bw']:.0f} / {r['bw99']:.0f}", f"{r['bacq']:.2f}",
            "/".join(f"{x:.1f}" for x in r["cd"]), "/".join(f"{x:.1f}" for x in r["bd"]),
            "/".join(fmt(x) for x in r["cwd"][1:]), f"{r['rt']:.0f}"] for r in rows])
    if len(rows) >= 3:
        p = [r["p50"] for r in rows]
        keys = [("lock utilisation", "util"), ("held by busy CPUs / wall", "butil"), ("waiting share", "wshare"), ("free gap", "gap"),
                ("client hold mean", "ch"), ("client hold p99", "ch99"), ("client wait mean", "cw"),
                ("client wait p99", "cw99"), ("busy hold mean", "bh"), ("busy hold p99", "bh99"),
                ("busy wait mean", "bw"), ("busy wait p99", "bw99"), ("busy acq per client acq", "bacq"),
                ("2 x (client wait + hold)", "rt")]
        trs = [[k, f"{pearson(p, [r[v] for r in rows]):+.2f}",
                f"{min(r[v] for r in rows):.1f}-{max(r[v] for r in rows):.1f}"] for k, v in keys]
        trs.append(["client depth 3 share", f"{pearson(p, [r['cd'][3] for r in rows]):+.2f}",
                    f"{min(r['cd'][3] for r in rows):.2f}-{max(r['cd'][3] for r in rows):.2f}"])
        trs.append(["client vCPU", f"{pearson(p, [r['cvcpu'] for r in rows]):+.2f}",
                    f"{min(r['cvcpu'] for r in rows)}-{max(r['cvcpu'] for r in rows)}"])
        print(f"r with the contended p50 over {len(rows)} boots:\n")
        table(["figure", "r", "min-max"], trs)

# ---------------------------------------------------------------- depth build
def depth_rows(img):
    out = []
    for n, b in boots(img):
        if not b["depth"]:
            continue
        cl = b["dcaller"]
        r = b["rp50"]
        cseg = []
        for sg in range(20):
            nn, q = b["depth"][sg][cl]
            cseg.append([100.0 * x / nn if nn else float("nan") for x in q])
        tot = [sum(b["depth"][sg][cl][1][k] for sg in range(20)) for k in range(4)]
        T = sum(tot)
        busy = [c for c in b["depth"][0] if c != cl and sum(b["depth"][sg][c][0] for sg in range(20)) > 0]
        btot = [sum(b["depth"][sg][c][1][k] for sg in range(20) for c in busy) for k in range(4)]
        BT = sum(btot) or 1
        bacq = sum(b["depth"][sg][c][0] for sg in range(20) for c in busy) / max(1, len(busy)) / T
        # mean tickets ahead of the client, per round
        mahead = [sum(k * x for k, x in enumerate(s)) / 100.0 for s in cseg]
        out.append(dict(boot=n, p50=contended(b)["p50"], worst=contended(b)["worst"], rs=r, cl=cl, busy=sorted(busy),
                        cd=[100.0 * x / T for x in tot], bd=[100.0 * x / BT for x in btot], bacq=bacq,
                        mahead=mahead, r_rounds=pearson(r, mahead),
                        d3=[s[3] for s in cseg]))
    return out

def item_depth(img):
    rows = depth_rows(img)
    if not rows:
        return
    print(f"### {img}: tickets ahead at asking (no TSC), per boot\n")
    table(["boot", "p50", "worst round", "round spread", "client vCPU", "busy vCPUs", "client 0/1/2/3+ (%)",
           "busy 0/1/2/3+ (%)", "busy acq per client acq", "client mean ahead, rounds min-max",
           "r(round p50, round mean ahead)"],
          [[r["boot"], r["p50"], r["worst"], f"{spread(r['rs']):.1f} %", r["cl"], ",".join(map(str, r["busy"])),
            "/".join(f"{x:.2f}" for x in r["cd"]), "/".join(f"{x:.2f}" for x in r["bd"]),
            f"{r['bacq']:.3f}", f"{min(r['mahead']):.3f}-{max(r['mahead']):.3f}", f"{r['r_rounds']:+.2f}"]
           for r in sorted(rows, key=lambda r: r["p50"])])
    groups = {}
    for r in rows:
        groups.setdefault((r["cl"], ",".join(map(str, r["busy"]))), []).append(r["p50"])
    print("By placement (client vCPU; busy vCPUs):\n")
    table(["client vCPU", "busy vCPUs", "boots", "p50 each", "median"],
          [[k[0], k[1], len(v), ", ".join(map(str, sorted(v))), f"{st.median(v):.0f}"]
           for k, v in sorted(groups.items(), key=lambda kv: st.median(kv[1]))])
    p = [r["p50"] for r in rows]
    ma = [st.mean(r["mahead"]) for r in rows]
    print(f"Across boots: r(p50, client mean tickets ahead) = {pearson(p, ma):+.2f}; "
          f"pooled over all rounds of all boots: r = "
          f"{pearson([x for r in rows for x in r['rs']], [x for r in rows for x in r['mahead']]):+.2f}.\n")

# ---------------------------------------------------------------- items 3, 4
def sweep(groups):
    rows = []
    for label, img, simg, dimg in groups:
        bs, v, w = summary_line(img)
        if not v:
            continue
        rsp = [spread(b["rp50"]) for _, b in bs if "rp50" in b]
        sr = stats_rows(simg) if simg else []
        dr = depth_rows(dimg) if dimg else []
        p99 = [contended(b)["p99"] for _, b in bs]
        rows.append([label, len(v), f"{st.median(v):.0f}", f"{min(v)}-{max(v)}", f"{spread(v):.1f} %",
                     f"{100 * st.pstdev(v) / st.mean(v):.1f} %",
                     f"{st.median(w):.0f} / {max(w)}", f"{st.median(rsp):.1f} %" if rsp else "-",
                     f"{st.median(p99):.0f} / {spread(p99):.1f} %",
                     f"{st.median(r['util'] for r in sr):.1f} % / {st.median(r['butil'] for r in sr):.1f} %" if sr else "-",
                     "/".join(f"{st.median(r['cd'][k] for r in sr):.1f}" for k in range(4)) if sr else "-",
                     "/".join(f"{st.median(r['cd'][k] for r in dr):.1f}" for k in range(4)) if dr else "-",
                     f"{spread([r['p50'] for r in sr]):.1f} %" if sr else "-"])
    table(["variant", "boots", "contended p50 median", "min-max", "spread across boots", "CV",
           "worst round p50 median / max", "within-boot round spread (median)", "p99 median / spread",
           "lock utilisation: all / busy CPUs' holds (stats build)", "client tickets ahead 0/1/2/3 % (stats build)",
           "client tickets ahead 0/1/2/3 % (depth build, no TSC)", "stats build p50 spread"], rows)

if __name__ == "__main__":
    print("## Item 1\n")
    for img in ["base", "gate"]:
        if boots(img):
            item1(img) if img == "base" else None
    print("## Item 2\n")
    for img in ["stats"]:
        item2(img)
    print("## Depth builds\n")
    for img in ["depth", "depth-s100", "depth-s300", "depth-s600", "depth-s1000", "depth-s2000", "depth-s3000", "depth-b2", "depth-b1", "ts-depth", "ts-depth-b1"]:
        item_depth(img)
    print("## Items 3 and 4\n")
    sweep([("3 busy, N=0 (base)", "base", "stats", "depth")] +
          [(f"3 busy, N={n}", f"s{n}", f"stats-s{n}", f"depth-s{n}") for n in (100, 300, 600, 1000, 2000, 3000, 10000)] +
          [("2 busy, N=0", "b2", "stats-b2", "depth-b2"), ("1 busy, N=0", "b1", "stats-b1", "depth-b1")])
    for img in ["s300", "s1000", "s3000", "s10000", "b2", "b1", "stats-s300", "stats-s1000", "stats-s3000",
                "stats-s10000", "stats-b2", "stats-b1"]:
        if boots(img) and img.startswith("stats-"):
            item2(img)
