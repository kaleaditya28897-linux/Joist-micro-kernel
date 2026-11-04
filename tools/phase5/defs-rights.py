#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""defs-rights.py - Phase 5 step 6: the port rights each MIG routine passes,
in its request and in its reply, for decision 0001's two capabilities a
message (plan choice 3: "the count ... to be checked before the backend is
written").

A .defs file is preprocessed (with the Hurd's and Mach's include paths), its
`type` declarations are followed to find which types are port rights (one
right) or arrays of them (any number), and each routine's arguments are
counted by direction. The request port (the endpoint called: the argument
marked requestport, else the first) and the reply ports (the reply
capability) aren't counted: they travel as the call itself.

usage: defs-rights.py OUT.csv CPP_COMMAND... -- DEFS...
"""
import csv
import re
import subprocess
import sys

out = sys.argv[1]
sep = sys.argv.index("--")
cpp = sys.argv[2:sep]
files = sys.argv[sep + 1:]

RIGHT = re.compile(r"MACH_MSG_TYPE_(COPY|MAKE|MOVE)_(SEND|SEND_ONCE|RECEIVE)|MACH_MSG_TYPE_PORT_(SEND|SEND_ONCE|RECEIVE)|polymorphic")
SKIP_DIR = {"requestport", "replyport", "ureplyport", "sreplyport", "waittime", "msgoption", "msgseqno",
            "sectoken", "usectoken", "ssectoken", "audittoken", "uaudittoken", "saudittoken"}

rows = []
for f in files:
    text = subprocess.run(cpp + [f], capture_output=True, text=True).stdout
    text = re.sub(r"^#.*$", "", text, flags=re.M)
    kinds = {}  # type name -> "port" | "ports" (array) | None
    for name, rhs in re.findall(r"\btype\s+(\w+)\s*=\s*([^;]*);", text):
        rhs0 = rhs.split("ctype")[0].split("intran")[0]
        k = None
        if RIGHT.search(rhs0):
            k = "port"
        m = re.match(r"\s*(array\s*\[[^\]]*\]\s*of\s*)?(\w+)", rhs0)
        if m and m.group(2) in kinds and kinds[m.group(2)]:
            k = "ports" if m.group(1) or kinds[m.group(2)] == "ports" else "port"
        elif m and m.group(1) and k == "port":
            k = "ports"
        kinds[name] = k
    sub = re.search(r"\bsubsystem\s+(?:\w+\s+)?(\w+)\s+(\d+)", text)
    subsys = sub.group(1) if sub else f
    for kw, name, args in re.findall(r"\b(routine|simpleroutine)\s+(\w+)\s*\((.*?)\)\s*;", text, flags=re.S):
        req = rep = 0
        req_arrays = rep_arrays = 0
        parts = [x.strip() for x in args.split(";") if x.strip()]
        if parts and not any(re.match(r"requestport\b", x) for x in parts):
            parts = parts[1:]  # MIG's first argument is the request port
        for a in parts:
            m = re.match(r"(?:(in|out|inout|requestport|replyport|ureplyport|sreplyport|waittime|msgoption|msgseqno|\w*token)\s+)?(\w+)\s*:\s*(\w+)?", a)
            if not m:
                continue
            d, _, t = m.groups()
            d = d or "in"
            if d in SKIP_DIR or t is None:
                continue
            inline = None
            mm = re.search(r":\s*(array\s*\[[^\]]*\]\s*of\s*\w+|\w+)", a)
            if mm and mm.group(1).startswith("array"):
                inline = "ports" if RIGHT.search(a) or kinds.get(mm.group(1).split()[-1]) else None
            k = inline or kinds.get(t) or ("port" if RIGHT.search(a) else None)
            if k is None:
                continue
            if d in ("in", "inout"):
                req += k == "port"
                req_arrays += k == "ports"
            if d in ("out", "inout"):
                rep += k == "port"
                rep_arrays += k == "ports"
        rows.append([subsys, name, kw, req, req_arrays, rep, rep_arrays])

with open(out, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["subsystem", "routine", "kind", "request rights", "request arrays of rights",
                "reply rights", "reply arrays of rights"])
    w.writerows(rows)

over = [r for r in rows if r[3] > 2 or r[5] > 2 or r[4] or r[6]]
print(f"routines: {len(rows)}; more than 2 rights, or an array of them, either way: {len(over)}")
for r in over:
    print("  ", r[0], r[1], "request", r[3], "+arrays" * bool(r[4]), "reply", r[5], "+arrays" * bool(r[6]))
