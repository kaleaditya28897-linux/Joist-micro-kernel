#!/usr/bin/env python3
"""Find GCC nested function definitions (unsupported by clang).

usage: nested.py PATH...  (directories are walked, files scanned) -> TSV: file, line, function name

Version 2 (2026-09-30). Version 1 tracked brace depth with regexes over
comment- and string-stripped text. It desynchronised whenever the text still
held an unbalanced brace, most often in the branches of an #if/#else, and then
silently dropped every nested function after that point in the file (see
docs/phase0-data/README.md, stage 6).

This version tokenizes instead:

- comments, string and character literals (including '{' and '\\''), and
  backslash-newline continuations are consumed as whole tokens, so a brace
  inside any of them is never counted;
- preprocessor directives are read as whole logical lines. Conditionals are
  tracked the way a compiler sees one configuration: every branch of an
  #if/#elif/#else chain starts at the depth the chain started at, and after
  #endif the depth continues from the branch taken in ONE consistent
  configuration (see branch_taken). All branches are still scanned, so a
  nested function inside any branch is reported;
- `#if 0` (and `#if (0)`) branches are skipped entirely: they are dead code.

Braces are tracked as a stack of what each one opened (see brace_kind), and
a nested function is reported when a `{` inside a *code* brace closes a declarator
`TYPE NAME ( ... ) [__attribute__((...))] {`, where the token before NAME is a
type word, a `*`, or an identifier (a typedef name), and NAME is not a
control keyword. That shape cannot be a statement, so false positives need a
macro that expands to one; none are known.

Headers are scanned too (since 2026-09-30): glibc's sysdeps/htl/pt-key.h
defines a nested function (`do_init`) inside a static inline function, and
clang reported it while v2 was still scanning only .c files.

Known limitation: deliberately unbalanced code inside a conditional that is
never compiled (GNU Mach's linux/src/drivers/scsi/53c78xx.c wraps half an
if/else in `#ifdef notyet`) drifts the brace stack, because `notyet` is
assumed true like every other macro. A rule preferring "not taken" for such
chains fixed that file but broke GNU Mach's pmap.c and i82365.c, and was
removed (2026-10-01). Where a compiler has built the configuration, its
"function definition is not allowed here" errors are the authority; this
scanner is for code no compiler has reached. tests/nested/known-limits/ holds
the failing shape.

Regression test: docs/phase0-data/trampolines.csv. GCC generated a trampoline
for each of its 80 rows, so each is a nested function; a correct scanner finds
all 80.
"""
import os, re, sys

KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'else', 'do',
            'case', 'goto', '__attribute__', '__typeof__', 'typeof', '__asm__',
            'asm', '_Alignof', '__alignof__', '_Generic', '__extension__'}
# Tokens that can end a type in a declaration: `int f (`, `char *f (`, `foo_t f (`.
NOT_TYPE_END = {'=', ',', '(', '[', '?', ':', '!', '&&', '||', '+', '-', '/',
                '%', '<', '>', '==', '!=', '<=', '>=', '&', '|', '^', '~',
                '.', '->', 'return', 'else', 'case', 'goto', 'sizeof', ';',
                '{', '}'}

TOKEN = re.compile(r'''
    (?P<ws>[ \t\f\v\r]+|\\\n)
  | (?P<nl>\n)
  | (?P<comment>/\*.*?\*/|//(?:\\\n|[^\n])*)
  | (?P<str>"(?:\\.|\\\n|[^"\\\n])*")
  | (?P<chr>'(?:\\.|[^'\\\n])*')
  | (?P<id>[A-Za-z_]\w*)
  | (?P<num>\.?\d(?:[\w.]|[eEpP][+-])*)
  | (?P<op>->|\+\+|--|<<=?|>>=?|[<>=!]=|&&|\|\||\#\#|[-+*/%&|^~!<>=?:;,.(){}\[\]\#])
  | (?P<other>.)
''', re.S | re.X)
DIRECTIVE = re.compile(r'[ \t]*#[ \t]*(\w*)(.*)')
IF_ZERO = re.compile(r'\s*\(?\s*0\s*\)?\s*(?:/\*.*\*/|//.*)?$')


def logical_lines_directive(text, pos):
    """Return (directive, argument, end) for a directive starting at pos."""
    end = pos
    while True:
        nl = text.find('\n', end)
        if nl < 0:
            end = len(text)
            break
        if nl > 0 and text[nl - 1] == '\\':
            end = nl + 1
            continue
        end = nl
        break
    body = text[pos:end].replace('\\\n', ' ')
    m = DIRECTIVE.match(body)
    return (m.group(1), m.group(2), end) if m else ('', '', end)


def scan(path):
    text = open(path, errors='replace').read()
    found = []
    stack = []              # what each open '{' opened: 'code', 'aggregate' or 'linkage'
    line = 1
    toks = []               # significant tokens in the current live region: (kind, value, line)
    cond = []               # see the #if handling below
    assumed = {}            # macro -> truth value assumed at first test, reused after
    at_line_start = True
    pos = 0
    n = len(text)
    while pos < n:
        if at_line_start:
            m = re.compile(r'[ \t]*#').match(text, pos)
            if m:
                d, arg, end = logical_lines_directive(text, pos)
                line += text.count('\n', pos, end)
                pos = end
                if d in ('if', 'ifdef', 'ifndef'):
                    dead = d == 'if' and IF_ZERO.match(arg) is not None
                    taken = (not dead) and branch_taken(d, arg, assumed)
                    # frame: [entry stack, end stack of the taken branch, dead, any taken,
                    #         this branch taken, chain has an #else]
                    cond.append([list(stack), None, dead, taken, taken, False])
                elif d in ('elif', 'else') and cond:
                    f = cond[-1]
                    if f[4]:
                        f[1] = list(stack)    # remember where the taken branch ended
                    stack = list(f[0])
                    dead = d == 'elif' and IF_ZERO.match(arg) is not None
                    now = (not f[3]) and (not dead) and (d == 'else' or branch_taken('if', arg, assumed))
                    f[2], f[3], f[4] = dead, f[3] or now, now
                    if d == 'else':
                        f[5] = True
                elif d == 'endif' and cond:
                    f = cond.pop()
                    if f[4]:
                        f[1] = list(stack)
                    stack = f[1] if f[1] is not None else list(f[0])
                continue
            at_line_start = False
        m = TOKEN.match(text, pos)
        kind = m.lastgroup
        val = m.group(kind)
        pos = m.end()
        line_here = line
        line += val.count('\n')
        if kind == 'nl':
            at_line_start = True
            continue
        if kind in ('ws', 'comment'):
            if kind == 'comment' and '\n' in val:
                pass
            continue
        if any(f[2] for f in cond):
            continue                          # inside #if 0: dead code
        if val == '{':
            name = declarator_name(toks)
            if name and 'code' in stack:
                found.append((name[1], name[0]))
            stack.append(brace_kind(toks))
        elif val == '}':
            if stack:
                stack.pop()
        toks.append((kind, val, line_here))
        if len(toks) > 4000:
            del toks[:2000]
    return found


SIMPLE = re.compile(r'^\s*(!?)\s*(?:defined\s*\(?\s*([A-Za-z_]\w*)\s*\)?|([A-Za-z_]\w*))\s*(?:/\*.*\*/|//.*)?$')


def branch_taken(d, arg, assumed):
    """Is this branch taken in the one configuration being followed?

    Each macro is assumed defined and true the first time it is tested, and
    that value is reused for every later test, so `#ifdef X` and a later
    `#ifndef X` agree. Following the first branch of each conditional
    independently does not describe any real configuration: on GNU Mach's
    pmap.c, `#ifdef __x86_64__` and a later `#ifndef __x86_64__` leave the
    brace stack one level off, and every later function then reads as nested.
    Expressions this can't evaluate fall back to "taken" (the first branch)."""
    if d in ('ifdef', 'ifndef'):
        name = arg.split()[0] if arg.split() else ''
        v = assumed.setdefault(name, True)
        return v if d == 'ifdef' else not v
    m = SIMPLE.match(arg)
    if not m:
        return True
    neg, name = m.group(1), m.group(2) or m.group(3)
    if name.isdigit():
        v = name != '0'
    else:
        v = assumed.setdefault(name, True)
    return (not v) if neg else v


def brace_kind(toks):
    """Classify the '{' about to be pushed, from the tokens before it.

    'linkage'   extern "C" { / extern "C++" {  (C++ in headers)
    'aggregate' struct/union/enum/class/namespace bodies and initializers
    'code'      everything else: function bodies and blocks inside them.
    Only a definition inside 'code' is a nested function; this is what keeps
    C++ member and inline functions in headers from being reported."""
    if not toks:
        return 'code'
    prev = toks[-1]
    if prev[0] == 'str' and len(toks) > 1 and toks[-2][1] == 'extern':
        return 'linkage'
    if prev[1] in ('=', ',', '{', '(', 'return', 'struct', 'union', 'enum', 'class', 'namespace'):
        return 'aggregate'
    if prev[0] == 'id' and len(toks) > 1 and toks[-2][1] in ('struct', 'union', 'enum', 'class', 'namespace'):
        return 'aggregate'
    return 'code'


def declarator_name(toks):
    """If toks ends with `TYPE NAME ( ... ) [attrs]`, return (name, line)."""
    i = len(toks) - 1
    # Skip trailing __attribute__ ((...)) groups.
    while i >= 0 and toks[i][1] == ')':
        j = match_paren(toks, i)
        if j is None:
            return None
        if j >= 2 and toks[j - 1][1] == '(' and toks[j - 2][1] == '__attribute__':
            i = j - 3
            continue
        if j >= 1 and toks[j - 1][1] == '__attribute__':
            i = j - 2
            continue
        # This is the parameter list.
        if j < 2:
            return None
        name_tok, before = toks[j - 1], toks[j - 2]
        if name_tok[0] != 'id' or name_tok[1] in KEYWORDS:
            return None
        if before[1] in NOT_TYPE_END or before[1] in KEYWORDS:
            return None
        if before[0] not in ('id',) and before[1] != '*':
            return None
        return name_tok[1], name_tok[2]
    return None


def match_paren(toks, i):
    level = 0
    while i >= 0:
        v = toks[i][1]
        if v == ')':
            level += 1
        elif v == '(':
            level -= 1
            if level == 0:
                return i
        i -= 1
    return None


if __name__ == '__main__':
    for root in sys.argv[1:]:
        if os.path.isfile(root):
            for ln, name in scan(root):
                print(f'{root}\t{ln}\t{name}')
            continue
        for d, dirs, files in os.walk(root):
            dirs[:] = sorted(x for x in dirs if x not in ('.git', '.pc', 'build-clang', 'build-gcc', 'debian'))
            for fn in sorted(files):
                if fn.endswith(('.c', '.h')):
                    p = os.path.join(d, fn)
                    for ln, name in scan(p):
                        print(f'{p}\t{ln}\t{name}')
