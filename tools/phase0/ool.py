#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""List every Hurd RPC parameter that MIG may transfer out-of-line (OOL).

usage: ool.py HURD_SRC GNUMACH_SRC > ool-params.csv

A parameter is OOL-capable when its type is, directly or through a `type`
alias, an unbounded array (`array[] of …`) or a pointer-array (`^array…`),
which is how MIG decides on out-of-line transfer. Type aliases are collected
from gnumach's include/mach/*.defs and the Hurd's hurd/*.defs; routines are
read from the Hurd's hurd/*.defs (the interfaces the Hurd's own servers
export).

Columns: defs file, routine, direction, parameter, type, flags. The flags
are MIG's per-argument annotations after preprocessing, e.g. `dealloc` (sender's copy is
deallocated on send, the §6 "sender-side deallocation" semantics) and
`servercopy`.
"""
import csv, glob, os, re, subprocess, sys

hurd, gnumach = sys.argv[1:]

def cpp(f):
    """Preprocess a .defs file the way MIG does, so macros such as RPT
    (reply port) and SCP (servercopy) are expanded."""
    return subprocess.run(['cpp', '-P', '-undef', '-D__i386__', '-DTypeCheck=1',
                           '-I', hurd, '-I', os.path.join(hurd, 'hurd'),
                           '-I', os.path.join(gnumach, 'include'), f],
                          capture_output=True, text=True, check=True).stdout

def clean(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    t = re.sub(r'//[^\n]*', '', t)
    return re.sub(r'^\s*#[^\n]*', '', t, flags=re.M)

files = sorted(glob.glob(os.path.join(gnumach, 'include/mach/*.defs'))) + \
        sorted(glob.glob(os.path.join(hurd, 'hurd/*.defs')))
alias = {}
for f in files:
    for name, rhs in re.findall(r'\btype\s+(\w+)\s*=\s*([^;]+);', clean(open(f).read())):
        alias[name] = rhs.strip()

def ool(t, depth=0):
    t = t.strip()
    if re.match(r'\^?\s*array\s*\[\s*\]', t) or t.startswith('^'):
        return True
    base = re.match(r'(\w+)', t)
    return bool(base and depth < 10 and base.group(1) in alias and ool(alias[base.group(1)], depth + 1))

w = csv.writer(sys.stdout)
w.writerow(['defs', 'routine', 'direction', 'parameter', 'type', 'flags'])
for f in sorted(glob.glob(os.path.join(hurd, 'hurd/*.defs'))):
    text = clean(cpp(f))
    for kind, name, params in re.findall(r'\b(routine|simpleroutine)\s+(\w+)\s*\((.*?)\)\s*;', text, re.S):
        for p in params.split(';'):
            p = ' '.join(p.split())
            m = re.match(r'(?:(in|out|inout|requestport|replyport|sreplyport|ureplyport|waittime|msgoption|msgseqno)\s+)?(\w+)\s*:\s*(.+)', p)
            if not m:
                continue
            direction, pname, rest = m.group(1) or 'in', m.group(2), m.group(3)
            parts = [x.strip() for x in rest.split(',')]
            ptype, flags = parts[0], ' '.join(parts[1:])
            if direction in ('in', 'out', 'inout') and ool(ptype):
                w.writerow([os.path.basename(f), name, direction, pname, ptype, flags])
