#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# glibc-tests-spec.py - how glibc's own Makefiles run each test, for
# glibc-tests-run.sh (Phase 5 step 7, the design's item 11 d; the owner's
# choice, 2026-11-08: a test's arguments, environment and stdin, as its
# Makefile gives them).
#
# usage: glibc-tests-spec.py RECIPES BUILD SRC OUT
#   RECIPES: the output of `make -n -k check run-built-tests=yes` in BUILD
#   (glibc's build directory); SRC: glibc's source tree; OUT: a directory,
#   given for each test whose recipe is found OUT/SUBDIR/TEST.args (module
#   2 of user/glibcroot, without argv[0]: "a ARG" and "e NAME=VALUE" lines),
#   and if its Makefile gives them OUT/SUBDIR/TEST.stdin (its input) and
#   OUT/SUBDIR/TEST.expect (the output its "-cmp" test compares with), and
#   OUT/spec.txt, a line a test: what was found.
#
# A recipe is the line glibc's Rules make for a test: env NAME=VALUE...
# BUILD/elf/ld.so --library-path PATH BUILD/SUBDIR/TEST ARGS... > OUT
# [< INPUT]. Tests run through a script of their own (tst-printf.sh, ...)
# aren't found: they run with no arguments. The environment drops what
# only a dynamic program uses (LD_*); paths in it name the build machine,
# which a test on Joist can't open anyway (no filesystem yet).
import os
import shlex
import sys

recipes, build, src, out = sys.argv[1:5]
build = os.path.realpath(build)
src = os.path.realpath(src)
DROP = ("LD_PRELOAD=", "LD_LIBRARY_PATH=", "LD_AUDIT=", "LD_BIND_NOW=")
STOP = {";", "&&", "||", "|", "&", ">", ">>", "<", "2>&1", "2>", ")", "("}


def lines(path):
    """The recipes, continuation lines joined."""
    cur = ""
    with open(path, errors="replace") as f:
        for raw in f:
            raw = raw.rstrip("\n")
            if raw.endswith("\\"):
                cur += raw[:-1] + " "
                continue
            yield cur + raw
            cur = ""
    if cur:
        yield cur


def tokens(line):
    lx = shlex.shlex(line, posix=True, punctuation_chars=True)
    lx.whitespace_split = True
    try:
        return list(lx)
    except ValueError:
        return []


def test_of(path):
    """SUBDIR/TEST for a program in the build tree, or None."""
    p = os.path.realpath(path) if os.path.isabs(path) else None
    if p is None or not p.startswith(build + "/"):
        return None
    rel = p[len(build) + 1:]
    return rel if rel.count("/") == 1 else None


specs = {}
expects = {}
for line in lines(recipes):
    if "ld-x86-64.so" in line and "--library-path" in line:
        t = tokens(line)
        for i, tok in enumerate(t):
            if tok.endswith("/ld-x86-64.so.1") and i + 3 < len(t) and t[i + 1] == "--library-path":
                test = test_of(t[i + 3])
                if test is None:
                    break
                env = [e for e in t[:i] if "=" in e and not e.startswith(DROP)]
                args, j, stdin = [], i + 4, None
                while j < len(t) and t[j] not in STOP:
                    args.append(t[j])
                    j += 1
                for k in range(j, len(t) - 1):
                    if t[k] == "<":
                        stdin = t[k + 1]
                    if t[k] == ";":
                        break
                if test not in specs:
                    specs[test] = (env, args, stdin)
                break
    elif line.lstrip().startswith(("cmp ", "diff ")):
        t = tokens(line.strip())
        files = [x for x in t[1:] if not x.startswith("-")][:2]
        if len(files) == 2:
            for a, b in (files, files[::-1]):
                if b.endswith(".out") and test_of(b[:-4]) is not None and not a.startswith(build):
                    expects[test_of(b[:-4])] = a

os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "spec.txt"), "w") as rep:
    for test in sorted(specs):
        env, args, stdin = specs[test]
        sub = test.split("/")[0]
        d = os.path.join(out, sub)
        os.makedirs(d, exist_ok=True)
        base = os.path.join(out, test)
        notes = []
        bad = [x for x in env + args if "\n" in x]
        if bad:
            rep.write(f"{test} SKIPPED: a newline in an argument\n")
            continue
        with open(base + ".args", "w") as f:
            for a in args:
                f.write(f"a {a}\n")
            for e in env:
                f.write(f"e {e}\n")
        notes.append(f"{len(args)} args, {len(env)} env")
        for what, name in (("stdin", stdin), ("expect", expects.get(test))):
            if name is None:
                continue
            p = name if os.path.isabs(name) else os.path.join(src, sub, name)
            if os.path.isfile(p):
                with open(p, "rb") as f, open(f"{base}.{what}", "wb") as g:
                    g.write(f.read())
                notes.append(f"{what} {os.path.relpath(p, src) if p.startswith(src) else p}")
            else:
                notes.append(f"{what} {name} MISSING")
        rep.write(f"{test} " + "; ".join(notes) + "\n")
print(f"{len(specs)} recipes, {len(expects)} with an expected output: {out}/spec.txt")
