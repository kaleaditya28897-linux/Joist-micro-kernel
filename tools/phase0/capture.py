#!/usr/bin/env python3
"""Classify GCC nested functions from -fdump-tree-nested dumps.

usage: capture.py SRC_ROOT BUILD_ROOT > nested-capture.csv

Reads every *.nested dump under BUILD_ROOT (produced by compiling with
-fdump-tree-nested; see capture-build.sh) and, for each nested function,
reports:

  captures     enclosing-frame variables the nested body reads or writes,
               i.e. every `CHAIN.n->var` in its lowered body
  addr_taken   whether an enclosing function builds a trampoline or
               descriptor for it (`__builtin_init_trampoline (&FRAME.n.name`),
               i.e. it escapes as a function pointer, typically a callback

and derives the effort class used in decision 0002:

  hoist        no captures: move it to file scope as a static function
  add-params   captures, but only called directly: pass the captures as
               parameters
  callback     captures and escapes as a pointer: the callback API must
               carry a context argument (the expensive case)

A nested function is recognised by the parent's local declaration
`static T name (...);` that GCC emits in the lowered parent.
"""
import csv, os, re, sys

src_root, build_root = sys.argv[1:]
w = csv.writer(sys.stdout)
w.writerow(['file', 'function', 'enclosing', 'captures', 'addr_taken', 'class'])

sig = re.compile(r'^[A-Za-z_].*?\b(\w+) \(.*\)$')
decl = re.compile(r'^\s+static .*?\b(\w+) \(.*\);$')
tramp = re.compile(r'__builtin_init_(?:trampoline|descriptor) \(&FRAME\.\d+\.(\w+),')
chain = re.compile(r'CHAIN\.\d+->(\w+)')

for d, _, files in os.walk(build_root):
    for fn in sorted(files):
        if not fn.endswith('.nested'):
            continue
        # Dump names look like `<obj>-<src>.c.NNNt.nested` or `<src>.c.NNNt.nested`.
        src = re.sub(r'\.\d+t\.nested$', '', fn).split('-', 1)[-1]
        rel = os.path.relpath(d, build_root)
        lines = open(os.path.join(d, fn), errors='replace').read().split('\n')
        bodies, cur, name = {}, None, None
        for i, l in enumerate(lines):
            if cur is None:
                m = sig.match(l)
                if m and i + 1 < len(lines) and lines[i + 1] == '{':
                    name, cur = m.group(1), []
            elif l == '}':
                bodies[name] = cur
                cur = None
            else:
                cur.append(l)
        parent = {}
        escapes = set()
        for fname, body in bodies.items():
            for l in body:
                m = decl.match(l)
                if m and m.group(1) in bodies:
                    parent[m.group(1)] = fname
                escapes.update(tramp.findall(l))
        for nf in sorted(parent):
            caps = sorted(set(v for l in bodies[nf] for v in chain.findall(l)))
            cls = 'hoist' if not caps else 'callback' if nf in escapes else 'add-params'
            w.writerow([os.path.join(rel, src), nf, parent[nf], ' '.join(caps),
                        'yes' if nf in escapes else 'no', cls])
