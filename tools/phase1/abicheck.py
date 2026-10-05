#!/usr/bin/env python3
"""abicheck.py - is every result code docs/abi.md lists for an operation
actually produced by the model under test? (Phase 1 work item 7.)

For each operation row of docs/abi.md's tables, takes the result codes named
in it (E_*, BLOCKED; OK always), and looks for each one in two places:

  unit      a unit test asserting  joist_<fn>(...) == JOIST_<code>
            (model/test/test_*.c, excluding the property tests)
  property  a property test's coverage line for the operation, in a
            `make check` log, counting <code>=N with N > 0

It also runs the reverse check: a code a property test reached for an
operation that the operation's row doesn't list is written with listed = 0.

Output CSV: operation,code,listed,unit,property,reached
usage: abicheck.py ABI_MD TEST_DIR MAKE_CHECK_LOG
"""
import csv
import glob
import re
import sys

# ABI operation -> (model function, coverage-line names in make check)
OPS = {
    "obj_create": ("obj_create", ["obj_create", "create"]),
    "cap_derive": ("cap_derive", ["derive"]),
    "cap_mint": ("cap_mint", ["mint"]),
    "cap_delete": ("cap_delete", ["delete", "cap_delete"]),
    "cap_read": ("cap_read", []),
    "boot": ("boot", []),
    "task_create": ("task_create", ["task_create"]),
    "task_destroy": ("task_destroy", ["task_destroy"]),
    "task_reap": ("task_reap", ["reap"]),
    "task_descendant": ("task_descendant", ["descendant"]),
    "thread_spawn": ("thread_spawn", ["spawn"]),
    "thread_kill": ("thread_kill", ["kill"]),
    "Call": ("call", ["call"]),
    "Recv": ("recv", ["recv"]),
    "Reply": ("reply", ["reply"]),
    "ReplyRecv": ("reply_recv", ["reply_recv"]),
    "reply_save": ("reply_save", ["reply_save"]),
    "reply_saved": ("reply_saved", ["reply_saved"]),
    "Signal": ("signal", ["signal"]),
    "Wait": ("wait", ["wait"]),
    "bind_nosenders": ("bind_nosenders", ["bind"]),
    "nosenders_next": ("nosenders_next", ["collect"]),
    "request_deadname": ("request_deadname", ["deadname"]),
}


def main():
    abi_md, test_dir, check_log = sys.argv[1:4]
    rows = {}
    for line in open(abi_md):
        m = re.match(r"\| `?([A-Za-z_]+)`? \|", line)
        if m and m.group(1) in OPS:
            cells = line.split("|")
            # The results column is the third cell (op | args | results | ...).
            # Codes the row explicitly rules out ("Never `E_X`") don't count.
            results = re.sub(r"Never `E_[A-Z]+`[^|]*", "", cells[3])
            codes = set(re.findall(r"\bE_[A-Z]+\b", results))
            if "Blocks?" not in line and re.search(r"\bBLOCKED\b", line):
                codes.add("BLOCKED")
            # OK is implied, except where the row says the operation always
            # blocks (Call).
            if "always: `BLOCKED`" not in line:
                codes.add("OK")
            rows[m.group(1)] = codes
    unit = set()
    for f in glob.glob(test_dir + "/test_*.c"):
        if "props" in f:
            continue
        text = open(f).read()
        for fn, code in re.findall(r"joist_(\w+)\((?:[^;()]|\([^;()]*\))*\)\s*==\s*JOIST_(\w+)",
                                   text):
            unit.add((fn, code))
    prop = set()
    for line in open(check_log):
        m = re.match(r"\s+coverage (\S+)\s+(.*)", line)
        if m:
            # Before the "[max work ...]" suffix of the whole-model test.
            for code, n in re.findall(r"(\w+)=(\d+)", m.group(2).split("[")[0]):
                if int(n) > 0:
                    prop.add((m.group(1), code))
    out = csv.writer(sys.stdout)
    out.writerow(["operation", "code", "listed", "unit", "property", "reached"])
    for op, codes in rows.items():
        fn, covnames = OPS[op]
        seen = {code for c in covnames for (name, code) in prop if name == c}
        for code in sorted(codes | seen):
            u = (fn, code) in unit
            p = code in seen
            out.writerow([op, code, int(code in codes), int(u), int(p), int(u or p)])


if __name__ == "__main__":
    main()
