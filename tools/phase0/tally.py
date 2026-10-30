#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tally Mach kernel RPCs in rpctrace logs.

usage: tally.py gnumach.msgids boot.txt shell.txt build.txt
Prints a markdown table: subsystem | call | boot | shell | build.
Only calls defined in gnumach.msgids (the kernel's interfaces) are counted;
Hurd server RPCs (io_*, dir_*, proc_*, ...) are excluded.
Also writes mach-call-counts.csv (every defined call, zeros included).
"""
import csv, re, sys
from collections import Counter

msgids, *logs = sys.argv[1:]
sub = {}
for line in open(msgids):
    f = line.split()
    if len(f) >= 3:
        sub[f[2]] = f[0]

call = re.compile(r'->([a-z_0-9]+) \(')
counts = []
for path in logs:
    c = Counter()
    with open(path, errors='replace') as fh:
        for line in fh:
            m = call.search(line)
            if m and m.group(1) in sub:
                c[m.group(1)] += 1
    counts.append(c)

names = sorted(set().union(*counts), key=lambda n: (sub[n], -sum(c[n] for c in counts)))
with open('mach-call-counts.csv', 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['subsystem', 'call'] + [l.rsplit('/', 1)[-1].split('.')[0] for l in logs])
    for n in sorted(sub, key=lambda n: (sub[n], n)):
        w.writerow([sub[n], n] + [c[n] for c in counts])
print('| Subsystem | Call | ' + ' | '.join(l.rsplit('/', 1)[-1].split('.')[0] for l in logs) + ' |')
print('|---|---|' + '---:|' * len(logs))
for n in names:
    print(f'| {sub[n]} | `{n}` | ' + ' | '.join(str(c[n]) for c in counts) + ' |')
print()
print('Totals: ' + ', '.join(f'{sum(c.values())}' for c in counts))
unused = sorted(n for n in sub if not any(c[n] for c in counts))
print(f'\nDefined but not observed ({len(unused)}):')
for s in sorted(set(sub[n] for n in unused)):
    print(f'- {s}: ' + ', '.join(n for n in unused if sub[n] == s))
