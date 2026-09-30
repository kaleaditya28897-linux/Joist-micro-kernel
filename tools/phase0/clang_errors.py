#!/usr/bin/env python3
"""Classify clang errors from a glibc build log for decision 0002.

usage: clang_errors.py glibc-make.log nested-functions.csv GLIBC_SRC > clang-glibc-errors.csv

Every distinct `file:line:col: error: message` line is classified:

  nested      the error is a GCC nested function or a direct consequence of one:
              "function definition is not allowed here", or an "undeclared
              function/identifier" whose name is a nested function defined in
              the same source file (clang rejects the definition, so every use
              of the name fails too).
  nested-file the error is in a file that contains nested functions but is not
              recognisably one of the above. Some of these are further
              knock-ons (e.g. a type used only inside the rejected body); they
              are kept apart so the "other" count stays honest.
  other       the error is in a file with no nested functions at all. These
              are the clang problems that remain after nested functions are
              converted: the part of the error surface 0002 had not measured.

Errors are located by basename, because glibc's log prints paths relative to
each subdirectory. Where a basename is ambiguous in glibc-src, the log's
directory context (the most recent "Entering directory") breaks the tie.
Output columns: class, subdir, file, line, message, count (identical lines in
the log, e.g. the same header error reported once per including file).
"""
import csv, os, re, sys
from collections import Counter, defaultdict

log, nested_csv, src = sys.argv[1:]

nested_by_file = defaultdict(set)
for r in csv.DictReader(open(nested_csv)):
    if r['tree'].startswith('glibc'):
        nested_by_file[os.path.basename(r['file'])].add(r['function'])
        nested_by_file[r['file']].add(r['function'])

enter = re.compile(r"make\[\d+\]: Entering directory '.*?/glibc-[^/]+/([^']*)'")
err = re.compile(r'^(\S+?):(\d+):(\d+): error: (.*)$')
name_in = re.compile(r"'([A-Za-z_]\w*)'")

counts = Counter()
subdir = ''
for line in open(log, errors='replace'):
    m = enter.search(line)
    if m:
        subdir = m.group(1)
        continue
    m = err.match(line.rstrip('\n'))
    if m:
        path, ln, _, msg = m.groups()
        path = re.sub(r'^(\.\./)+', '', path)
        counts[(subdir, path, int(ln), msg)] += 1

w = csv.writer(sys.stdout)
w.writerow(['class', 'subdir', 'file', 'line', 'message', 'count'])
for (subdir, path, ln, msg), n in sorted(counts.items()):
    base = os.path.basename(path)
    names = nested_by_file.get(path) or nested_by_file.get(os.path.join(subdir, base)) \
        or nested_by_file.get(base, set())
    quoted = name_in.findall(msg)
    if 'function definition is not allowed here' in msg:
        cls = 'nested'
    elif names and quoted and quoted[0] in names and 'undeclared' in msg:
        cls = 'nested'
    elif names:
        cls = 'nested-file'
    else:
        cls = 'other'
    w.writerow([cls, subdir, path, ln, msg, n])
