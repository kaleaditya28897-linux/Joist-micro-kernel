#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Inventory of Mach traps (syscalls, invisible to rpctrace).

usage: traps.py syscall_sw.h gnumach-src hurd-src glibc-src > traps.csv

For every `kernel_trap(name, number, nargs)` in the user header, reports
whether GNU Mach implements it (MACH_TRAP/MACH_TRAP_STACK entry in
kern/syscall_sw.c) and which components reference it (`name`, `__name`, or
the header's `syscall_name` form), counting references in .c/.h/.S files
outside the header itself."""
import csv, os, re, sys
hdr, gm, hurd, glibc = sys.argv[1:]
traps = re.findall(r'^kernel_trap\((\w+),(-?\d+),(\d+)\)', open(hdr).read(), re.M)
table = open(os.path.join(gm, 'kern/syscall_sw.c')).read()
impl = set(re.findall(r'MACH_TRAP(?:_STACK)?\((\w+),', table))
rx = re.compile(r'\b(?:__)?(' + '|'.join(sorted((t[0] for t in traps), key=len, reverse=True)) + r')\b')
use = {t[0]: set() for t in traps}
for root, tag in ((hurd, 'hurd'), (glibc, 'glibc')):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', '.pc', 'build-clang', 'build-gcc', 'debian', 'po', 'manual')]
        for fn in files:
            if not fn.endswith(('.c', '.h', '.S')) or fn == 'syscall_sw.h':
                continue
            p = os.path.join(d, fn)
            rel = os.path.relpath(p, root).split(os.sep)
            comp = tag + ':' + ('/'.join(rel[:3]) if rel[0] == 'sysdeps' else rel[0])
            for m in rx.finditer(open(p, errors='replace').read()):
                use[m.group(1)].add(comp)
w = csv.writer(sys.stdout)
w.writerow(['trap', 'number', 'nargs', 'implemented_by_gnumach', 'n_components', 'components'])
for name, num, n in traps:
    w.writerow([name, num, n, 'yes' if name in impl else 'no', len(use[name]), ','.join(sorted(use[name]))])
