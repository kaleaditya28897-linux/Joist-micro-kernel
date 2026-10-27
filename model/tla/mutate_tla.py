#!/usr/bin/env python3
"""mutate_tla.py - prove IpcDelete's properties have teeth (Phase 1 item 8).

Each mutation below breaks one rule of IpcDelete.tla. For each, a copy of
the spec is mutated and TLC runs it with IpcDelete.cfg (the safe
configuration); TLC must report an error. The same idea as model/test/
mutate.sh: a property that no injected bug can violate checks nothing.

usage: mutate_tla.py [TLC_COMMAND]      (default: tlc, on PATH)
Exit status 0 when every mutation is caught.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# (name, text to find (must occur exactly once), replacement)
MUTATIONS = [
    ("server death strands its caller (I10)",
     """    /\\ srv' = "dead"
    /\\ st' = IF Waiting(sReply) THEN [st EXCEPT ![sReply[1]] = "run"] ELSE st""",
     """    /\\ srv' = "dead"
    /\\ st' = st"""),
    ("endpoint death leaves callers asleep (I10)",
     """                        /\\ st' = [c \\in Clients |-> IF st[c] = "sendq" THEN "run" ELSE st[c]]""",
     """                        /\\ st' = st"""),
    ("deleting a bound reply object strands its caller (I10)",
     """                     THEN /\\ st' = IF Waiting(rObj) THEN [st EXCEPT ![rObj[1]] = "run"] ELSE st""",
     """                     THEN /\\ st' = st"""),
    ("reply_save copies instead of moving (I6)",
     """    /\\ rObj' = sReply
    /\\ sReply' = None""",
     """    /\\ rObj' = sReply
    /\\ sReply' = sReply"""),
    ("Recv allowed while holding a reply capability (I6)",
     """    /\\ srv = "run" /\\ sReply = None /\\ epAlive""",
     """    /\\ srv = "run" /\\ epAlive"""),
    ("a timed-out caller stays queued (I10)",
     """    /\\ q' = Dequeue(c)""",
     """    /\\ q' = q"""),
    ("destruction may stall half-way (I10 liveness: only NoStrandedClient sees it)",
     """Spec == Init /\\ [][Next]_vars /\\ WF_vars(DestroyThread) /\\ WF_vars(SweepStep)""",
     """Spec == Init /\\ [][Next]_vars /\\ WF_vars(DestroyThread)"""),
    ("a destroyed space accepts capabilities (I1)",
     """    /\\ phase # "done"
    /\\ (RefuseIntoDying => phase = "alive")
    /\\ \\E i \\in 1..NSlots :""",
     """    /\\ (RefuseIntoDying => phase \\in {"alive", "done"})
    /\\ \\E i \\in 1..NSlots :"""),
    ("cap_copy_batch into a dying space (I1, 0022)",
     """    /\\ (RefuseIntoDying => phase = "alive")
    /\\ \\E S \\in SUBSET""",
     """    /\\ \\E S \\in SUBSET"""),
    ("a dying space accepts mappings (I12, 0020)",
     """    /\\ (RefuseIntoDying => phase = "alive")
    /\\ \\E p \\in (1..NPages) \\ mapped""",
     """    /\\ \\E p \\in (1..NPages) \\ mapped"""),
    ("the table sweep passes a table without unmapping it (I12)",
     """         ELSE /\\ mapped' = mapped \\ {tsweep}""",
     """         ELSE /\\ mapped' = mapped"""),
    ("the table sweep may stall (I10 liveness)",
     """ /\\ WF_vars(TableStep)""",
     """"""),
]


def main():
    tlc = sys.argv[1] if len(sys.argv) > 1 else "tlc"
    spec = open(os.path.join(HERE, "IpcDelete.tla")).read()
    failed = 0
    for name, find, repl in MUTATIONS:
        if spec.count(find) != 1:
            print(f"STALE    {name}: text found {spec.count(find)} times")
            failed = 1
            continue
        work = tempfile.mkdtemp(prefix="tla-mutant-")
        try:
            open(os.path.join(work, "IpcDelete.tla"), "w").write(spec.replace(find, repl))
            shutil.copy(os.path.join(HERE, "IpcDelete.cfg"), work)
            out = subprocess.run([tlc, "-workers", "4", "-metadir", os.path.join(work, "meta"),
                                  "-config", "IpcDelete.cfg", "IpcDelete.tla"],
                                 cwd=work, capture_output=True, text=True).stdout
        finally:
            shutil.rmtree(work, ignore_errors=True)
        errs = re.findall(r"^Error: (.*)$", out, re.M)
        if "No error has been found" in out or not errs:
            print(f"SURVIVED {name}")
            failed = 1
        else:
            print(f"caught   {name}: {errs[0]}")
    return failed


if __name__ == "__main__":
    sys.exit(main())
