#!/usr/bin/env python3
# bytable.py RAWDIR : contended p50 by client vCPU, per image and host order
# (single-measurement images only), and placement's share of the variance.
import json, sys, statistics as st, math
from collections import defaultdict, Counter
B = json.load(open(sys.argv[1] + "/boots.json"))
def spread(v): return 100 * (max(v) - min(v)) / st.median(v)
def w10(v): return max(spread(v[i:i + 10]) for i in range(len(v) - 9)) if len(v) >= 10 else None
print("| image@order | client vCPU | host CPU | boots | p50 median | min-max | spread | worst 10-window | CV |")
print("|---|---|---|---|---|---|---|---|---|")
share = {}
for var in sorted(set(b["img"] + "@" + b["order"] for b in B if "client" in b and len(b.get("clients", [])) == 1)):
    xs = [b for b in B if b["img"] + "@" + b["order"] == var]
    g = defaultdict(list)
    for b in xs: g[b["client"]].append(b["cont"]["p50"])
    for c in sorted(g):
        v = g[c]; w = w10(v)
        print(f"| {var} | {c} | {var[-4:][c]} | {len(v)} | {st.median(v):.0f} | {min(v)}-{max(v)} | {spread(v):.1f} % | {'-' if w is None else f'{w:.1f} %'} | {100 * st.stdev(v) / st.mean(v) if len(v) > 1 else 0:.1f} % |")
    v = [b["cont"]["p50"] for b in xs]; m = st.mean(v)
    tot = sum((x - m) ** 2 for x in v); within = sum((b["cont"]["p50"] - st.mean(g[b["client"]])) ** 2 for b in xs)
    share[var] = (100 * (1 - within / tot), 100 * math.sqrt(within / (len(v) - len(g))) / m, 100 * st.stdev(v) / m)
print("\n| image@order | variance share of client vCPU | SD within one placement | SD all boots |")
print("|---|---|---|---|")
for k, (a, b_, c) in share.items(): print(f"| {k} | {a:.0f} % | {b_:.2f} % | {c:.2f} % |")
