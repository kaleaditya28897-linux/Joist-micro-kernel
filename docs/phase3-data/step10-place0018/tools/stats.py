#!/usr/bin/env python3
# stats.py RAWDIR : summaries from raw/boots.json (analyze.py): per variant,
# contended p50 median, min-max, spread ((max-min)/median), worst 10-boot
# window, CV; by client vCPU and client host CPU; correlations with host
# figures; per-boot round SDs.
import json, sys, statistics as st, itertools, math
from collections import defaultdict
B = json.load(open(sys.argv[1] + "/boots.json"))
def spread(v): return 100 * (max(v) - min(v)) / st.median(v)
def w10(v): return max(spread(v[i:i + 10]) for i in range(len(v) - 9)) if len(v) >= 10 else float("nan")
def cv(v): return 100 * st.stdev(v) / st.mean(v) if len(v) > 1 else float("nan")
def r(x, y):
    mx, my = st.mean(x), st.mean(y)
    sx = math.sqrt(sum((a - mx) ** 2 for a in x)); sy = math.sqrt(sum((b - my) ** 2 for b in y))
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx and sy else float("nan")
var = defaultdict(list)
for b in B: var[b["img"] + "@" + b["order"]].append(b)
print("## per variant (boot order)\n")
print("| variant | boots | p50 median | min-max | spread | worst 10-window | CV | p99 median / spread | worst-round median |")
print("|---|---|---|---|---|---|---|---|---|")
for k, xs in sorted(var.items()):
    v = [b["cont"]["p50"] for b in xs]; p = [b["cont"]["p99"] for b in xs]; w = [b["cont"]["worst"] for b in xs]
    print(f"| {k} | {len(v)} | {st.median(v):.0f} | {min(v)}-{max(v)} | {spread(v):.1f} % | {w10(v):.1f} % | {cv(v):.1f} % | {st.median(p):.0f} / {spread(p):.1f} % | {st.median(w):.0f} |")
print("\n## per variant, p50 in boot order\n")
for k, xs in sorted(var.items()):
    print(f"- {k}: " + " ".join(str(b["cont"]["p50"]) for b in xs))
D = [b for b in B if "client" in b]
print("\n## depth: by client vCPU, all orders\n")
print("| order | client vCPU | client host CPU | boots | p50 median | min-max | spread | CV |")
print("|---|---|---|---|---|---|---|---|")
g = defaultdict(list)
for b in D: g[(b["order"], b["client"], b["client_host"])].append(b["cont"]["p50"])
for k in sorted(g):
    v = g[k]
    print(f"| {k[0]} | {k[1]} | {k[2]} | {len(v)} | {st.median(v):.0f} | {min(v)}-{max(v)} | {spread(v):.1f} % | {cv(v) if len(v) > 1 else 0:.1f} % |")
for key, name in (("client", "client vCPU"), ("client_host", "client host CPU")):
    g = defaultdict(list)
    for b in D: g[b[key]].append(b["cont"]["p50"] / st.median([x["cont"]["p50"] for x in D if x["order"] == b["order"]]))
    print(f"\nby {name}, all orders pooled (p50 / its order's median): " + "; ".join(f"{k}: n={len(v)} {100*(st.mean(v)-1):+.2f} %" for k, v in sorted(g.items())))
# share of variance explained by placement within order 2345 and pooled
for sel, name in ((lambda b: b["order"] == "2345", "order 2345"), (lambda b: True, "all orders, normalised per order")):
    xs = [b for b in D if sel(b)]
    if len(xs) < 4: continue
    med = {o: st.median([b["cont"]["p50"] for b in D if b["order"] == o]) for o in set(b["order"] for b in xs)}
    v = [b["cont"]["p50"] / med[b["order"]] for b in xs]
    for key in ("client", "client_host", "root_taker"):
        g = defaultdict(list)
        for b, x in zip(xs, v): g[b[key]].append(x)
        m = st.mean(v); tot = sum((x - m) ** 2 for x in v)
        within = sum((x - st.mean(g[b[key]])) ** 2 for b, x in zip(xs, v))
        print(f"{name}: variance share explained by {key}: {100 * (1 - within / tot):.0f} %; residual SD {100 * math.sqrt(within / (len(v) - len(g))):.2f} %")
print("\n## correlations with the contended p50 (within each variant)\n")
print("| variant | r(ruler lo) | r(ruler hi) | r(host freq at end) | r(pkg temp mean) | r(pkg temp at start) | r(host IRQs 2-5) | r(boot index) | r(2 AS passive p50) | r(own ctx p50) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for k, xs in sorted(var.items()):
    y = [b["cont"]["p50"] for b in xs]
    cols = []
    for f in (lambda b: b["cont"]["rlo"], lambda b: b["cont"]["rhi"], lambda b: b.get("freq"), lambda b: b.get("tpkg"),
              lambda b: b.get("t0"), lambda b: b.get("irq25"), lambda b: b["n"], lambda b: b["2as"]["p50"], lambda b: b["own"]["p50"]):
        x = [f(b) for b in xs]
        cols.append(f"{r(x, y):+.2f}" if None not in x else "-")
    print(f"| {k} | " + " | ".join(cols) + " |")
print("\n## round p50s within boots (images with BENCH_ROUNDS)\n")
for k, xs in sorted(var.items()):
    xs = [b for b in xs if "rounds" in b]
    if not xs: continue
    sds = [100 * st.stdev(b["rounds"]) / st.mean(b["rounds"]) for b in xs]
    means = [st.mean(b["rounds"]) for b in xs]
    print(f"- {k}: within-boot round SD median {st.median(sds):.1f} % (range {min(sds):.1f}-{max(sds):.1f}); SD of boot means {cv(means):.1f} %")
