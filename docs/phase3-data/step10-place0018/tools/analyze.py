#!/usr/bin/env python3
# analyze.py RAWDIR : per-boot table of the place0018 campaigns (raw/*.log).
# Per boot: variant, contended p50/p99/worst, ruler range, placement (client
# CPU, root's taker after the server's bind, whether the poked CPU won),
# round p50s, host frequency / temperature during the boot, host IRQs on
# CPUs 2-5, and the QEMU threads' last host CPUs. Writes raw/boots.tsv.
import sys, re, os, glob, statistics as st, json
raw = sys.argv[1]
LINES = {
 "a system call that does nothing": "null",
 "two address spaces, passive server": "2as",
 "one address space, passive server": "1as",
 "two address spaces, server with its own context": "own",
 "two address spaces, passive server, 4 KiB of IPC buffer each way": "4k",
 "two address spaces, passive server, every other CPU making system calls": "cont",
}
def parse(path):
    b = {}
    m = re.match(r"(.+)@(\d+)-(\d+)\.log$", os.path.basename(path))
    b["img"], b["order"], b["n"] = m.group(1), m.group(2), int(m.group(3))
    txt = open(path, "rb").read().decode("latin-1")
    b["when"] = re.search(r"campaign: (\S+)", txt).group(1)
    for line in txt.splitlines():
        mm = re.match(r"ipcbench: (.*?): n=\S+ min=(\d+) p50=(\d+) p99=(\d+) worst-p50=(\d+) max=(\d+) cycles .*?12000 cycles took (\d+) to (\d+) counts", line)
        if mm and mm.group(1) in LINES:
            k = LINES[mm.group(1)]
            if k == "cont":
                b.setdefault("cont_all", []).append(int(mm.group(3)))
            b[k] = dict(min=int(mm.group(2)), p50=int(mm.group(3)), p99=int(mm.group(4)), worst=int(mm.group(5)),
                        rlo=int(mm.group(7)), rhi=int(mm.group(8)))
        if line.startswith("ipcbench-rounds: p50"):
            b["rounds"] = [int(x) for x in line.split()[2:]]
        mm = re.match(r"lockdepth: caller cpu (\d+)", line)
        if mm:
            b["client"] = int(mm.group(1))
            b.setdefault("clients", []).append(int(mm.group(1)))
        mm = re.match(r"pin-1s: (.*): (\S+) last (-?\d+)", line)
        if mm: b.setdefault("pins", {})[mm.group(1)] = (mm.group(2), int(mm.group(3)))
    # RUN_LOCK_DEPTH counts per round and CPU (ticket mod 4 in order builds)
    segs = {}
    for sg, cpu, n, q in re.findall(r"lockdepth: seg (\d+) cpu (\d+) n (\d+) depth (\d+ \d+ \d+ \d+)", txt):
        segs.setdefault(int(sg), {})[int(cpu)] = (int(n), [int(x) for x in q.split()])
    b["is_order"] = "RUN_LOCK_ORDER" in txt
    if b["is_order"] and segs and "client" not in b:
        pass
    if b["is_order"]:
        cyc, pur = [], []
        for sg in sorted(segs):
            d = segs[sg]
            res = {c: max(range(4), key=lambda k: d[c][1][k]) for c in d if d[c][0]}
            share = min(d[c][1][res[c]] / d[c][0] for c in res)
            # the CPUs in the order they take the lock after the client's CPU
            cl = int(re.search(r"lockdepth: caller cpu (\d+)", txt).group(1))
            after = sorted((c for c in res if c != cl), key=lambda c: (res[c] - res[cl]) % 4)
            ok = sorted((res[c] - res[cl]) % 4 for c in res) == [0, 1, 2, 3]
            cyc.append(("".join(str(c) for c in after)) if ok else "?")
            pur.append(share)
        b["cycle"] = cyc
        b["purity"] = pur
    # order builds: each CPU's last ticket at each round's start (and the end):
    # the client holds the lock then, the others are queued in ticket order
    snaps = re.findall(r"lockdepth: snap (\d+) (\d+) (\d+) (\d+) (\d+)", txt)
    if snaps:
        cl = int(re.search(r"lockdepth: caller cpu (\d+)", txt).group(1))
        seq = []
        for sn in snaps:
            t = [int(x) for x in sn[1:]]
            rel = sorted(((t[c] - t[cl]) & 0xffffffff, c) for c in range(4) if c != cl)
            seq.append("".join(str(c) for _, c in rel) if [d for d, _ in rel] == [1, 2, 3] else "?")
        b["snapseq"] = seq
    # pred builds: per round and CPU, the previous holder's CPU
    if "RUN_LOCK_PRED" in txt:
        b["pred"] = {sg: {c: d[c][1] for c in d} for sg, d in segs.items()}
    # placement events
    ev = re.findall(r"placement: \d+ (\w+) cpu (\d+) tag (\d+) a (\d+)", txt)
    ev = [(k, int(c), int(t), int(a)) for k, c, t, a in ev]
    if ev:
        b["ev"] = ev
        now = dict((int(c), int(t)) for c, t in re.findall(r"placement: now cpu (\d+) runs (\d+)", txt))
        b["now"] = now
        # the root's thread: the caller of mark 0 (op 5)
        root = [a for k, c, t, a in ev if k == "mark" and t == 0][0]
        i30 = [i for i, e in enumerate(ev) if e[0] == "mark" and e[2] == 30][0]
        # the poke right after mark 30 and who took the root next
        pk = next(e for e in ev[i30:] if e[0] == "poke" and e[3] != 0)
        took = next(e for e in ev[i30:] if e[0] == "enter" and e[3] == root)
        b["root_poked"] = [i for i in range(4) if pk[3] >> i & 1]
        b["root_taker"] = took[1]
        # inflight at the taker's choice: the last poke entry by that cpu before 'took'
        it = ev.index(took)
        pre = [e for e in ev[i30:it] if e[0] == "poke" and e[1] == took[1]]
        b["taker_inflight"] = pre[-1][2] if pre else None
        busy_ids = [a for k, c, t, a in ev if k == "enter" and t == 0 and a not in (root, 32)]
        b["busy_cpus"] = sorted(c for c, t in now.items() if c != b.get("client"))
        b["busy_spawn_cpus"] = [next(e[1] for e in ev if e[0] == "enter" and e[3] == x) for x in sorted(set(busy_ids))[:3]]
        b["sig"] = " ".join(f"{k[0]}{c}:{a if k != 'poke' else hex(a)}" for k, c, t, a in ev if k in ("enter", "poke") and not (k == "poke" and a == 0))
    if "client" in b:
        b["client_host"] = int(b["order"][b["client"]])
    # host side
    fq = path[:-4] + ".freq"
    if os.path.exists(fq):
        rows = [l.split() for l in open(fq) if l.split() and l.split()[1] == "1"]
        if rows:
            # the contended measurement (~0.2 s) is the boot's end: the last sample
            b["freq"] = st.mean(st.mean(int(x) for x in r[2:6]) for r in rows[-1:]) / 1e3
            b["tpkg"] = st.mean(int(r[6]) for r in rows) / 1e3
            b["tmax"] = max(int(r[6]) for r in rows) / 1e3
            b["t0"] = int(rows[0][6]) / 1e3
    ib, ia = path[:-4] + ".irq-before", path[:-4] + ".irq-after"
    if os.path.exists(ib):
        def cnt(p):
            d = {}
            for l in open(p).read().splitlines()[1:]:
                f = l.split()
                if not f[0].endswith(":"): continue
                v = [int(x) for x in f[1:13] if x.isdigit()][:12]
                if len(v) == 12: d[f[0]] = v
            return d
        x, y = cnt(ib), cnt(ia)
        b["irq25"] = sum(y[k][c] - x.get(k, [0]*12)[c] for k in y for c in range(2, 6))
    return b
boots = [parse(p) for p in sorted(glob.glob(os.path.join(raw, "*@*-*.log")))]
boots.sort(key=lambda b: b["when"])
json.dump(boots, open(os.path.join(raw, "boots.json"), "w"))
cols = ["img", "order", "n", "when", "cyc", "cont.p50", "cont.p99", "cont.worst", "cont.rlo", "cont.rhi", "client", "client_host",
        "busy_cpus", "root_poked", "root_taker", "taker_inflight", "freq", "tpkg", "t0", "irq25", "2as.p50", "own.p50"]
def g(b, c):
    if c == "cyc":
        return " ".join(b.get("cycle", [])[:1]) + ("" if len(set(b.get("cycle", []))) <= 1 else "+")
    if "." in c:
        a, k = c.split(".")
        return b.get(a, {}).get(k, "")
    v = b.get(c, "")
    if isinstance(v, float): return f"{v:.1f}"
    return v
with open(os.path.join(raw, "boots.tsv"), "w") as f:
    f.write("\t".join(cols) + "\n")
    for b in boots:
        f.write("\t".join(str(g(b, c)) for c in cols) + "\n")
print(len(boots), "boots")
