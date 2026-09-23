#!/usr/bin/env python3
"""Tally Hurd server RPCs (everything in /usr/share/msgids except gnumach.msgids)
and the offset/whence arguments of io_read, io_write and io_seek.
usage: hurdrpc.py boot.txt shell.txt build.txt
Writes hurd-rpc-counts.csv and io-offsets.csv to the current directory."""
import csv, glob, re, sys
from collections import Counter
sub = {}
for f in sorted(glob.glob('/usr/share/msgids/*.msgids')):
    k = f.rsplit('/', 1)[1][:-7]
    for l in open(f):
        p = l.split()
        if len(p) >= 3:
            sub.setdefault(p[2], (p[0], k))
rx = re.compile(r'->([a-z_0-9]+) \(')
names = [p.rsplit('/', 1)[-1].split('.')[0] for p in sys.argv[1:]]
cs, offs = [], []
for path in sys.argv[1:]:
    c, o = Counter(), Counter()
    for l in open(path, errors='replace'):
        m = rx.search(l)
        if not (m and m.group(1) in sub and sub[m.group(1)][1] != 'gnumach'):
            continue
        n = m.group(1).removesuffix('_request')
        c[n] += 1
        if n == 'io_read':
            a = re.search(r'io_read_request \((-?\d+) ', l)
            o['io_read', 'offset=-1' if a and a.group(1) == '-1' else 'explicit'] += 1
        elif n == 'io_write':
            a = re.search(r' (-?\d+)\) = ', l)
            o['io_write', 'offset=-1' if a and a.group(1) == '-1' else 'explicit'] += 1
        elif n == 'io_seek':
            a = re.search(r'io_seek_request \(-?\d+ (\d)\)', l)
            o['io_seek', {'0': 'SEEK_SET', '1': 'SEEK_CUR', '2': 'SEEK_END'}.get(a.group(1) if a else '', 'unparsed')] += 1
    cs.append(c); offs.append(o)
with open('hurd-rpc-counts.csv', 'w', newline='') as fh:
    w = csv.writer(fh); w.writerow(['subsystem', 'rpc'] + names)
    for n in sorted(set().union(*cs), key=lambda n: -sum(c[n] for c in cs)):
        w.writerow([sub.get(n, sub.get(n + '_request'))[0], n] + [c[n] for c in cs])
with open('io-offsets.csv', 'w', newline='') as fh:
    w = csv.writer(fh); w.writerow(['rpc', 'argument'] + names)
    for k in sorted(set().union(*offs)):
        w.writerow(list(k) + [o[k] for o in offs])
