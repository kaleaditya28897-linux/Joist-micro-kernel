#!/usr/bin/env python3
"""roundprofile.py RAWDIR IMG... : per image, the mean over boots of each round's
p50 relative to its boot's median (in %), round by round: a time-locked
effect shows as the same rounds high in every boot."""
import os, re, sys, statistics as st
raw = sys.argv[1]
print("| image | boots | " + " | ".join(str(i + 1) for i in range(20)) + " | boots with round 6, 7 or 8 its highest |")
print("|" + "---|" * 23)
for img in sys.argv[2:]:
    prof = []; hi = 0; nb = 0
    for f in sorted(os.listdir(raw)):
        if not re.match(re.escape(img) + r"-\d+\.log$", f):
            continue
        for l in open(os.path.join(raw, f), errors="replace"):
            if l.startswith("ipcbench-rounds: p50"):
                r = [int(x) for x in l.split()[2:]]
                m = st.median(r)
                prof.append([100.0 * (x - m) / m for x in r])
                nb += 1
                hi += r.index(max(r)) in (5, 6, 7)
    if prof:
        print(f"| {img} | {nb} | " + " | ".join(f"{st.mean(p[i] for p in prof):+.1f}" for i in range(20)) + f" | {hi} |")
