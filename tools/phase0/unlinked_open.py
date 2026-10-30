#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""How often is a file used after it was unlinked? (client-recovery §8 Q2)

usage: unlinked_open.py build.txt > unlinked-open.csv

Reads an rpctrace log. For each process (pidN) it remembers, by final path
component, the port each successful `dir_lookup` returned (`A<--B(pidN)`
notation). When the same process later calls `dir_unlink (name)`, every port
it opened under that name is marked unlinked. Any later io_* or file_* RPC on
such a port is a *use after unlink*: the file is open, nameless, and still in
use. That's exactly the state a server restart cannot recover (§3).

Limits, all of which make this an undercount:
- Names are matched on the last path component, and rpctrace truncates
  strings to 16 characters (-s 16), so longer names can collide or miss.
- Only a lookup and an unlink in the *same* process are paired. A file
  opened by one process and unlinked by another (e.g. a shell opening a
  temporary file that a child deletes) is not seen.
- The trace ends at the gsync_wait deadlock (the first `make`), so only the
  configure part of the build is covered.
- `tmpfile()`/`O_TMPFILE` files are created by `dir_mkfile` without any name
  and never unlinked; they are counted separately (`dir_mkfile` calls).

Output: one row per unlinked name that saw use after unlink, with the
number of later RPCs and the kinds of RPC.
"""
import csv, re, sys
from collections import defaultdict, Counter

line_rx = re.compile(r'^\s*(\S+<--\S+\(pid(-?\d+)\))->(\w+) \((.*)$')
lookup_rx = re.compile(r'^"([^"]*)" .*?\) = 0 \d+ "[^"]*"\s+(\S+<--\S+\(pid-?\d+\))')
unlink_rx = re.compile(r'^"([^"]*)"\) = 0')

opened = defaultdict(lambda: defaultdict(set))   # pid -> name -> {port}
unlinked = {}                                    # port -> (pid, name)
uses = defaultdict(Counter)                      # (pid, name) -> Counter(rpc)
n_unlink = n_mkfile = 0

for line in open(sys.argv[1], errors='replace'):
    m = line_rx.match(line)
    if not m:
        continue
    port, pid, rpc, rest = m.groups()
    if rpc == 'dir_lookup':
        l = lookup_rx.match(rest)
        if l:
            name = l.group(1).rstrip('/').rsplit('/', 1)[-1]
            opened[pid][name].add(l.group(2))
            unlinked.pop(l.group(2), None)       # a re-used port name is a new file
    elif rpc == 'dir_unlink':
        u = unlink_rx.match(rest)
        if u:
            n_unlink += 1
            name = u.group(1).rsplit('/', 1)[-1]
            for p in opened[pid].get(name, ()):
                unlinked[p] = (pid, name)
    elif rpc == 'dir_mkfile':
        n_mkfile += 1
    elif port in unlinked and (rpc.startswith('io_') or rpc.startswith('file_')):
        uses[unlinked[port]][rpc.removesuffix('_request')] += 1

w = csv.writer(sys.stdout)
w.writerow(['pid', 'name', 'rpcs_after_unlink', 'kinds'])
for (pid, name), c in sorted(uses.items(), key=lambda kv: -sum(kv[1].values())):
    w.writerow([pid, name, sum(c.values()), ' '.join(f'{k}:{v}' for k, v in c.most_common())])
print(f'successful dir_unlink: {n_unlink}; names used after unlink: {len(uses)} '
      f'in {len({p for p, _ in uses})} processes; dir_mkfile: {n_mkfile}', file=sys.stderr)
