#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Which clang "undeclared" errors come from declarations hidden behind
`#if defined __USE_EXTERN_INLINES` (decision 0002, 2026-09-30).

glibc's include/features.h deliberately leaves __USE_EXTERN_INLINES undefined
when glibc itself is built with clang (`!(defined __clang__ && defined _LIBC)`).
Hurd-port headers declare ordinary variables and functions inside such
blocks, so under clang those declarations vanish.

usage: extern_inline_guard.py GLIBC_SRC clang-glibc-errors.csv > out.csv

Collects every identifier declared inside (extern declarations, GNU-style
function definitions with the name at column 0, and #defines) an #if/#ifdef block whose condition
mentions __USE_EXTERN_INLINES, in headers under hurd/, mach/, htl/ and
sysdeps/{mach,hurd,htl}. Then marks each undeclared-identifier/function error
whose name is one of them. Output: the error CSV with two extra columns,
guard_hidden (yes/no) and declared_in (header:line of the guarded declaration).
"""
import csv, os, re, sys

src, errors = sys.argv[1:]
roots = ['hurd', 'mach', 'htl', 'sysdeps/mach', 'sysdeps/hurd', 'sysdeps/htl']
decl = re.compile(r'\b([A-Za-z_]\w*)\s*(?:\(|;|\[|=)')
hidden = {}
for r in roots:
    for d, _, files in os.walk(os.path.join(src, r)):
        for fn in files:
            if not fn.endswith('.h'):
                continue
            p = os.path.join(d, fn)
            stack = []
            for i, line in enumerate(open(p, errors='replace'), 1):
                s = line.strip()
                dm = re.match(r'#\s*define\s+([A-Za-z_]\w*)', s)
                if dm and any(stack):
                    hidden.setdefault(dm.group(1), f'{os.path.relpath(p, src)}:{i}')
                    continue
                m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', s)
                if m:
                    k, cond = m.groups()
                    if k in ('if', 'ifdef', 'ifndef'):
                        stack.append('__USE_EXTERN_INLINES' in cond and '!' not in cond.split('__USE_EXTERN_INLINES')[0][-3:])
                    elif k in ('elif', 'else') and stack:
                        stack[-1] = False
                    elif k == 'endif' and stack:
                        stack.pop()
                    continue
                f = re.match(r'([A-Za-z_]\w*)\s*\(', line)   # GNU style: function name at column 0
                if any(stack) and f and f.group(1) not in ('if', 'while', 'for', 'switch', 'return'):
                    hidden.setdefault(f.group(1), f'{os.path.relpath(p, src)}:{i}')
                    continue
                if any(stack) and s.startswith('extern'):
                    for name in decl.findall(s.split('/*')[0]):
                        if name not in ('extern', 'struct', 'int', 'void', 'char', 'unsigned', 'const'):
                            hidden.setdefault(name, f'{os.path.relpath(p, src)}:{i}')
                            break

rows = list(csv.DictReader(open(errors)))
w = csv.writer(sys.stdout)
w.writerow(list(rows[0].keys()) + ['guard_hidden', 'declared_in'])
for r in rows:
    q = re.findall(r"'([A-Za-z_]\w*)'", r['message'])
    hit = 'undeclared' in r['message'] and q and q[0] in hidden
    w.writerow(list(r.values()) + ['yes' if hit else 'no', hidden.get(q[0], '') if hit else ''])
print(f'{len(hidden)} identifiers declared behind __USE_EXTERN_INLINES', file=sys.stderr)
