#!/usr/bin/env python3
"""Classify GCC nested functions by whether they capture enclosing locals.

Input:  the same trees nested.py scans, plus nested-functions.csv rows.
Output: TSV  tree, file, line, function, verdict, captured-names

verdict is one of:
  none     - references nothing from the enclosing function's scope
  sibling  - references only sibling nested functions, no enclosing data
  capture  - references at least one enclosing parameter or local variable

Heuristic, like nested.py. It strips comments/strings/preprocessor, brace-matches
to find the enclosing function and the nested body, collects the enclosing
function's parameters and its declared locals, and reports which of those names
appear as identifiers in the nested body and are not shadowed by the nested
function's own parameters or locals. Over-reporting is possible when a local
name matches a global or a struct field used in the nested body; under-reporting
is possible when a declaration form is not recognised. Spot-check before relying
on the exact number.
usage: captures.py TREE_DIR...
"""
import os, re, sys

DECLSTART = re.compile(
    r'(?:^|[;{}])\s*((?:(?:register|auto|const|volatile|static|struct|union|enum|unsigned|signed'
    r'|char|short|int|long|float|double|void|\w+_t|\w+)\b[\s\*]+)+)')
IDENT = re.compile(r'\b[A-Za-z_]\w*\b')
HDR = re.compile(r'(\w+)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?:__attribute__\s*\(\(.*?\)\)\s*)?\{', re.S)
KW = {'if','for','while','switch','return','sizeof','else','do','case','default','goto','break',
      'continue','struct','union','enum','typedef','static','const','void','int','char','long',
      'short','unsigned','signed','float','double','register','volatile','auto','inline','extern'}

def strip(t):
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group().count('\n'), t, flags=re.S)
    t = re.sub(r'//[^\n]*', '', t)
    t = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', t)
    t = re.sub(r"'(?:\\.|[^'\\\n])*'", "''", t)
    return re.sub(r'^\s*#[^\n]*(?:\\\n[^\n]*)*', lambda m: '\n' * m.group().count('\n'), t, flags=re.M)

def depths(t):
    d, out = 0, [0]*(len(t)+1)
    for i, ch in enumerate(t):
        out[i] = d
        d += (ch == '{') - (ch == '}')
    return out

def match_brace(t, i):
    """i is index of '{'; return index just past its matching '}'."""
    d = 0
    for j in range(i, len(t)):
        if t[j] == '{': d += 1
        elif t[j] == '}':
            d -= 1
            if d == 0: return j+1
    return len(t)

def decl_names(text):
    """All declarator names in a block, including 2nd+ declarators of a comma list.

    `struct idvec *a = f (), *b = g ();` yields {a, b}, which the previous
    single-name regex missed -- it is the commonest form in this code base.
    """
    out = set()
    for m in DECLSTART.finditer(text):
        rest = text[m.end():]
        end = rest.find(';')
        if end < 0:
            continue
        body = rest[:end]
        if '}' in body:
            continue
        # A brace here is a struct/array initialiser, e.g. `struct s x = {1,2};`.
        # Keep the declarator, drop the initialiser -- the earlier version
        # skipped the whole declaration and lost the name.
        br = body.find('{')
        if br >= 0:
            body = body[:br].rstrip().rstrip('=').rstrip()
        depth = 0
        parts, cur = [], []
        for ch in body:                      # split on top-level commas only
            if ch in '([': depth += 1
            elif ch in ')]': depth -= 1
            if ch == ',' and depth == 0:
                parts.append(''.join(cur)); cur = []
            else:
                cur.append(ch)
        parts.append(''.join(cur))
        for i, d in enumerate(parts):
            d = d.split('=')[0]
            ids = [x for x in IDENT.findall(d) if x not in KW]
            if not ids:
                continue
            # first declarator still carries the type words; later ones do not
            out.add(ids[-1] if i == 0 else ids[0] if len(ids) == 1 else ids[-1])
    return out

def names_in_params(params):
    out = set()
    for p in params.split(','):
        p = p.strip()
        if not p or p == 'void': continue
        ids = IDENT.findall(p)
        ids = [x for x in ids if x not in KW]
        if ids: out.add(ids[-1])
    return out

def analyse(path, tree):
    t = strip(open(path, errors='replace').read())
    dep = depths(t)
    funcs = []                       # (start_of_body, end_of_body, name, params, hdrpos)
    for m in HDR.finditer(t):
        name = m.group(1)
        if name in KW: continue
        b = t.index('{', m.end(2) if m.end(2) <= m.end() else m.start())
        b = m.end() - 1              # the '{' the regex consumed
        funcs.append((b, match_brace(t, b), name, m.group(2), m.start(1), dep[m.start(1)]))
    for b, e, name, params, hpos, d in funcs:
        if d <= 0:                   # top-level function, not nested
            continue
        # innermost enclosing function = smallest enclosing body among depth-0 headers
        enc = None
        for b2, e2, n2, p2, h2, d2 in funcs:
            if b2 < hpos and e2 > e and (enc is None or b2 > enc[0]):
                enc = (b2, e2, n2, p2)
        if enc is None: continue
        eb, ee, ename, eparams = enc
        outer = t[eb:b] + t[e:ee]                       # enclosing body minus the nested body
        scope = names_in_params(eparams) | decl_names(outer)
        # sibling nested functions declared inside the same enclosing body
        sibs = {n2 for b2, e2, n2, p2, h2, d2 in funcs
                if b2 > eb and e2 < ee and not (b2 >= b and e2 <= e) and n2 != name}
        body = t[b:e]
        own = names_in_params(params) | decl_names(body)
        used = {x for x in IDENT.findall(body) if x not in KW}
        cap = sorted((used & scope) - own - sibs - {name, ename})
        sib = sorted((used & sibs) - own - {name, ename})
        verdict = 'capture' if cap else ('sibling' if sib else 'none')
        line = t.count('\n', 0, hpos) + 1
        rel = os.path.relpath(path, os.path.dirname(tree.rstrip('/')))
        print(f"{os.path.basename(tree.rstrip('/'))}\t{rel.split('/',1)[-1]}\t{line}\t{name}\t"
              f"{verdict}\t{';'.join(cap) or ';'.join(sib)}")

for root in sys.argv[1:]:
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in ('.git','.pc','build-clang','build-gcc','debian')]
        for fn in files:
            if fn.endswith('.c'):
                analyse(os.path.join(d, fn), root)
