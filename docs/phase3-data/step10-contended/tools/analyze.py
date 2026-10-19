# analyze.py CONTEND_DIR : tables for findings.md (markdown on stdout).
# Per boot of lb-cur-stats: the contended window's lock figures, client and busy
# CPUs apart, in CPU cycles (TSC counts x 12000 / mean of the line's ruler min
# and max, as the lock study's parse.py did for ipcbench); hold's parts; lock
# order (predecessor) and queue depth; segments. Per boot of both images: the
# contended p50/p99, ruler, IRQs on host CPUs 2-5, host frequency. Pearson r of
# the contended p50 against each figure across boots.
import re, sys, glob, statistics as st, collections, math
h = sys.argv[1]
pat = re.compile(r'ipcbench: (.*?): n=\S+ min=(\d+) p50=(\d+) p99=(\d+) worst-p50=(\d+) .*?took (\d+) to (\d+) counts')
short = ['null call', '2 AS passive', '1 AS passive', '2 AS own ctx', '2 AS 4 KiB', 'contended']

def boots(img):
    fs = glob.glob(f'{h}/raw/{img}-*.log')
    fs = [f for f in fs if re.search(rf'/{img}-(\d+)\.log$', f)]
    return sorted(fs, key=lambda f: int(re.search(r'-(\d+)\.log$', f).group(1)))

def irq(f):
    d = {}
    for l in open(f).readlines()[1:]:
        p = l.split()
        if not p: continue
        k = p[0].rstrip(':')
        v = [int(x) for x in p[1:13] if x.isdigit()]
        if len(v) < 6: continue
        src = k + ' ' + (p[-1] if k.isdigit() else ' '.join(x for x in p[1:] if not x.isdigit()))
        d[src] = v[2:6]
    return d

def irqdiff(base):
    a, b = irq(base + '.irq-before'), irq(base + '.irq-after')
    c = collections.Counter(); per = [0] * 4
    for s in b:
        dv = [y - x for x, y in zip(a.get(s, [0] * 4), b[s])]
        if sum(dv): c[s] = sum(dv)
        for j, x in enumerate(dv): per[j] += x
    return c, per

def freq(base):
    s = [l.split() for l in open(base + '.freq')]
    s = [[int(x) for x in l[2:6]] for l in s if len(l) >= 6 and l[1] == '1']
    return [st.median(c) / 1000 for c in zip(*s)] if s else [float('nan')] * 4

def lock(lines):
    d = {'cpus': []}
    for l in lines:
        m = re.match(r'lockstats: window (\d+) tsc counts, cpus (\d+)', l)
        if m: d['window'] = int(m[1]); d['ncpus'] = int(m[2]); continue
        m = re.match(r'lockstats: cpu (\d+) (.*)', l)
        if m:
            toks = m[2].split(); c = {}
            for i in range(0, len(toks), 2):
                k, v = toks[i], toks[i + 1]
                c[k] = tuple(int(x) for x in v.split('/')) if '/' in v else int(v)
            d['cpus'].append(c)
    return d

def load(f):
    r = {'f': f, 'lines': [], 'lock': None, 'seg': None, 'pins': {}}
    ls = []
    for l in open(f, errors='replace'):
        l = l.strip()
        m = re.match(r'pin: CPU (\d+)/KVM: (\d+)', l)
        if m: r['pins'][int(m[1])] = int(m[2])
        if l.startswith('lockstats:'): ls.append(l)
        if l.startswith('ipcbench: segments'):
            r['seg'] = [tuple(map(int, x.split('/'))) for x in l.split(':', 2)[2].split()]
        m = pat.search(l)
        if m: r['lines'].append((m[1], int(m[3]), int(m[4]), int(m[5]), int(m[6]), int(m[7])))
    if ls: r['lock'] = lock(ls)
    base = f[:-4]
    r['irq'], r['irqcpu'] = irqdiff(base)
    r['freq'] = freq(base)
    c = r['lines'][5]
    r['p50'], r['p99'], r['worst'], r['rA'], r['rB'] = c[1], c[2], c[3], c[4], c[5]
    r['cpc'] = 12000 / ((c[4] + c[5]) / 2)
    return r

def figures(r):
    d = r['lock']; cpc = r['cpc']; W = d['window']
    cs = d['cpus']
    cli = max(range(len(cs)), key=lambda i: cs[i]['fast'][0])
    busy = [i for i in range(len(cs)) if i != cli]
    def agg(ids):
        n = sum(cs[i]['acq'] for i in ids)
        g = {'n': n,
             'hold': sum(cs[i]['hold'] for i in ids) / n * cpc,
             'wait': sum(cs[i]['wait'] for i in ids) / n * cpc,
             'hold_p99': st.mean(cs[i]['hold_p99'] for i in ids) * cpc,
             'wait_p99': st.mean(cs[i]['wait_p99'] for i in ids) * cpc,
             'hold_p50': st.mean(cs[i]['hold_p50'] for i in ids) * cpc,
             'wait_p50': st.mean(cs[i]['wait_p50'] for i in ids) * cpc}
        parts = 0
        for k, nm in (('abi_syscall', 'sys'), ('pick_on', 'pick'), ('next_event', 'next'),
                      ('poke_others', 'poke'), ('space_switch', 'cr3'), ('tick', 'tick'), ('timer_write', 'timer')):
            t = sum(cs[i][k][1] for i in ids) / n * cpc
            g[nm] = t
            if nm != 'timer': parts += t   # the timer is written after the unlock (RUN_LOCK_TIMER_OUT)
        g['rest'] = g['hold'] - parts
        return g
    f = {'cli': cli, 'vcpu_host': r['pins'].get(cli), 'c': agg([cli]), 'b': agg(busy)}
    tot_hold = sum(c['hold'] for c in cs); tot_wait = sum(c['wait'] for c in cs); n = sum(c['acq'] for c in cs)
    f['wait_share'] = 100 * tot_wait / (W * len(cs))
    f['busy_share'] = 100 * tot_hold / W
    f['gap'] = (W - tot_hold) / n * cpc        # lock free between a release and the next holder's first instruction
    f['acq_rate'] = n / W                      # acquisitions per TSC count
    f['client_rt_kernel'] = 2 * (f['c']['wait'] + f['c']['hold'])  # two entries per round trip
    f['pred_cli'] = [cs[cli]['pred'][i] for i in range(len(cs))]
    f['pred'] = [[c['pred'][i] for i in range(len(cs))] for c in cs]
    f['depth_cli'] = cs[cli]['depth'][:5]
    f['depth_busy'] = [sum(cs[i]['depth'][k] for i in busy) for k in range(5)]
    f['busy_hold_each'] = [cs[i]['hold'] / cs[i]['acq'] * cpc for i in busy]
    f['busy_next_each'] = [cs[i]['next_event'][1] / cs[i]['acq'] * cpc for i in busy]
    return f

def pr(x, y):
    if len(x) < 3: return float('nan')
    mx, my = st.mean(x), st.mean(y)
    sx = math.sqrt(sum((a - mx) ** 2 for a in x)); sy = math.sqrt(sum((b - my) ** 2 for b in y))
    return sum((a - mx) * (b - my) for a, b in zip(x, y)) / (sx * sy) if sx and sy else float('nan')

def sp(v): return 100 * (max(v) - min(v)) / st.median(v)

cur = [load(f) for f in boots('lb-cur')]
sts = [load(f) for f in boots('lb-cur-stats')]
for r in sts: r['F'] = figures(r)
num = lambda r: int(re.search(r'-(\d+)\.log$', r['f']).group(1))

print('### Every line, p50 median / min-max / spread | p99 median / min-max / spread\n')
print('| line | lb-cur p50 | lb-cur p99 | lb-cur-stats p50 | lb-cur-stats p99 |')
print('|---|---|---|---|---|')
for i in range(6):
    cell = []
    for runs in (cur, sts):
        p50 = [r['lines'][i][1] for r in runs]; p99 = [r['lines'][i][2] for r in runs]
        cell.append(f'{st.median(p50):g} / {min(p50)}-{max(p50)} / {sp(p50):.1f} %')
        cell.append(f'{st.median(p99):g} / {min(p99)}-{max(p99)} / {sp(p99):.1f} %')
    print(f'| {short[i]} | ' + ' | '.join(cell) + ' |')

print('\n### lb-cur, per boot (sorted by contended p50)\n')
print('| boot | contended p50 | p99 | worst seg p50 | ruler counts min-max | host MHz CPU2/3/4/5 (median) | IRQs on host CPUs 2/3/4/5 | top sources |')
print('|---|---|---|---|---|---|---|---|')
for r in sorted(cur, key=lambda r: r['p50']):
    top = ', '.join(f'{s.split()[0]} {n}' for s, n in r['irq'].most_common(3))
    print(f"| {num(r)} | {r['p50']} | {r['p99']} | {r['worst']} | {r['rA']}-{r['rB']} | {'/'.join(f'{x:.0f}' for x in r['freq'])} | {'/'.join(map(str, r['irqcpu']))} | {top} |")

print('\n### lb-cur-stats, per boot, the contended window (CPU cycles; sorted by contended p50)\n')
print('| boot | p50 | p99 | client vCPU (host CPU) | waiting share | lock held share | free gap per handover | client hold mean / p99 | client wait mean / p99 | busy hold mean / p99 | busy wait mean / p99 | 2 x (client wait + hold) |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
for r in sorted(sts, key=lambda r: r['p50']):
    F = r['F']; c, b = F['c'], F['b']
    print(f"| {num(r)} | {r['p50']} | {r['p99']} | {F['cli']} ({F['vcpu_host']}) | {F['wait_share']:.1f} % | {F['busy_share']:.1f} % | {F['gap']:.0f} | "
          f"{c['hold']:.0f} / {c['hold_p99']:.0f} | {c['wait']:.0f} / {c['wait_p99']:.0f} | {b['hold']:.0f} / {b['hold_p99']:.0f} | {b['wait']:.0f} / {b['wait_p99']:.0f} | {F['client_rt_kernel']:.0f} |")

print('\n### lb-cur-stats, the hold\'s parts per acquisition (CPU cycles; sorted by contended p50)\n')
print('| boot | p50 | who | hold | abi_syscall | pick | next_event | poke | CR3 | tick | the rest | (timer write, after unlock) |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|')
for r in sorted(sts, key=lambda r: r['p50']):
    for who in ('c', 'b'):
        g = r['F'][who]
        print(f"| {num(r)} | {r['p50']} | {'client' if who == 'c' else 'busy (3)'} | {g['hold']:.0f} | {g['sys']:.0f} | {g['pick']:.0f} | {g['next']:.0f} | {g['poke']:.0f} | {g['cr3']:.0f} | {g['tick']:.1f} | {g['rest']:.0f} | {g['timer']:.1f} |")

print('\n### lb-cur-stats, lock order (sorted by contended p50)\n')
print('Predecessor: of the client\'s acquisitions, the share whose previous holder was each CPU; busy CPUs\' hold each; queue depth (tickets ahead at asking).\n')
print('| boot | p50 | client CPU | client\'s predecessor CPU 0/1/2/3 | busy CPUs\' mean hold each | busy next_event each | client depth 0/1/2/3 | busy depth 0/1/2/3 |')
print('|---|---|---|---|---|---|---|---|')
for r in sorted(sts, key=lambda r: r['p50']):
    F = r['F']; n = sum(F['pred_cli'])
    print(f"| {num(r)} | {r['p50']} | {F['cli']} | {'/'.join(f'{100*x/n:.0f}' for x in F['pred_cli'])} % | {'/'.join(f'{x:.0f}' for x in F['busy_hold_each'])} | {'/'.join(f'{x:.0f}' for x in F['busy_next_each'])} | {'/'.join(map(str, F['depth_cli'][:4]))} | {'/'.join(map(str, F['depth_busy'][:4]))} |")

print('\n### lb-cur-stats, per boot: ruler, host, interrupts, segments\n')
print('| boot | p50 | ruler counts min-max | host MHz CPU2/3/4/5 | IRQs on host CPUs 2/3/4/5 | segment p50s min-median-max | segment spread |')
print('|---|---|---|---|---|---|---|')
for r in sorted(sts, key=lambda r: r['p50']):
    s = [a for a, _ in r['seg']]
    print(f"| {num(r)} | {r['p50']} | {r['rA']}-{r['rB']} | {'/'.join(f'{x:.0f}' for x in r['freq'])} | {'/'.join(map(str, r['irqcpu']))} | {min(s)}-{st.median(s):g}-{max(s)} | {sp(s):.1f} % |")

# correlations
def corr_table(runs, items, label):
    y = [r['p50'] for r in runs]
    print(f'\n### Pearson r with the contended p50, {label} ({len(runs)} boots)\n')
    print('| figure | r | min-max over boots |')
    print('|---|---|---|')
    for name, fn in items:
        x = [fn(r) for r in runs]
        print(f'| {name} | {pr(x, y):+.2f} | {min(x):.4g}-{max(x):.4g} |')

common = [('ruler counts, mean of min and max', lambda r: (r['rA'] + r['rB']) / 2),
          ('ruler max - min', lambda r: r['rB'] - r['rA']),
          ('IRQs on host CPUs 2-5', lambda r: sum(r['irqcpu'])),
          ('host freq CPU2-5 mean (MHz)', lambda r: st.mean(r['freq'])),
          ('null call p50', lambda r: r['lines'][0][1]),
          ('2 AS passive p50', lambda r: r['lines'][1][1]),
          ('own ctx p50', lambda r: r['lines'][3][1]),
          ('contended p99', lambda r: r['p99'])]
corr_table(cur, common, 'lb-cur')
sitems = common + [
    ('client vCPU', lambda r: r['F']['cli']),
    ('waiting share %', lambda r: r['F']['wait_share']),
    ('lock held share %', lambda r: r['F']['busy_share']),
    ('free gap per handover', lambda r: r['F']['gap']),
    ('2 x (client wait + hold)', lambda r: r['F']['client_rt_kernel']),
    ('client hold mean', lambda r: r['F']['c']['hold']),
    ('client hold p99', lambda r: r['F']['c']['hold_p99']),
    ('client wait mean', lambda r: r['F']['c']['wait']),
    ('client wait p99', lambda r: r['F']['c']['wait_p99']),
    ('busy hold mean', lambda r: r['F']['b']['hold']),
    ('busy hold p99', lambda r: r['F']['b']['hold_p99']),
    ('busy wait mean', lambda r: r['F']['b']['wait']),
    ('busy wait p99', lambda r: r['F']['b']['wait_p99'])] + [
    (f'{w} {k}', (lambda w, k: lambda r: r['F'][w][k])(w, k))
    for w in ('c', 'b') for k in ('sys', 'pick', 'next', 'poke', 'cr3', 'rest', 'timer')]
sitems = [(n.replace('c ', 'client ').replace('b ', 'busy ') if n[:2] in ('c ', 'b ') else n, f) for n, f in sitems]
corr_table(sts, sitems, 'lb-cur-stats')

# paired rounds: lb-cur boot k with lb-cur-stats boot k (booted next to each other)
pairs = [(a, b) for a in cur for b in sts if num(a) == num(b)]
if len(pairs) >= 3:
    print('\n### lb-cur boot k against lb-cur-stats boot k (booted back to back)\n')
    print(f"- r(lb-cur p50, stats p50) = {pr([a['p50'] for a, b in pairs], [b['p50'] for a, b in pairs]):+.2f}")
    for name, fn in [('2 x (client wait + hold)', lambda r: r['F']['client_rt_kernel']),
                     ('busy hold mean', lambda r: r['F']['b']['hold']),
                     ('free gap per handover', lambda r: r['F']['gap']),
                     ('client vCPU', lambda r: r['F']['cli'])]:
        print(f"- r(lb-cur p50, stats {name}) = {pr([a['p50'] for a, b in pairs], [fn(b) for a, b in pairs]):+.2f}")

# ---- lb-cur-order: the lock's order without the TSC (RUN_LOCK_ORDER) ----
def order(r):
    cs = []
    for l in open(r['f'], errors='replace'):
        m = re.match(r'lockorder: cpu (\d+) acq (\d+) fast (\d+) pred (\S+) depth (\S+)', l.strip())
        if m:
            cs.append({'acq': int(m[2]), 'fast': int(m[3]), 'pred': list(map(int, m[4].split('/'))),
                       'depth': list(map(int, m[5].split('/')))})
    cli = max(range(len(cs)), key=lambda i: cs[i]['fast'])
    busy = [i for i in range(len(cs)) if i != cli]
    O = {'cli': cli, 'host': r['pins'].get(cli), 'cs': cs}
    O['cli_acq'] = cs[cli]['acq']
    O['busy_acq'] = [cs[i]['acq'] for i in busy]
    n = sum(cs[cli]['depth'])
    O['cli_depth'] = [100 * x / n for x in cs[cli]['depth'][:4]]
    nb = sum(sum(cs[i]['depth']) for i in busy)
    O['busy_depth'] = [100 * sum(cs[i]['depth'][k] for i in busy) / nb for k in range(4)]
    O['cli_pred'] = [100 * x / sum(cs[cli]['pred']) for x in cs[cli]['pred'][:len(cs)]]
    # how often a busy CPU follows another busy CPU in the same order (a fixed rotation shows as one
    # predecessor per CPU near 100 %); the largest single predecessor share per busy CPU
    O['busy_pred_max'] = [100 * max(cs[i]['pred'][:len(cs)]) / sum(cs[i]['pred']) for i in busy]
    O['busy_after_cli'] = 100 * sum(cs[i]['pred'][cli] for i in busy) / nb
    return O

ords = [load(f) for f in boots('lb-cur-order')]
if ords:
    for r in ords: r['O'] = order(r)
    print('\n### lb-cur-order (no TSC in the kernel), per boot, sorted by contended p50\n')
    print('| boot | p50 | p99 | client vCPU (host CPU) | client acq | busy acq each | client depth 0/1/2/3 (%) | busy depth 0/1/2/3 (%) | client\'s predecessor CPU 0/1/2/3 (%) | busy: largest predecessor share each (%) | segment p50s min-median-max | ruler min-max | IRQs host CPUs 2/3/4/5 |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    for r in sorted(ords, key=lambda r: r['p50']):
        O = r['O']; s = [a for a, _ in r['seg']] if r['seg'] else [0]
        print(f"| {num(r)} | {r['p50']} | {r['p99']} | {O['cli']} ({O['host']}) | {O['cli_acq']} | {'/'.join(map(str, O['busy_acq']))} | "
              f"{'/'.join(f'{x:.1f}' for x in O['cli_depth'])} | {'/'.join(f'{x:.1f}' for x in O['busy_depth'])} | "
              f"{'/'.join(f'{x:.0f}' for x in O['cli_pred'])} | {'/'.join(f'{x:.0f}' for x in O['busy_pred_max'])} | "
              f"{min(s)}-{st.median(s):g}-{max(s)} | {r['rA']}-{r['rB']} | {'/'.join(map(str, r['irqcpu']))} |")
    print('\n### lb-cur-order, contended p50 by the client\'s vCPU\n')
    print('| client vCPU | boots | p50 each | median |')
    print('|---|---|---|---|')
    byv = collections.defaultdict(list)
    for r in ords: byv[r['O']['cli']].append(r['p50'])
    for v in sorted(byv):
        print(f"| {v} | {len(byv[v])} | {', '.join(map(str, sorted(byv[v])))} | {st.median(byv[v]):g} |")
    oitems = common + [('client vCPU', lambda r: r['O']['cli']),
                       ('client vCPU is 3', lambda r: float(r['O']['cli'] == 3)),
                       ('client depth 3 share', lambda r: r['O']['cli_depth'][3]),
                       ('client depth <3 share', lambda r: 100 - r['O']['cli_depth'][3]),
                       ('busy depth 3 share', lambda r: r['O']['busy_depth'][3]),
                       ('busy acquisitions, total', lambda r: sum(r['O']['busy_acq'])),
                       ('busy acquisitions per client acquisition', lambda r: sum(r['O']['busy_acq']) / r['O']['cli_acq']),
                       ('busy largest predecessor share, mean', lambda r: st.mean(r['O']['busy_pred_max']))]
    corr_table(ords, oitems, 'lb-cur-order')
    print('\n### stats build, contended p50 by the client\'s vCPU\n')
    print('| client vCPU | boots | p50 each | median |')
    print('|---|---|---|---|')
    byv = collections.defaultdict(list)
    for r in sts: byv[r['F']['cli']].append(r['p50'])
    for v in sorted(byv):
        print(f"| {v} | {len(byv[v])} | {', '.join(map(str, sorted(byv[v])))} | {st.median(byv[v]):g} |")
    print('\n### lb-cur, 10-boot windows\n')
    c = [r['p50'] for r in cur]
    for lo in range(0, len(c) - 9, 5):
        w = c[lo:lo + 10]
        print(f'- boots {lo + 1}-{lo + 10}: p50 {min(w)}-{max(w)}, spread {sp(w):.1f} %')
    print(f'- all {len(c)}: p50 median {st.median(c):g}, {min(c)}-{max(c)}, spread {sp(c):.1f} %')

# ---- placement test: lb-cur.iso with guest RAM in a kept /dev/shm file (A-D) or anonymous ----
groups = {g: [load(f) for f in boots(f'lb-cur-{g}')] for g in ('memA', 'memB', 'memC', 'memD', 'anon')}
if any(groups.values()):
    print('\n### Placement test: lb-cur.iso, guest RAM kept in one file across boots (memA-D) or ordinary (anon)\n')
    print('| RAM | boots | contended p50, boot by boot | median | spread within | contended p99 median | null call p50 range | 2 AS passive p50 range | own ctx p50 range |')
    print('|---|---|---|---|---|---|---|---|---|')
    meds = []
    for g, rs in groups.items():
        if not rs: continue
        p = [r['p50'] for r in sorted(rs, key=num)]
        if g != 'anon': meds.append(st.median(p))
        rng_ = lambda i: f"{min(r['lines'][i][1] for r in rs)}-{max(r['lines'][i][1] for r in rs)}"
        print(f"| {g} | {len(p)} | {', '.join(map(str, p))} | {st.median(p):g} | {sp(p):.1f} % | {st.median(r['p99'] for r in rs):g} | {rng_(0)} | {rng_(1)} | {rng_(3)} |")
    if len(meds) > 1:
        print(f'\n- spread of the four files\' medians: {min(meds):g}-{max(meds):g}, {100 * (max(meds) - min(meds)) / st.median(meds):.1f} %')
        allf = [r['p50'] for g in ('memA', 'memB', 'memC', 'memD') for r in groups[g]]
        print(f'- all file-backed boots together: {min(allf)}-{max(allf)}, spread {sp(allf):.1f} %')

# ---- the order build on two kept RAM files: does what is left follow the client's vCPU? ----
og = {g: [load(f) for f in boots(f'lb-cur-order-{g}')] for g in ('memE', 'memF')}
if any(og.values()):
    print('\n### lb-cur-order on kept RAM files, boot by boot: contended p50 (client vCPU)\n')
    print('| RAM | boots | p50 (client vCPU), boot by boot | spread | median by client vCPU |')
    print('|---|---|---|---|---|')
    for g, rs in og.items():
        for r in rs: r['O'] = order(r)
        rs = sorted(rs, key=num)
        byv = collections.defaultdict(list)
        for r in rs: byv[r['O']['cli']].append(r['p50'])
        print(f"| {g} | {len(rs)} | {', '.join(f'{r['p50']} ({r['O']['cli']})' for r in rs)} | {sp([r['p50'] for r in rs]):.1f} % | "
              + ', '.join(f'vCPU {v}: {st.median(p):g} ({len(p)})' for v, p in sorted(byv.items())) + ' |')
