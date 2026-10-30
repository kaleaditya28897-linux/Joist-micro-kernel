#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""regcover.py - how many message registers the hot Hurd RPCs need
(Phase 1 work item 6; decision 0001 open item 1).

Inputs (docs/phase1-data/item6/ and docs/phase0-data/amd64/):
  mig-sizes.csv        per-field payload of every routine (migsizes.py)
  trace-sizes.csv      measured sizes of the variable parts (tracesizes.py)
  hurd-rpc-counts.csv  calls per routine per workload (Phase 0)

A message's register payload is its fixed part (inline fields and the
return code, each rounded up to whole 8-byte words) plus its variable part.
Ports travel as capabilities, not payload, and are counted separately. The
operation label (MIG's msgid) is not counted: like seL4's message-info word
it travels in a register of its own. A call fits in M registers when both its
request and its reply payload fit in M words.

Variable parts are sized conservatively: each measured call at the upper
bound of its power-of-two bucket; a truncated string (">=16") as fitting only
if M words could hold MIG's 1024-byte maximum; an io_read reply as the amount
requested; variable parts no trace measures as not fitting.

Output: a CSV of M against the share of calls that fit, per workload.
usage: regcover.py MIG_SIZES TRACE_SIZES COUNTS
"""
import collections
import csv
import sys

WORD = 8
MS = [2, 4, 6, 8, 10, 12, 16, 24, 32, 64, 128, 256, 512, 1024]
# The variable part of each measured routine and direction, as named in
# trace-sizes.csv (rpc, measure).
MEASURED = {
    ("io_read", "reply", "data"): ("io_read", "requested"),
    ("io_write", "request", "data"): ("io_write", "written"),
    ("dir_lookup", "request", "file_name"): ("dir_lookup", "file_name"),
    ("dir_lookup", "reply", "retry_name"): ("dir_lookup", "retry_name"),
}
LOGS = {"boot": "boot.txt", "shell": "shell.txt", "build": "build.txt"}


def words(n):
    return (n + WORD - 1) // WORD


def main():
    mig_csv, trace_csv, counts_csv = sys.argv[1:4]
    fixed = collections.defaultdict(int)   # (routine, dir) -> words
    var = collections.defaultdict(list)    # (routine, dir) -> [field]
    ports = collections.defaultdict(int)
    for r in csv.DictReader(open(mig_csv)):
        key = (r["routine"], r["direction"])
        fixed.setdefault(key, 0)
        if r["class"] in ("inline", "retcode"):
            fixed[key] += words(int(r["bytes"]))
        elif r["class"] == "var":
            var[key].append(r["field"])
        elif r["class"] in ("port", "portvar"):
            ports[key] += 1
    # dist[(log, rpc, measure)] = [(bytes or None, calls)]
    dist = collections.defaultdict(list)
    for r in csv.DictReader(open(trace_csv)):
        b = None if r["bucket"] == ">=16" else int(r["bucket"].lstrip(">"))
        dist[(r["log"], r["rpc"], r["measure"])].append((b, int(r["calls"])))
    counts = list(csv.DictReader(open(counts_csv)))

    def fits_share(routine, direction, log, m):
        """Share (0..1) of this routine's messages in this direction whose
        payload fits in m words, or None when the routine is unknown."""
        key = (routine, direction)
        if key not in fixed:
            return None
        room = m * WORD - fixed[key] * WORD
        if room < 0:
            return 0.0
        fields = var.get(key, [])
        if not fields:
            return 1.0
        if len(fields) > 1:
            return 0.0  # several variable parts, none measured jointly
        meas = MEASURED.get((routine, direction, fields[0]))
        if meas is None:
            return 0.0  # unmeasured variable part: assume it doesn't fit
        d = dist.get((LOGS[log], meas[0], meas[1]), [])
        total = sum(c for _, c in d)
        if total == 0:
            return 0.0
        ok = sum(c for b, c in d if (b is not None and b <= room) or (b is None and room >= 1024))
        return ok / total

    out = csv.writer(sys.stdout)
    out.writerow(["registers", "workload", "calls", "fit_calls", "fit_share", "unknown_routines"])
    for m in MS:
        for log in LOGS:
            calls = fit = 0.0
            unknown = 0
            for r in counts:
                n = int(r[log])
                if n == 0:
                    continue
                q = fits_share(r["rpc"], "request", log, m)
                p = fits_share(r["rpc"], "reply", log, m)
                if q is None or p is None:
                    unknown += 1
                    continue
                calls += n
                # Request and reply sizes are measured independently; only
                # one side of each measured routine is variable, so the
                # product is exact here.
                fit += n * q * p
            out.writerow([m, log, int(calls), round(fit), f"{fit / calls:.3f}", unknown])


if __name__ == "__main__":
    main()
