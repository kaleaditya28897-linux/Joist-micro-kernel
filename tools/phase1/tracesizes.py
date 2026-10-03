#!/usr/bin/env python3
"""tracesizes.py - sizes of the variable parts of hot Hurd RPCs, from
rpctrace logs (Phase 1 work item 6).

rpctrace prints strings and data truncated to 16 characters (escapes such as
\\x7f or \\0 count as one byte), so a string is measured exactly when it is
shorter than 16 bytes and is otherwise counted as ">=16". Numbers are exact.

Measured, per log:
  io_read   requested amount     (2nd argument; an upper bound on the reply)
  io_write  bytes written        (the reply's amount)
  dir_lookup file_name length    (request), retry_name length (reply)

Output CSV on stdout:
  log,rpc,measure,bucket,calls
where bucket is the upper bound of a power-of-two size class in bytes
(8, 16, 32, ... ) or ">=16" for truncated strings.

usage: tracesizes.py LOG...
"""
import collections
import os
import re
import sys

BUCKETS = [8 << i for i in range(20)]

STR = r'"((?:[^"\\]|\\.)*)"'


def strlen(s):
    """Bytes in an rpctrace-printed string, or None if it was truncated."""
    n = len(re.findall(r"\\x[0-9a-fA-F]{2}|\\[0-7]{1,3}|\\.|.", s))
    return None if n >= 16 else n + 1  # + the terminating NUL MIG sends


def bucket(n):
    for b in BUCKETS:
        if n <= b:
            return str(b)
    return ">" + str(BUCKETS[-1])


def main():
    rows = collections.Counter()
    pats = {
        "io_read": re.compile(r"->io_read(?:_request)? \((-?\d+) (\d+)\)"),
        "io_write": re.compile(r"->io_write(?:_request)? \(" + STR + r" -?\d+\) = 0 (\d+)"),
        "dir_lookup": re.compile(r"->dir_lookup(?:_request)? \(" + STR + r" -?\d+ -?\d+\)"
                                 r"(?: = 0 -?\d+ " + STR + r")?"),
    }
    for path in sys.argv[1:]:
        log = os.path.basename(path)
        with open(path, errors="replace") as f:
            for line in f:
                if "->io_read" in line:
                    m = pats["io_read"].search(line)
                    if m:
                        rows[(log, "io_read", "requested", bucket(int(m.group(2))))] += 1
                elif "->io_write" in line:
                    m = pats["io_write"].search(line)
                    if m:
                        rows[(log, "io_write", "written", bucket(int(m.group(2))))] += 1
                elif "->dir_lookup" in line:
                    m = pats["dir_lookup"].search(line)
                    if m:
                        n = strlen(m.group(1))
                        rows[(log, "dir_lookup", "file_name",
                              ">=16" if n is None else bucket(n))] += 1
                        if m.group(2) is not None:
                            n = strlen(m.group(2))
                            rows[(log, "dir_lookup", "retry_name",
                                  ">=16" if n is None else bucket(n))] += 1
    print("log,rpc,measure,bucket,calls")
    for (log, rpc, measure, b), n in sorted(rows.items()):
        print(f"{log},{rpc},{measure},{b},{n}")


if __name__ == "__main__":
    main()
