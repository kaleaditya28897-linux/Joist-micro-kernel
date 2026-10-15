#!/usr/bin/env python3
"""qmpstats.py SOCK OUT: read KVM's per-vCPU statistics through QMP
(query-stats, target vcpu) once a second until QEMU goes; one JSON line a
read into OUT, with the host's monotonic time."""
import json, socket, sys, time

sock_path, out_path = sys.argv[1], sys.argv[2]
s = None
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(0.05)
else:
    sys.exit(1)
f = s.makefile("rw")


def cmd(c, args=None):
    m = {"execute": c}
    if args:
        m["arguments"] = args
    f.write(json.dumps(m) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise EOFError
        r = json.loads(line)
        if "return" in r or "error" in r:
            return r


f.readline()  # greeting
cmd("qmp_capabilities")
with open(out_path, "w") as out:
    try:
        while True:
            r = cmd("query-stats", {"target": "vcpu"})
            out.write(json.dumps({"t": time.monotonic(), "r": r}) + "\n")
            out.flush()
            time.sleep(1)
    except (EOFError, OSError, ValueError):
        pass
