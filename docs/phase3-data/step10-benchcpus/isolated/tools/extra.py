# extra.py BENCHFIX_DIR : run2 additions (markdown on stdout): all-line stats for run1 vs run2
# (all 20 and each 10-run half), per-run own-context p50/p99, interrupts on host CPUs 2-5 per boot.
import re, sys, glob, statistics as st, collections
h = sys.argv[1]
pat = re.compile(r'ipcbench: (.*?): n=\S+ min=(\d+) p50=(\d+) p99=(\d+) .*?took (\d+) to (\d+) counts')
def load(d):
    runs = {}
    for f in glob.glob(d + '/fast-*.log'):
        k = int(re.search(r'-(\d+)\.log$', f).group(1))
        runs[k] = [(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
                   for m in map(pat.search, open(f)) if m]
    return dict(sorted(runs.items()))
r1, r2 = load(h + '/run1/raw'), load(h + '/raw')
names = [r[0] for r in next(iter(r2.values()))]
short = ['null call', '2 AS passive', '1 AS passive', '2 AS own ctx', '2 AS 4 KiB', 'contended']
def sp(v): return 100 * (max(v) - min(v)) / st.median(v)
def stats(runs, i, lo, hi):
    rows = [r[i] for k, r in runs.items() if lo <= k <= hi]
    p50 = [x[1] for x in rows]; p99 = [x[2] for x in rows]
    return f'{st.median(p50):g} / {min(p50)}-{max(p50)} / {sp(p50):.1f} % | {st.median(p99):g} / {min(p99)}-{max(p99)} / {sp(p99):.1f} %'
print('\n### Every line, run1 vs run2: p50 median / min-max / spread | p99 median / min-max / spread\n')
print('| line | runs | run1 p50 | run1 p99 | run2 p50 | run2 p99 |')
print('|---|---|---|---|---|---|')
for i in range(len(names)):
    for lo, hi in ((1, 20), (1, 10), (11, 20)):
        print(f'| {short[i]} | {lo}-{hi} | {stats(r1, i, lo, hi)} | {stats(r2, i, lo, hi)} |')
print('\n### Per run, run2: null, contended, own context (p50 / p99), own-context ruler A-B\n')
print('| run | null p50 | null p99 | contended p50 | contended p99 | own ctx p50 | own ctx p99 | own ctx ruler A-B | IRQs on CPUs 2-5 |')
print('|---|---|---|---|---|---|---|---|---|')
def irq(f):
    d = {}
    for l in open(f).readlines()[1:]:
        p = l.split()
        if not p: continue
        k = p[0].rstrip(':')
        v = [int(x) for x in p[1:13] if x.isdigit()]
        if len(v) < 6: continue
        src = k if not k.isdigit() else k + ' ' + p[-1]
        if not k.isdigit(): src = k + ' ' + ' '.join(x for x in p[1:] if not x.isdigit())
        d[src] = v[2:6]
    return d
tot = collections.Counter(); percpu = collections.Counter(); perrun = {}
for k in r2:
    a, b = irq(f'{h}/raw/fast-{k}.irq-before'), irq(f'{h}/raw/fast-{k}.irq-after')
    c = collections.Counter()
    for s in b:
        dv = [y - x for x, y in zip(a.get(s, [0] * 4), b[s])]
        if sum(dv): c[s] = sum(dv)
        for j, x in enumerate(dv): percpu[(s, j + 2)] += x
    perrun[k] = c; tot += c
for k, r in r2.items():
    c = perrun[k]
    top = ', '.join(f'{s.split()[0]} {n}' for s, n in c.most_common(4))
    print(f'| {k} | {r[0][1]} | {r[0][2]} | {r[5][1]} | {r[5][2]} | {r[3][1]} | {r[3][2]} | {r[3][3]}-{r[3][4]} | {sum(c.values())} ({top}) |')
print('\n### Interrupts on host CPUs 2-5 during the boots, all 20 runs, by source\n')
print('| source | total | per run min-median-max | CPU2 | CPU3 | CPU4 | CPU5 |')
print('|---|---|---|---|---|---|---|')
for s, n in tot.most_common():
    v = [perrun[k][s] for k in r2]
    print(f'| {s} | {n} | {min(v)}-{st.median(v):g}-{max(v)} | ' + ' | '.join(str(percpu[(s, j)]) for j in range(2, 6)) + ' |')
