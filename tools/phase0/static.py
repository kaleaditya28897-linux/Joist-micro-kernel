#!/usr/bin/env python3
"""Static side of the Mach inventory: for every kernel call in gnumach.msgids,
find the Hurd/glibc components that call it (as `name (` or `__name (`).
usage: static.py gnumach.msgids hurd-src glibc-src > static.tsv
Columns: subsystem, call, #calling components, calling components,
implementing components (server side, `*_S_call`)."""
import os, re, sys
from collections import defaultdict
msgids, hurd, glibc = sys.argv[1:]
calls = {}
for l in open(msgids):
    f = l.split()
    if len(f) >= 3 and not f[2].endswith('_reply'):
        calls[f[2]] = f[0]
alt = '|'.join(sorted(calls, key=len, reverse=True))
rx = re.compile(r'(?:\b(?:__)?|(_S_))(' + alt + r')\s*\(')   # group 1 set => server implementation
use = defaultdict(set)
impl = defaultdict(set)
for root, tag in ((hurd, 'hurd'), (glibc, 'glibc')):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', '.pc', 'build-clang', 'debian', 'po', 'manual')]
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            p = os.path.join(d, fn)
            rel = os.path.relpath(p, root).split(os.sep)
            comp = tag + ':' + (rel[0] if tag == 'hurd' else '/'.join(rel[:3]) if rel[0] == 'sysdeps' else rel[0])
            try:
                txt = open(p, errors='replace').read()
            except OSError:
                continue
            for m in rx.finditer(txt):
                (impl if m.group(1) else use)[m.group(2)].add(comp)
for n in sorted(calls, key=lambda n: (calls[n], n)):
    print(f'{calls[n]}\t{n}\t{len(use[n])}\t{",".join(sorted(use[n]))}\t{",".join(sorted(impl[n]))}')
