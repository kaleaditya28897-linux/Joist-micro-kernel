#!/usr/bin/env python3
"""migsizes.py - payload sizes of Hurd RPC messages (Phase 1 work item 6).

For each .defs file given, runs MIG to generate the server stubs, extracts
every routine's Request and Reply struct, and compiles a program (appended to
the generated server file, so every type resolves exactly as in a real
build) that prints sizeof() of every field. Each field is then classified:

  header  mach_msg_header_t, MIG type descriptors, padding: Mach framing,
          which Joist does not send (0001: payload in registers, capabilities
          as capabilities)
  port    mach_port_t and its MIG aliases: a capability transfer
  inline  fixed-size data: integers, structs, fixed arrays
  var     a variable-length array MIG inlines up to a maximum (char data[N]
          unions, string_t); its real size is per call, from the traces
  portvar a variable-length array of ports
  retcode the reply's return code (kept: 4 bytes of payload)

Output CSV on stdout:
  defs,routine,direction,field,ctype,bytes,class

usage: migsizes.py OUTDIR DEFS... (run on the Hurd testbed; needs mig, gcc)
"""
import os
import re
import subprocess
import sys

INC = "/usr/include/x86_64-gnu"


def mig(defs, outdir):
    base = os.path.splitext(os.path.basename(defs))[0]
    server = os.path.join(outdir, base + "Server.c")
    subprocess.run(["mig", "-I" + INC, "-cc", "gcc -E -x c",
                    "-user", "/dev/null", "-server", server,
                    "-header", os.path.join(outdir, base + ".h"),
                    "-sheader", os.path.join(outdir, base + "_S.h"), defs],
                   check=True, cwd=outdir, capture_output=True)
    return base, server


def structs(src):
    """Yield (routine, 'request'|'reply', body) from a MIG server file."""
    for m in re.finditer(r"mig_internal void _X(\w+)\s*\([^)]*\)\s*\{(.*?)\n\}", src, re.S):
        name, body = m.group(1), m.group(2)
        for sm in re.finditer(r"typedef struct \{(.*?)\} (Request|Reply);", body, re.S):
            yield name, sm.group(2).lower(), sm.group(1)


def fields(body):
    """Fields as (ctype, name) from a struct body; a union of an inline
    array and its out-of-line pointer becomes one 'var' field."""
    out = []
    body = re.sub(r"union\s*\{\s*([^;]*?)\s+(\w+)\[(\d+)\];\s*[^;]*;\s*\};",
                  r"VAR \2 \3;", body)
    for line in body.split(";"):
        line = " ".join(line.split())
        if not line:
            continue
        if line.startswith("VAR "):
            _, name, n = line.split()
            out.append(("var", name, int(n)))
            continue
        m = re.match(r"(.*?)\s*\b(\w+)\s*(\[\d+\])?$", line)
        ctype, name, arr = m.group(1), m.group(2), m.group(3) or ""
        out.append((ctype + arr, name, None))
    return out


def main():
    outdir, defs_list = sys.argv[1], sys.argv[2:]
    rows = []
    for defs in defs_list:
        base, server = mig(defs, outdir)
        src = open(server).read()
        items = list(structs(src))
        # Port types: whatever the server file declares as a port in a type
        # descriptor (MACH_MSG_TYPE_*_SEND/RECEIVE/PORT_*): find the field
        # names whose descriptor names a port, by MIG's naming (xType).
        # MIG declares a port as MACH_MSG_TYPE_PORT_*, MOVE_*, COPY_* or
        # MAKE_*, or POLYMORPHIC when the disposition is chosen at run time.
        portish = set(re.findall(r"const mach_msg_type_(?:long_)?t (\w+)Type = \{\s*"
                                 r"\.msgt_(?:header\.msgt_)?name =\s*\(unsigned char\) "
                                 r"(MACH_MSG_TYPE_(?:PORT_(?!NAME)\w+|MOVE_\w+|COPY_\w+|MAKE_\w+|POLYMORPHIC))",
                                 src))
        portnames = {n for n, _ in portish}
        # Request fields are validated instead: In0P->xType.msgt_name (or, for
        # a long descriptor, .msgtl_name) != MACH_MSG_TYPE_PORT_SEND etc.
        portnames |= set(re.findall(r"(\w+)Type\.msgtl?_name != (?:\(unsigned char\) )?"
                                    r"MACH_MSG_TYPE_(?:PORT_(?!NAME)|MOVE_|COPY_|MAKE_|POLYMORPHIC)", src))
        prog = [src, "\n#include <stdio.h>\nint main(void) {\n"]
        meta = []
        for i, (routine, direction, body) in enumerate(items):
            fl = fields(body)
            decl = re.sub(r"union\s*\{", "union {", body)
            prog.append("{ typedef struct {%s} S%d;\n" % (decl, i))
            for ctype, name, n in fl:
                if ctype == "var":
                    prog.append('printf("%%d\\n", (int)%d);\n' % n)
                else:
                    prog.append('printf("%%zu\\n", sizeof(((S%d *)0)->%s));\n' % (i, name))
                meta.append((routine, direction, name, ctype))
            prog.append("}\n")
        prog.append("return 0; }\n")
        cfile = os.path.join(outdir, base + "_sizes.c")
        open(cfile, "w").write("".join(prog))
        exe = os.path.join(outdir, base + "_sizes")
        subprocess.run(["gcc", "-w", "-I" + INC, "-I" + outdir, "-o", exe, cfile],
                       check=True, capture_output=True)
        sizes = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split()
        for (routine, direction, name, ctype), size in zip(meta, sizes):
            if name == "Head" or ctype.startswith("mach_msg_type") or "Pad" in name \
                    or name == "RetCodeType":
                cls = "header"
            elif name == "RetCode":
                cls = "retcode"
            elif name in portnames:
                cls = "port" if ctype != "var" else "portvar"
            elif ctype == "var" or ctype.startswith("string_t"):
                cls = "var"
            else:
                cls = "inline"
            rows.append((base, routine, direction, name, ctype, size, cls))
    print("defs,routine,direction,field,ctype,bytes,class")
    for r in rows:
        print(",".join(str(x) for x in r))


if __name__ == "__main__":
    main()
