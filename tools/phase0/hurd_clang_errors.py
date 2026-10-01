#!/usr/bin/env python3
"""Classify clang errors from the per-directory Hurd build (spike-hurd-clang.sh).

usage: hurd_clang_errors.py LOGDIR nested-functions.csv > hurd-clang-errors.csv

LOGDIR holds one <dir>.log per Hurd library/program directory. Every distinct
`file:line:col: error: message` is classified, with the same rules as
clang_errors.py uses for glibc:

  nested       "function definition is not allowed here", or an undeclared
               function/identifier whose name is a nested function defined in
               the same file (clang rejects the definition, so uses fail too)
  nested-file  in a file that contains nested functions, not one of the above
  other        in a file with no nested functions: what remains to port once
               nested functions are converted
Link errors are not compile errors and are not counted (they come from
libraries that failed to build, which is expected here).
Columns: class, dir, file, line, message, count.
"""
import csv, glob, os, re, sys
from collections import Counter, defaultdict

logdir, nested_csv = sys.argv[1:]
nested = defaultdict(set)
for r in csv.DictReader(open(nested_csv)):
    if r['tree'].startswith('hurd'):
        nested[r['file']].add(r['function'])

err = re.compile(r'^(\S+?):(\d+):\d+: error: (.*)$')
counts = Counter()
for log in sorted(glob.glob(os.path.join(logdir, '*.log'))):
    d = os.path.basename(log)[:-4]
    for line in open(log, errors='replace'):
        m = err.match(line.rstrip('\n'))
        if m:
            path, ln, msg = m.groups()
            # paths are relative to the directory being built
            rel = os.path.normpath(os.path.join(d, path))
            counts[(d, rel, int(ln), msg)] += 1

w = csv.writer(sys.stdout)
w.writerow(['class', 'dir', 'file', 'line', 'message', 'count'])
for (d, f, ln, msg), n in sorted(counts.items()):
    names = nested.get(f, set())
    q = re.findall(r"'([A-Za-z_]\w*)'", msg)
    if 'function definition is not allowed here' in msg:
        cls = 'nested'
    elif names and q and q[0] in names and 'undeclared' in msg:
        cls = 'nested'
    elif names:
        cls = 'nested-file'
    else:
        cls = 'other'
    w.writerow([cls, d, f, ln, msg, n])
