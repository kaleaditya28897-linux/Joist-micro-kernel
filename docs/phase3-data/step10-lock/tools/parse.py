#!/usr/bin/env python3
"""parse.py LOG... : the lock study's figures from serial logs.

Each LOG is one boot. Prints, per (mode, k): total round trips/s (median,
min-max over boots), per pair, scaling against k=1, CPU cycles per round
trip (by the clients' ruler); and where lockstats lines are present, the
kernel lock's figures for that window. Also ipcbench lines."""
import re, statistics as st, sys
from collections import defaultdict

def med(v):
    return st.median(v) if v else float('nan')

scale = defaultdict(list)    # (mode,k) -> [(total_per_s, [cyc per rt per pair], cyc_per_count)]
locks = defaultdict(list)    # key -> [dict]
bench = defaultdict(list)    # name -> [p50]

def parse_lock(block):
    d = {'cpus': []}
    for l in block:
        m = re.match(r'lockstats: window (\d+) tsc counts, cpus (\d+)', l)
        if m:
            d['window'] = int(m[1]); d['ncpus'] = int(m[2]); continue
        m = re.match(r'lockstats: cpu (\d+) (.*)', l)
        if m:
            toks = m[2].split()
            c = {}
            for i in range(0, len(toks), 2):
                k, v = toks[i], toks[i + 1]
                if '/' in v:
                    a, b = v.split('/'); c[k] = (int(a), int(b))
                else:
                    c[k] = int(v)
            d['cpus'].append(c); continue
        m = re.match(r'lockstats: all acq (\d+) hold_p50 (\d+) hold_p99 (\d+) wait_p50 (\d+) wait_p99 (\d+)', l)
        if m:
            d['acq'], d['hold_p50'], d['hold_p99'], d['wait_p50'], d['wait_p99'] = map(int, m.groups())
    return d

for path in sys.argv[1:]:
    block = []
    pairs = defaultdict(list)
    for l in open(path, errors='replace'):
        l = l.strip()
        if l.startswith('lockstats:'):
            block.append(l); continue
        m = re.match(r'scale: (\S+) k=(\d+) pair=(\d+) n=(\d+) late=(\d+) ruler=(\d+)', l)
        if m:
            pairs[(m[1], int(m[2]))].append((int(m[4]), int(m[5]), int(m[6])))
            continue
        m = re.match(r'scale: (\S+) k=(\d+) total=(\d+) per_s=(\d+) window=(\d+)', l)
        if m:
            key = (m[1], int(m[2])); w = int(m[5])
            ps = pairs.pop(key)
            if any(p[1] for p in ps):
                print(f'# {path}: {key} a client started late', file=sys.stderr)
            cyc = [w * 12000 / (r * n) for n, _, r in ps if n and r]
            cpc = st.mean(12000 / r for _, _, r in ps if r)
            scale[key].append((int(m[4]), cyc, cpc, [p[0] for p in ps]))
            if block:
                d = parse_lock(block); d['cpc'] = cpc; d['rt'] = int(m[3])
                locks[('scale',) + key].append(d)
            block = []
            continue
        m = re.match(r'ipcbench: (.*): n=\S+ min=\d+ p50=(\d+) p99=(\d+).*cycles took (\d+) to (\d+) counts', l)
        if m:
            bench[m[1]].append((int(m[2]), int(m[3])))
            if block:
                d = parse_lock(block); d['cpc'] = 12000 / ((int(m[4]) + int(m[5])) / 2)
                locks[('ipcbench', m[1])].append(d)
            block = []

def rng(v, f='{:.0f}'):
    return f'{f.format(med(v))} ({f.format(min(v))}-{f.format(max(v))})'

print('## scaling (round trips/s, median (min-max) over boots)')
for mode in ('passive', 'own-context'):
    base = med([t for t, *_ in scale.get((mode, 1), [])]) if (mode, 1) in scale else None
    for k in range(1, 9):
        v = scale.get((mode, k))
        if not v:
            continue
        tot = [t for t, *_ in v]
        per = [t / k for t in tot]
        cyc = [med(c) for _, c, _, _ in v]
        ghz = [cpc for *_, cpc, _ in v]
        fair = [min(n) / max(n) for *_, n in v]
        sc = med(tot) / base if base else float('nan')
        print(f'{mode} k={k} runs={len(v)} total={rng(tot)} per_pair={rng(per)} scaling={sc:.2f} '
              f'cycles/rt={rng(cyc)} cpu_cycles_per_tsc={med(ghz):.3f} least/most pair={min(fair):.2f}')

print('## kernel lock (TSC counts converted to CPU cycles by the ruler)')
for key, v in locks.items():
    rows = []
    for d in v:
        W = d['window'] * d['ncpus']
        wait = sum(c['wait'] for c in d['cpus']); hold = sum(c['hold'] for c in d['cpus'])
        acq = d['acq'] or 1; cpc = d['cpc']
        kinds = {k: (sum(c[k][0] for c in d['cpus']), sum(c[k][1] for c in d['cpus'])) for k in ('fast', 'slow', 'idle')}
        scans = {k: (sum(c[k][0] for c in d['cpus']), sum(c[k][1] for c in d['cpus']))
                 for k in ('pick_on', 'next_event', 'poke_others', 'tick', 'abi_syscall', 'timer_write', 'space_switch') if k in d['cpus'][0]}
        rows.append(dict(wait_share=100 * wait / W, hold_share=100 * hold / W,
                         acq=acq, mean_hold=hold / acq * cpc, p50_hold=d['hold_p50'] * cpc,
                         p99_hold=d['hold_p99'] * cpc, mean_wait=wait / acq * cpc,
                         p99_wait=d['wait_p99'] * cpc,
                         **{f'{k}_n': n for k, (n, _) in kinds.items()},
                         **{f'{k}_hold': (t / n * cpc if n else 0) for k, (n, t) in kinds.items()},
                         **{f'{k}_n': n for k, (n, _) in scans.items()},
                         **{f'{k}_mean': (t / n * cpc if n else 0) for k, (n, t) in scans.items()},
                         **{f'{k}_per_slow': (t / kinds['slow'][0] * cpc if kinds['slow'][0] else 0)
                            for k, (n, t) in scans.items()},
                         slow_scans_per_slow=sum(t for k, (n, t) in scans.items() if k in ('pick_on', 'next_event', 'poke_others', 'tick'))
                         / max(1, kinds['slow'][0] + kinds['idle'][0]) * cpc))
    print(f'{key} runs={len(rows)}')
    for f in rows[0]:
        vals = [r[f] for r in rows]
        print(f'    {f} = {rng(vals, "{:.1f}" if "share" in f else "{:.0f}")}')

print('## ipcbench p50 (p99) cycles, median (min-max)')
for k, v in bench.items():
    print(f'{k}: p50 {rng([a for a, _ in v])} p99 {rng([b for _, b in v])} runs={len(v)}')
