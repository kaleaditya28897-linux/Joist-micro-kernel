#!/usr/bin/env python3
"""Find GCC nested function definitions (unsupported by clang).
Heuristic: strip comments/strings, track brace depth, and report a function
definition header `type name (args) {` that starts while depth > 0.
usage: nested.py DIR...   -> TSV: file, line, function name"""
import os, re, sys
hdr = re.compile(r'(?:^|[;{}])\s*(?:(?:static|inline|auto|const|unsigned|signed|struct|enum|__always_inline|error_t|int|void|char|long|short|size_t|ssize_t|kern_return_t|mach_port_t|file_t|io_t|\w+_t)\b[\s\*]*)+(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:__attribute__\s*\(\(.*?\)\)\s*)?\{', re.S)
kw = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do'}
def strip(t):
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group().count('\n'), t, flags=re.S)
    t = re.sub(r'//[^\n]*', '', t)
    t = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', t)
    t = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", t)
    return re.sub(r'^\s*#[^\n]*(?:\\\n[^\n]*)*', lambda m: '\n' * m.group().count('\n'), t, flags=re.M)
for root in sys.argv[1:]:
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git', '.pc', 'build-clang', 'debian')]
        for fn in files:
            if not fn.endswith('.c'):
                continue
            p = os.path.join(d, fn)
            t = strip(open(p, errors='replace').read())
            depth = [0] * (len(t) + 1)
            dd = 0
            for i, ch in enumerate(t):
                depth[i] = dd
                dd += (ch == '{') - (ch == '}')
            for m in hdr.finditer(t):
                name = m.group(1)
                if name in kw or depth[m.start(1)] <= 0:
                    continue
                print(f'{p}\t{t.count(chr(10), 0, m.start(1)) + 1}\t{name}')
