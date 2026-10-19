#!/usr/bin/env python3
"""analyze.py PREFIX N - the plan's stability test over N boots' logs
(PREFIX-1.log .. PREFIX-N.log): per measurement, the spread of the runs'
p50 and p99, (max - min) / median, over all runs and over each 10-run
window (1-10, 2-11, ... every one). Criterion: p50 < 5 %, p99 < 20 %.
The contended line (every other CPU making system calls) is reported but
left out of the verdict: a worst case, not held to the test (decision
0019)."""
import re
import statistics as st
import sys

prefix, n = sys.argv[1], int(sys.argv[2])
rows = {}
for i in range(1, n + 1):
    for line in open(f"{prefix}-{i}.log", errors="replace"):
        if line.startswith("ipcbench: ") and " p50=" in line:
            name = line.split(":")[1].strip()
            p50 = int(re.search(r" p50=(\d+)", line).group(1))
            p99 = int(re.search(r" p99=(\d+)", line).group(1))
            rows.setdefault(name, []).append((p50, p99))


def spread(v):
    return 100.0 * (max(v) - min(v)) / st.median(v)


worst_ok = True
for name, v in rows.items():
    a = [x[0] for x in v]
    b = [x[1] for x in v]
    wins = [(s, s + 10) for s in range(0, len(v) - 9)]
    w50 = max(spread(a[s:e]) for s, e in wins)
    w99 = max(spread(b[s:e]) for s, e in wins)
    held = "every other CPU" not in name
    ok = w50 < 5 and w99 < 20
    if held:
        worst_ok &= ok
    print(f"{name[:58]:58s} p50 {st.median(a):7g} ({min(a)}-{max(a)}) all {spread(a):4.1f}% "
          f"worst-10 {w50:4.1f}% | p99 all {spread(b):4.1f}% worst-10 {w99:4.1f}%  {('ok' if ok else 'FAIL') if held else 'worst case (0019)'}")
print("criterion met in every 10-run window" if worst_ok else "criterion NOT met in some 10-run window")
