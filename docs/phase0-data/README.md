# Phase 0 data

Derived data behind `docs/mach-inventory.md`, `docs/rpc-idempotency.md`,
`docs/client-recovery.md` §8 and decision 0002's clang spike. Every number in
those documents is in one of these files. Every file here states where it came
from, so any number can be re-derived and checked.

Raw inputs are too large to commit and stay on the testbed VM
(`docs/testbed.md`). Their SHA-256 sums are recorded below, so a re-run can
prove it used the same inputs.

## Environment

All collection was done on 2026-09-23, on the `joist-hurd` VM:

| | |
|---|---|
| Kernel | GNU Mach 1.8+git20260224-up-486 (i386) |
| Hurd, running | 1:0.9.git20251029-7, the version during the traces. It was upgraded to 1:0.9.git20260919-1 later that day by `apt-get build-dep`, and is active from the next reboot |
| libc | libc0.3 2.43-6 |
| rpctrace | from the running hurd package |
| clang | 1:21.1.8-13 (Debian clang-21) |
| mig | 1.8+git20231217-11 |
| Sources | Debian `hurd_0.9.git20260919-1`, `glibc_2.43-6` (`apt-get source`) |
| msgids | `/usr/share/msgids/gnumach.msgids`, sha256 `328f2cb5b607b6944b965ac45ef3c6273e4dd37782bfb540e64dd51ec110d512` |

## Stage 1: rpctrace workloads (dynamic side)

**Command:** `tools/phase0/workloads.sh`, run as `demo` in `~/trace` on the VM.
The coverage limits are in `docs/mach-inventory.md` *Coverage*; read them first.

| Raw trace (on VM, `~demo/trace/`) | Bytes | Lines | SHA-256 |
|---|---:|---:|---|
| `boot.txt` (ext2fs translator start-up proxy) | 67 141 | | `b0348f871ffa76150bd4c9d2e5529a7b4905500c79a07b943f5a0c3a394a6aae` |
| `shell.txt` (10 commands, each traced separately, concatenated) | 570 750 | 8 051 | `bcaa2d9ac3f948b58a7d9276b4066dd1ca1325c1bfeb074b9ac6f8c1d44668fb` |
| `build.txt` (`dpkg-buildpackage -b` of hello 2.12.3, through configure) | 132 588 859 | | `62cb0be1081a9c8deb4865b6d55861a87b3b7d242fc96ae6afedaa78bd08fda8` |
| `shell.cmds` (input to the first shell attempt) | 256 | | `a9bc3a3a9295d40c8f9a62495e99aa7ab0a20cfcab58d112820801d9e15632d8` |
| `shell-bash-i-hung.txt`: **discarded**, see errata | 14 697 | 191 | `b7c31258e07562e1720f19ca7efdc273de6ab2bbcfc6360e5c221c2ee04d21a7` |

Derived files:

| File | Produced by | Contents |
|---|---|---|
| `mach-call-counts.csv` | `tools/phase0/tally.py gnumach.msgids boot.txt shell.txt build.txt` | Every kernel call in `gnumach.msgids` (199), with the count per workload, zeros included |
| `mach-call-counts.md` | same command, stdout | The same data as a table of the observed calls only, plus the list of calls never observed |
| `hurd-rpc-counts.csv` | `tools/phase0/hurdrpc.py boot.txt shell.txt build.txt` | Every Hurd server RPC (everything in `/usr/share/msgids` except `gnumach`) that fired, per workload |
| `io-offsets.csv` | same command | Split of `io_read`/`io_write` by offset argument (`-1` = server file pointer, or explicit) and of `io_seek` by whence |

**How a line is counted:** a trace line counts as a call to `name` if it
matches `->name (`. This includes calls that never returned: a call that hung
(the `gsync_wait` deadlock) is counted once. rpctrace's `*_request` spellings
are merged with the base name, since they are the same msgid.

**Known labelling artefact:** 42 msgids in `gnumach.msgids` are shared by
two names. The one observed call affected is msgid 2200, which rpctrace prints
as `device_pager_init_pager`. The kernel only ever sends it to a pager, so it
is `memory_object_init`. The CSVs keep the name rpctrace printed; the
documents use the real name.

## Stage 2: static cross-reference

**Command:** `tools/phase0/static.py gnumach.msgids hurd-0.9.git20260919 glibc-2.43`
(run in `~/spike` on the VM).

| File | Contents |
|---|---|
| `static-call-sites.csv` | For each of the 196 non-reply kernel calls: the number of components that call it, which ones, and which components implement it server-side (`…_S_name`). A component is a top-level Hurd directory, a glibc top-level directory, or a three-level glibc `sysdeps/` path |

Limits: this is a grep, not a parse. It misses calls through macros or
function pointers, and implementations named outside the `_S_` convention. The
three groups known to be missed (`do_mach_notify_*`, `catch_exception_raise`,
`seqnos_memory_object_*`) were found by a targeted grep and are corrected in
`docs/mach-inventory.md` *Static results*, not in the CSV.

## Stage 2b: traps and OOL parameters

| File | Produced by | Contents |
|---|---|---|
| `traps.csv` | `tools/phase0/traps.py /usr/include/i386-gnu/mach/syscall_sw.h gnumach-1.8+git20260805 hurd-0.9.git20260919 glibc-2.43` | All 47 traps declared in the user header (sha256 `b03bf2c24ec69b0da6cd11295f423fa850495cc7242f42ff04bef0a4848d6acc`): number, argument count, whether GNU Mach's `kern/syscall_sw.c` implements it, and which components reference it by name |
| `ool-params.csv` | `tools/phase0/ool.py hurd-0.9.git20260919 gnumach-1.8+git20260805` | Every parameter of every routine in the Hurd's `hurd/*.defs` whose type MIG may send out-of-line (an unbounded array, directly or via a `type` alias), after `cpp` preprocessing, with its MIG flags (`dealloc`, …) |

Notes:

- The gnumach *source* is 1.8+git20260805, fetched with `apt-get source`.
  The *running* kernel is 1.8+git20260224. The trap table is stable across
  that range, but it is a different build.
- `traps.csv` finds no direct callers for the `syscall_*` traps. They are
  reached through glibc's generated shortcut wrappers (`mach/shortcut.awk`),
  which grep cannot see. `docs/mach-inventory.md` *Traps* explains this.
- `ool.py` needs `cpp`: without preprocessing, the `RPT`/`SCP` macros hide
  parameters. The first, unpreprocessed run found 129 parameters and missed
  `io_read`, `io_write` and `dir_readdir` entirely.

## Stage 3: client-recovery questions

Not a separate file; the evidence is small enough to state in
`docs/client-recovery.md` §8 directly. The generation-counter test is
reproducible with:

```sh
for i in 1 2 3; do rm -f genfile; touch genfile; ino=$(stat -c %i genfile)
  rpctrace -o s.txt stat genfile >/dev/null 2>&1
  grep -oE "\{[0-9]+ [0-9]+ [0-9]+ $ino [0-9]+ [0-9]+ " s.txt | head -1; done
# fields: fstype, fsid (2 words), ino (2 words), st_gen
```

Result on 2026-09-23: inode 227629 all three times, with `st_gen` 1790172149,
1790172413 and 1790172515.

## Stage 4: clang spike (decision 0002)

**Command:** `tools/phase0/spike.sh` (in `~/spike` on the VM), then
`tools/phase0/nested.py` over the sources.

| File | Produced by | Contents |
|---|---|---|
| `clang-hurd-errors.txt` | `grep error: hurd-make.log \| sort -u` | Every distinct clang error from the Hurd build (each file is compiled twice, PIC and non-PIC; duplicates removed) |
| `nested-functions.csv` | `nested.py hurd-0.9.git20260919` and `nested.py glibc-2.43/hurd glibc-2.43/sysdeps/mach glibc-2.43/mach` | Every GCC nested function definition found, by tree, file, line and name |
| `nested-capture.csv` | `tools/phase0/capture-build.sh`, then `tools/phase0/capture.py hurd-0.9.git20260919 hurd-0.9.git20260919/build-gcc` | Each Hurd nested function classified from GCC's `-fdump-tree-nested`: captured enclosing variables, whether its address is taken, and the effort class (`hoist` / `add-params` / `callback`). **Pending**; see decision 0002 |
| `clang-glibc-errors.txt` | `grep error: glibc-make.log \| sort -u` | Every distinct clang error from the glibc build, once the build has finished; see decision 0002's amendment for its state |

| Raw log (on VM, `~demo/spike/`) | SHA-256 |
|---|---|
| `hurd-make.log` | `8a69f96aba8cf1a21a059b3a2b3c1d2361a9964bcd77dbcf94e1bf7a27cd2109` |
| `hurd-configure.log` | `5139f6308886df12b0dde1db26199cd75a6117d4aa683b994cf5a4baddf70733` |

**Validation of `nested.py`:** clang flagged nested functions in 8 files before
the Hurd build stopped. For each of those files, the heuristic's count equals
clang's count, and its line numbers match clang's to within one line (the
heuristic reports the header, clang the brace). Across files the heuristic is
unvalidated: it can miss definitions with unusual return types, and it can
false-positive on K&R-style code.

## Errata

- **2026-09-23, before commit `a62c6cb`'s data was superseded:** the first
  version of `docs/mach-inventory.md` took its *shell* column from
  `shell-bash-i-hung.txt`, the 191-line trace of a single `bash -i` that hung
  almost immediately. The document described the per-command session instead.
  (A `pkill -f workloads.sh` killed the SSH command that was about to rename the
  per-command trace, because that command's own text contained `workloads.sh`.)
  Fixed: the current shell column comes from the per-command trace
  `shell.txt` above. The boot and build columns were unaffected.
