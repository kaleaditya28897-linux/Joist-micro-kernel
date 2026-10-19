# analyze.py BENCHFIX_DIR : tables for findings.md (markdown on stdout)
import re, sys, glob, os, statistics as st
h = sys.argv[1]
pat = re.compile(r'ipcbench: (.*?): n=\S+ min=(\d+) p50=(\d+) p99=(\d+) .*?took (\d+) to (\d+) counts')
def load(files):
    runs = {}
    for f in files:
        k = int(re.search(r'-(\d+)\.(txt|log)$', f).group(1))
        runs[k] = [(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
                   for m in map(pat.search, open(f)) if m]
    return dict(sorted(runs.items()))
ps = load(glob.glob(h + '/powersave-runs/fast-*.txt'))
pf = load(glob.glob(h + '/raw/fast-*.log'))
names = [r[0] for r in next(iter(ps.values()))]
short = {n: s for n, s in zip(names, ['null call', '2 AS passive', '1 AS passive', '2 AS own ctx', '2 AS 4 KiB', 'contended'])}
def summ(runs, label):
    print(f'\n### {label} ({len(runs)} runs)\n')
    print('| measurement | p50 median | p50 min-max | p50 spread | p99 median | p99 min-max | p99 spread | ruler A min-max | ruler B min-max |')
    print('|---|---|---|---|---|---|---|---|---|')
    for i, n in enumerate(names):
        rows = [r[i] for r in runs.values() if len(r) > i]
        p50 = [r[1] for r in rows]; p99 = [r[2] for r in rows]
        A = [r[3] for r in rows]; B = [r[4] for r in rows]
        m50, m99 = st.median(p50), st.median(p99)
        print(f'| {n} | {m50:g} | {min(p50)}-{max(p50)} | {100*(max(p50)-min(p50))/m50:.1f} % | {m99:g} | {min(p99)}-{max(p99)} | {100*(max(p99)-min(p99))/m99:.1f} % | {min(A)}-{max(A)} | {min(B)}-{max(B)} |')
def perrun(runs, label):
    print(f'\n### Per run, {label}\n')
    print('| run | null p50 | null p99 | null ruler A-B | contended p50 | contended p99 |')
    print('|---|---|---|---|---|---|')
    for k, r in runs.items():
        print(f'| {k} | {r[0][1]} | {r[0][2]} | {r[0][3]}-{r[0][4]} | {r[5][1]} | {r[5][2]} |')
summ(pf, 'performance governor'); summ(ps, 'powersave governor (given)')
# 10-run windows on performance runs (the plan's criterion is over 10 runs)
print('\n### Performance runs split into two 10-run halves\n')
for lo, hi in ((1, 10), (11, 20)):
    sub = {k: v for k, v in pf.items() if lo <= k <= hi}
    if len(sub) < 10: continue
    for i in (0, 5):
        p50 = [r[i][1] for r in sub.values()]; p99 = [r[i][2] for r in sub.values()]
        print(f'- runs {lo}-{hi}, {short[names[i]]}: p50 spread {100*(max(p50)-min(p50))/st.median(p50):.1f} %, p99 spread {100*(max(p99)-min(p99))/st.median(p99):.1f} %')
perrun(pf, 'performance'); perrun(ps, 'powersave')
print('\n### Slow first measurement (null call ruler A > 8000 counts)\n')
for lab, runs in (('performance', pf), ('powersave', ps)):
    slow = [k for k, r in runs.items() if r[0][3] > 8000]
    print(f'- {lab}: {slow if slow else "none"}')
print('\n### Frequency samples (kHz, only while the boot\'s QEMU ran)\n')
print('| run | samples | CPU2 min/med/max | CPU3 min/med/max | CPU4 min/med/max | CPU5 min/med/max |')
print('|---|---|---|---|---|---|')
allc = [[] for _ in range(4)]
for f in sorted(glob.glob(h + '/raw/fast-*.freq'), key=lambda f: int(re.search(r'-(\d+)\.freq', f).group(1))):
    k = int(re.search(r'-(\d+)\.freq', f).group(1))
    s = [l.split() for l in open(f)]
    s = [[int(x) for x in l[2:6]] for l in s if len(l) >= 6 and l[1] == '1']
    if not s: print(f'| {k} | 0 | | | | |'); continue
    cols = list(zip(*s))
    for c in range(4): allc[c] += cols[c]
    print(f'| {k} | {len(s)} | ' + ' | '.join(f'{min(c)}/{int(st.median(c))}/{max(c)}' for c in cols) + ' |')
print('| all | | ' + ' | '.join(f'{min(c)}/{int(st.median(c))}/{max(c)}' for c in allc) + ' |')
