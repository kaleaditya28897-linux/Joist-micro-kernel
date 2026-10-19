#!/usr/bin/env python3
"""series.py RAWDIR TSC_HZ IMG... : the contended line's per-batch medians (500
calls each) against time since the measurement began, averaged over each
image's boots in 4 ms bins, relative to each boot's median (in %); with the
ruler (CPU cycles per TSC count) likewise; and the lead from busy start."""
import os, re, sys, statistics as st
raw, hz = sys.argv[1], float(sys.argv[2])
BIN = 4.0  # ms
def load(img):
    out = []
    for f in sorted(os.listdir(raw), key=lambda x: (len(x), x)):
        if not re.match(re.escape(img) + r"-\d+\.log$", f):
            continue
        b = []; notes = {}
        for l in open(os.path.join(raw, f), errors="replace"):
            if l.startswith("ipcbench-batch: "):
                k, at, ru, p = map(int, l.split()[1:5]); b.append((at / hz * 1e3, ru, p))
            elif l.startswith("benchroot: tsc "):
                w, v = l.split()[2:4]; notes[w] = int(v)
            elif l.startswith("ipcbench-series: start tsc"):
                notes["start"] = int(l.split()[-1])
        if b:
            out.append((f, b, notes))
    return out
imgs = sys.argv[3:]
data = {i: load(i) for i in imgs}
nb = int(max(b[-1][0] for i in imgs for _, b, _ in data[i]) // BIN) + 1
print("Batch median relative to the boot's median (%), mean over boots, by time since the measurement began (ms):\n")
print("| image | boots | lead (busy start to measurement), ms | " + " | ".join(f"{int(k * BIN)}" for k in range(nb)) + " |")
print("|" + "---|" * (nb + 3))
for i in imgs:
    rows = []
    leads = []
    for f, b, n in data[i]:
        m = st.median(p for _, _, p in b)
        bins = [[] for _ in range(nb)]
        for t, ru, p in b:
            bins[int(t // BIN)].append(100.0 * (p - m) / m)
        rows.append([st.mean(x) if x else None for x in bins])
        if "busy-start" in n and "start" in n:
            leads.append((n["start"] - n["busy-start"]) / hz * 1e3)
    cells = []
    for k in range(nb):
        v = [r[k] for r in rows if r[k] is not None]
        cells.append(f"{st.mean(v):+.1f}" if v else "")
    print(f"| {i} | {len(rows)} | {st.median(leads):.0f} | " + " | ".join(cells) + " |")
print("\nRuler (TSC counts for 12 000 CPU cycles), mean over boots, same bins:\n")
print("| image | " + " | ".join(f"{int(k * BIN)}" for k in range(nb)) + " |")
print("|" + "---|" * (nb + 1))
for i in imgs:
    cells = []
    for k in range(nb):
        v = [ru for f, b, n in data[i] for t, ru, p in b if int(t // BIN) == k]
        cells.append(f"{st.mean(v):.0f}" if v else "")
    print(f"| {i} | " + " | ".join(cells) + " |")
