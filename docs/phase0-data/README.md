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

## Stage 5: nested-function capture analysis (2026-09-23, after the outage)

Answers the question decision 0002 named as deciding: of the 252 GCC nested
functions, how many capture enclosing locals? One that captures nothing becomes
a `static` function by moving it; one that captures needs closure conversion —
a context struct threaded through the callback API.

**Command:** `tools/phase0/captures.py hurd-0.9.git20260919 glibc-2.43`, run as
`demo` in `~/spike`, joined to `nested-functions.csv` so the population is
exactly the 252 rows `nested.py` found. Output: `nested-captures.csv`.

| Verdict | Count | Meaning |
|---|---:|---|
| `capture` | **209** | References an enclosing parameter or local. Needs closure conversion. |
| `sibling` | 7 | References only sibling nested functions. Hoist together. |
| `none` | 36 | Self-contained. Move and mark `static`. |

Hurd 149 / 4 / 26 of 179; glibc 60 / 3 / 10 of 73.

### Read this before using the number

**209 is a lower bound, and the tool has not converged.** It is a regex
heuristic over stripped source, and every audit round has found more captures
and moved the count up:

| Version | capture | none | What the audit found |
|---|---:|---:|---|
| v1 | 201 | 42 | Missed 2nd+ declarators of a comma list; no notion of sibling nested functions |
| v2 | 176 | 61 | Sibling references split out; declaration scanner skipped initialisers |
| v3 | 185 | 62 | Initialisers fixed |
| v4 | **209** | 36 | Type-prefix regex was greedy — its generic `\w+` alternative swallowed the declarator, so `int summary_status = 0;` yielded no name at all |

Two independent random samples of `none` verdicts were hand-checked against the
source: **2 of 6 wrong in the first, 3 of 7 wrong in the second.** Each failure
was a distinct regex bug, and fixing it raised the capture count. One known
false `none` survives even in v4 (`utils/login.c:432 use_child_init_port`,
which captures the array `ports` declared at line 420); it is corrected in the
CSV with `source=audit`. There is no reason to believe it is the last one.

`source` column: `tool` (v4 output), `hand` (9 rows whose definition header the
regex cannot match, each read and classified directly), `audit` (tool was
wrong, corrected after reading the source).

**Conclusion: do not treat 209 as precise.** The defensible statements are that
**at least 83% of these nested functions capture**, and that the true figure is
higher than whatever the current regex reports.

### The compiler cross-check

GCC emits `-Wtrampolines` exactly when it generates a trampoline, which happens
when a nested function captures *and* has its address taken — the callback
subset that carries the one-hour-per-function cost in 0002's sizing. That makes
it authoritative for the expensive bucket, where the regex is not.

Run in-tree (`~/spike/hurd-gcc-tramp`, log `tramp.log`):

```sh
make -k CC="gcc -Wtrampolines"
```

Two traps found while setting this up, both worth keeping:

1. **The Hurd does not build out-of-tree.** A separate build directory fails
   immediately with `make[1]: *** libports: No such file or directory`. Build in
   a copy of the source tree instead.
2. **Do not pass `CFLAGS=` on the make command line.** `Makeconf` uses
   `CFLAGS +=`, and a command-line assignment overrides the whole thing,
   dropping `-std=gnu99` and breaking every compile. Append the flag to `CC`.

The build was still running when this was written; `grep -c "warning:.*trampoline"`
on `tramp.log` gives the count, and `grep -oE "nested function .[^\x27]*."` the
distinct names. Mechanism verified against a synthetic capturing callback,
which warns as expected, and the flag is confirmed present on the real compile
lines.

## Stage 6: the GCC trampoline cross-check (2026-09-23/24)

The compiler-authoritative check promised in stage 5. GCC emits `-Wtrampolines`
exactly when it generates a trampoline, which happens when a nested function
captures *and* has its address taken — the callback subset carrying the
one-hour-per-function cost in 0002's sizing.

**Commands**, in `~/spike/hurd-gcc-tramp` (an in-tree copy; the Hurd does not
build out-of-tree):

```sh
make -k CC="gcc -Wtrampolines"                     # libraries -> tramp-libs.log
sh tramp_progs.sh                                  # 16 program dirs -> tramp-progs.log
```

A full build is ~11 hours on the single-core i386 VM, and `utils` — the biggest
concentration of nested functions — is 23rd of 34 program directories. So the
second pass builds only the directories that contain nested functions. All 16
compiled with **zero make errors** and all produced object files, so a zero in
the table below is a real result, not a skipped directory.

| Phase | Warnings | Distinct functions |
|---|---:|---:|
| libraries (8 dirs) | 44 | 17 |
| programs (16 dirs) | 63 | — |
| **combined distinct** | | **80** |

Per directory: utils 39, exec 8, ext2fs 6, storeio 4, eth-multiplexer 3,
fatfs 2, sutils 1; and **zero** in trans, proc, console-client, term, isofs,
tmpfs, pflocal, procfs, startup.

`trampolines.csv` has all 80, joined to `nested-captures.csv`.

### Result 1: the regex under-counts captures, as suspected — and now proven

Of the 80 trampolines, **70 are in the 252-row inventory**. A trampoline proves
the function captures, so every one of those 70 should have been classified
`capture`. Sixty-six were. **Four were not**:

| Function | regex said |
|---|---|
| `utils/ftpcp.c:312 parse_opt` | none |
| `fatfs/pager.c:1055 add_pager_max_prot` | none |
| `ext2fs/pager.c:1764 add_pager_max_prot` | none |
| `eth-multiplexer/netfs_impl.c:261 add_each_dev` | sibling |

This is the failure mode stage 5 predicted — false `none`, never false
`capture` — now demonstrated by the compiler rather than by hand-audit. The
corrected count is at least 209 + 4 = **213**, and still a floor.

### Result 2: the inventory itself is incomplete — the denominator is wrong

**Ten trampoline functions are not in the 252-row inventory at all**, including
three in files the inventory lists *zero* rows for (`utils/rpctrace.c`,
`ext2fs/truncate.c`). Examples: `rpctrace.c:1770 parse_opt`,
`truncate.c:160/172/185 free_block`, `pager.c:955/1654 sync_one`,
`portinfo.c:245 parse_opt`, `dev.c:357 buf_write`,
`parse.c:104/122 default_str_add`/`default_num_add`.

These are ordinary nested functions — nothing exotic:

```c
int
main (int argc, char **argv, char **envp)
{
  ...
  error_t parse_opt (int key, char *arg, struct argp_state *state)
    {
```

**Root cause, isolated.** `nested.py` tracks brace depth over the stripped
source and skips any candidate at depth <= 0. Running its own `strip()` over
`utils/rpctrace.c` and counting braces gives **depth 0 at `parse_opt`**, when
the true depth inside `main` is 1. The counter desynchronises somewhere earlier
in the file — an unbalanced brace that `strip()` fails to remove, most likely
inside a preprocessor conditional or a character literal — and every nested
function after that point in the file is silently dropped.

That makes this a *whole-file* defect, not a marginal miss: two files
contribute zero rows each. **252 is an undercount and the true population is
unknown.** Since trampolines are only the address-taken subset of captures, the
10 found here are a lower bound on what is missing.

### What this means for 0002

The direction is unambiguous and unfavourable: both the numerator (213+) and
the denominator (252+) move up. The nested-function conversion is larger than
the stage 5 estimate, not smaller. It does not change stage 5's conclusion that
*glibc's own* share is affordable — the four proven misses and all ten missing
functions are in the **Hurd**, not glibc — but it does mean the Hurd-side
figure should be treated as a floor with no upper bound established.

**Before this number is used for anything load-bearing, `nested.py` needs
replacing with something that does not hand-roll brace counting** — a real C
parser, or at minimum a scan validated against the GCC trampoline set as a
regression test. The 80 rows in `trampolines.csv` are exactly that test set: a
correct scanner must find all 80.

### Coverage limits

- Trampolines are captures **with the address taken**. A capturing nested
  function that is only called directly produces no warning, so 80 is a floor
  on captures, not a count of them.
- `nfs` has 5 nested functions but is not in `prog-subdirs`, so it never builds
  and can never appear here.
- glibc was not built with `-Wtrampolines`; this covers the Hurd only.
- i386. The x86_64 testbed (VM 111) exists now and should be used for any
  re-run.

## Stage 7: nested.py replaced, and the true population (2026-09-30)

**Command:** `tools/phase0/nested.py hurd-0.9.git20260919` and
`tools/phase0/nested.py glibc-2.43/hurd glibc-2.43/sysdeps/mach glibc-2.43/mach`,
run on VM 111 over `apt-get source` trees of the same versions as before
(hurd 0.9.git20260919-1, glibc 2.43-6). Script sha256
`85bbc56e6a91cfdeacdf437de7968b5419a12160a83f503159f7dfccf8aff3d6`.

Version 2 tokenizes instead of regex-stripping. Comments, strings,
character literals and line continuations are whole tokens. Preprocessor
conditionals are followed the way one compiler configuration sees them:
every `#if`/`#elif`/`#else` branch starts at the chain's entry depth, depth
continues from the first branch after `#endif`, and `#if 0` is skipped. The
docstring has the full rules.

**Regression results:**

| Check | v1 | v2 |
|---|---:|---:|
| `trampolines.csv` functions found (GCC-proven nested) | 70 / 80 | **80 / 80** |
| v1's 252 rows still found | — | **252 / 252** |
| New rows | — | 46 |
| **Total** | 252 | **298** (Hurd 222, glibc 76) |

The 46 new rows were checked by printing each one's declaration line from
the source: all 46 are function definitions. The three in
`libfshelp/rlock-tweak.c` have their return type on the previous line, which
is the shape an upward depth error would produce. They were confirmed nested
by locating the enclosing `fshelp_rlock_tweak`, which spans lines 46–549.
**No false positives found.** A unit test with the known failure shapes
(unbalanced braces across `#if`/`#else`, `'{'` literals, `#if 0`,
`__attribute__`, return type on its own line, and seven statement blocks that
must not match) gave exactly the three expected functions.

`nested-functions.csv` now has 298 rows. Its `found_by_v1` column marks the
46 that v1 dropped (43 Hurd, 3 glibc). 10 of those 46 are in
`trampolines.csv`, so they are proven captures.

**Not re-done:** capture classification of the 36 new rows that are not
trampolines. `captures.py` finds enclosing functions with its own hand-rolled
brace matching, the same defect class v1 had, so running it would not be
trustworthy. Decision 0002 is argued with bounds instead: see its 2026-09-30
amendment.

## Stage 8: the x86_64 re-run (2026-09-30, VM 111)

Same workloads (`tools/phase0/workloads.sh`, with the stale-`one.txt` fix), on
the target architecture. Files are in `amd64/`.

| | |
|---|---|
| Kernel | GNU Mach 1.8+git20260805-up-amd64 |
| Hurd, running = sources | 1:0.9.git20260919-1 (no drift this time) |
| libc | libc0.3 2.43-6 |
| msgids | `gnumach.msgids` sha256 `7fce119b10ddc7b61c21be5c15072566ecaec745a3dfe64909718d38e5c887a1` (198 calls; it lacks `host_kernel_version`) |
| Scripts | `workloads.sh` `c1d8b454…d0e1`, `tally.py` `0f358fdd…7284`, `hurdrpc.py` `e85ac436…c9d`: byte-identical to the repo at this commit |

| Raw trace (VM 111, `~aditya/trace/`) | Bytes | Lines | SHA-256 |
|---|---:|---:|---|
| `boot.txt` | 108 170 | 1 098 | `8279f58dec0cea98b612cc06d0943c5dd2293bea539b607461a2d62d026d4427` |
| `shell.txt` | 561 281 | 8 314 | `d486e68a89ade5f500cf407ff2875d02cd74e1ce196aac9a350ac5b9940d1811` |
| `build.txt` | 133 419 167 | 1 613 601 | `a294185e25e6cd5b3d2cf69eda5591ae7dfd3b5918a19961548e8b683a848332` |

| File | Produced by |
|---|---|
| `amd64/mach-call-counts.csv`, `.md` | `tally.py` (as stage 1) |
| `amd64/hurd-rpc-counts.csv`, `amd64/io-offsets.csv` | `hurdrpc.py` (as stage 1) |
| `amd64/i386-vs-amd64.csv` | a join of `mach-call-counts.csv` and `amd64/mach-call-counts.csv` on `call`, keeping calls non-zero on either side |
| `amd64/thread-state-flavours.csv` | `grep -oE "thread_(set\|get)_state \([0-9]+" build.txt \| sort \| uniq -c`; names from `/usr/include/mach/x86_64/thread_status.h` |

**How each workload ended, compared with i386:**

- **boot:** rpctrace **segfaulted** (rc 139) during ext2fs start-up; on i386 it
  hung. Before crashing it recorded the external-pager protocol, which i386
  never reached: kernel → ext2fs page requests and returns (printed as
  `device_pager_*`, the msgid collision from stage 1), and ext2fs → kernel
  `memory_object_data_supply`, `memory_object_lock_request` and
  `memory_object_ready`. Two "error lifting address space limits: (os/kern)
  invalid task" messages come from libpager's `setrlimit(RLIMIT_AS)`
  (`libpager/demuxer.c:327`): the task port is rpctrace's proxy, so the kernel
  rejects it. A tracing artefact.
- **shell:** 9 of 10 commands hit the 60 s timeout (i386: 7). Traces are
  still captured up to the hang.
- **build:** stopped at the first `make`, on `gsync_wait`, exactly as on i386.
  The run was ended at 12:51 by `kill -TERM` of the rpctrace PID, after four
  minutes without output, rather than waiting out the 50-minute `timeout`.
  (`pkill -x rpctrace` hung: `proc` lookups can stall on a wedged traced task.
  `ps -e -o pid,args` then `kill <pid>` worked.)

**Results:**

- The port, VM and fork profile is architecture-independent: most build
  counts agree within a few percent (`task_create` 3 064 vs 3 057,
  `mach_port_deallocate` 343 547 vs 349 274).
- `i386_set_gdt`/`get_gdt` (11 136) are replaced by `thread_set_state`/
  `get_state` flavour 7 (`i386_FSGS_BASE_STATE`): 7 821 sets and 2 472 gets.
  Decision 0010 is amended.
- The client-recovery offset finding holds on x86_64: `io_read` 99.3% at
  offset `-1`, `io_write` 94%. The explicit-offset writes rose from 24 to 1 638,
  still a small minority.
- `io_seek` `SEEK_SET` is up 42% (50 331 → 71 521), probably configure taking
  different paths per architecture. It changes no conclusion.

## Stage 9: the glibc clang build, and its root causes (2026-09-30, VM 111)

**Command:** `tools/phase0/spike-glibc.sh` (glibc 2.43-6, `CC=clang` 21.1.8-10,
out-of-tree, `make -k`, no `-j`), started 12:51 by `~/run-amd64.sh`, with
`tools/phase0/clang-watchdog.sh` capping each clang process in CPU time.

| Raw file (VM 111, `~aditya/spike/`) | SHA-256 |
|---|---|
| `glibc-configure.log` (rc 0) | `7437ff49698a14ba8baecc1afacf044277909940f54a0d6a87aeb5d8c0411d18` |
| `glibc-make.log` | `7e12c2920a88eaac50a40ea0ede905460eee3e353bba40c5f0c893857cad1105` |
| `clang-watchdog.log` (copied to `amd64/`) | `d0e005f565322afcef7a38997f760f66b8967e15977f7f6ff33d260699213c14` |

**How far it got.** glibc builds in two passes over its subdirectories. The
first pass (`subdir_lib`, which builds libc itself) **completed through all
52 subdirectories**. The second pass (libm and programs) was **stopped by
decision at 15:09** in `math/`, because every libm file there was hitting the
clang hang below. So libc's error surface is complete, and libm's is not.

**Watchdog.** 13 compiles were stopped (`amd64/clang-watchdog.log`):
`wcsmbs/wcstold_l.c` (PIC variant) manually at 30 CPU-min, `s_llroundl.c` at
26:32, then 11 more at the 5-min cap after it was lowered at 14:01 (the log
records the change). 12 of the 13 are math code.

### The clang hang, diagnosed on one file

`s_lround.c`, using its exact build command (`amd64/clang-hang-s_lround.cmd`):

| Variant (2-min cap unless noted) | Result |
|---|---|
| as built | still running at cap |
| without `-frounding-math -ftrapping-math` | still running at cap |
| `-O1` | still running at cap |
| `-O0` | refused by glibc's own `#error` (not informative) |
| `-fsyntax-only` (frontend only) | still running at cap |
| `-E` (preprocess only) | 0 s |
| `-fsyntax-only` on the preprocessed `.i`, all build flags | 0 s |
| `-fsyntax-only -w` (all warnings off) | still running at cap |

`SIGSEGV` sent at 10, 20, 60 and 120 s makes LLVM print its stack
(`amd64/clang-hang-stack.txt`). **Every sample shows the same parser position,
`math/bits/mathcalls-helper-functions.h:25:33`, inside
`Sema::warnOnReservedIdentifier` → `SourceManager::getPresumedLoc`.** That's an
infinite loop, not a slow compile, and it depends on clang's source-location
state (the same code parses instantly from preprocessed input).
`getPresumedLoc` is also where the two clang **crashes** died (`locale/loadlocale.c`,
`intl/localealias.c`, in `glibc-make.log`), so one defect probably explains
both. *Only `s_lround.c` was diagnosed; the other 12 stopped compiles are
assumed to be the same hang, not shown.* Whether it is specific to the Hurd
or a clang 21 bug on every target is **not yet known**.

### Error classification

`clang_errors.py` → `extern_inline_guard.py` → `root_cause.py` produce
`amd64/clang-glibc-root-causes.csv` (sha256
`486b05982094804999ea688fd04a84b6966a6a78e2c756544a3bc7ced85d931a`): 625
distinct error sites (file, line, message, per subdirectory), each with a
root cause. The rules and their evidence are in `root_cause.py`'s docstring.

| Root cause | Sites |
|---|---:|
| extern-inline guard | 281 |
| nested function | 249 |
| nested-function knock-on | 43 |
| unattributed | 30 |
| generated `<stdin>` (gen-as-const) | 16 |
| VLA in struct | 5 |
| asm label | 1 |

**The extern-inline guard, established step by step:**

1. `include/features.h:554` leaves `__USE_EXTERN_INLINES` undefined when glibc
   itself is built with clang (`!(defined __clang__ && defined _LIBC)`).
   Confirmed with `clang -dM -E` on `alloc-fd.c`'s build command: `__OPTIMIZE__`
   is 1, `__extern_inline` is defined, `__USE_EXTERN_INLINES` is not.
2. Hurd-port headers put ordinary declarations behind that guard.
   `extern_inline_guard.py` finds **53 identifiers** in guarded blocks, and the
   errors trace to four headers: `hurd/hurd/fd.h` (126 sites),
   `sysdeps/hurd/include/hurd/signal.h` (22),
   `sysdeps/mach/htl/bits/spin-lock-inline.h` (14), `mach/lock-intern.h` (10).
3. `hurd/hurd/userlink.h:27` includes `<hurd/signal.h>` **only** under the
   guard, and it's the only path by which `hurd.h` reaches it. Hence the 50
   `HURD_CRITICAL_BEGIN`/`END` errors: `clang -dM -E` on `hurdauth.c` shows
   neither that macro nor `_HURD_SIGNAL_H` defined.
4. The alias errors point at inline functions defined inside guarded blocks
   (e.g. `_hurd_port_locked_get`, `hurd/hurd/port.h:91`). The "subscript of
   pointer to function type `int (int, error_t)`" errors are clang's
   did-you-mean replacing the hidden `_hurd_dtable` with `_hurd_fd_error`.

**The scanner, once more.** Reading an unattributed error (`do_init`
undeclared, `sysdeps/htl/pt-key.h:82`) exposed a nested function **in a
header**, in a directory the glibc scan never covered (`htl/`,
`sysdeps/htl/`). `nested.py` now scans headers and tracks what each brace
opened (code, aggregate, or `extern "C"` linkage), so C++ inline and member
functions in headers are not counted; the old depth rule reported 25 of those (109 rows over the whole tree, against 84 now)
in `math.h`, `iscanonical.h`, `nptl/pthread.h`, `arm/unwind.h` and
`i386/sys/ucontext.h` and `powerpc/sys/ucontext.h`. It was then run over **the whole glibc tree**, not a
directory list. Regression: 80/80 trampolines, all 298 earlier rows kept, 8
new glibc rows. `nested-functions.csv` now has 306 rows and an
`in_hurd_build` column: glibc **77** in the Hurd build (the new one is
`pt-key.h`'s `do_init`), 5 in other architectures (m68k, powerpc), 2 in test
programs. Script sha256 `0ceb17d590309d9cf8088838e27b52a247a811e87b5df8dfe11c68a8d0bac8a1`.

**Unattributed (30 sites, ~15 distinct):** the x86_64 signal code
(`sysdeps/mach/hurd/x86/trampoline.c`: `offsetof`, `sc_r8`, `sc_fpkind`,
`__mach_host_self`; `x86_64/sigreturn.c`: dereference of incomplete
`unsigned char[]`), `catch-exc.c` (`__mach_task_self`, declared unconditionally
in `mach_init.h`, so a different include problem), and int-to-pointer
conversions in `geteuids.c`, `getgroups.c`, `group_member.c`, `wait4.c` and
`waitid.c`. Not yet read. Some may be further guard knock-ons.

## Stage 10: is the clang hang specific to the Hurd? (2026-10-01)

**1. Version × target matrix on the Hurd** (`tools/phase0/hang-matrix.sh`,
VM 111, `s_lround.c`'s exact build command at `-fsyntax-only -w`, 60 s cap).
clang 19.1.7 (`7+hurd.1`) and 22.1.8 (`1+b1`) were installed alongside 21.1.8
from debian-ports.

| clang | `--target` | Result |
|---|---|---|
| 19.1.7 | default (Hurd) | stopped at cap, `mathcalls-helper-functions.h:25:33`, `Sema::warnOnReservedIdentifier` |
| 19.1.7 | `x86_64-linux-gnu` | same |
| 21.1.8 | default | same |
| 21.1.8 | `x86_64-linux-gnu` | same |
| 22.1.8 | default | same |
| 22.1.8 | `x86_64-linux-gnu` | same |

Not a version regression, not fixed in 22, and not caused by the Hurd *target*.

**2. Duplicate file identities ruled out.** clang's FileManager identifies
files by `(st_dev, st_ino)`. All 131 headers the compile opens (`-E -H`) were
`stat`ed: no two distinct paths share an identity.

**3. Same input on a Linux host.** The compile's complete input, every file in
its `-M` dependency list (139 files, `amd64/clang-hang-bundle.list`), was
packed at absolute paths into `hang-bundle.tgz` (84 KB, sha256
`d094ceb1c51ad6dbf60c1bd027989006e61e634a7e5f3e55ecc7714bae5e555f`).
`tools/phase0/hang-bundle-run.sh` runs the command from the bundle with the
VM's default include search list pinned (`-nostdinc` + `-isystem` into the
bundle), so the host's own headers are never consulted.

| Host | clang | `--target` | Result |
|---|---|---|---|
| VM 111, GNU 0.9 (control) | Debian 21.1.8 (10) | Hurd | **stopped at cap**, same position and frame |
| VM 111, GNU 0.9 (control) | Debian 21.1.8 (10) | `x86_64-linux-gnu` | **stopped at cap**, same position and frame |
| Workstation, Linux 6.18 (Void) | 21.1.8 | `x86_64-linux-gnu` | **0 s, 0 errors** |
| Workstation, Linux 6.18 (Void) | 21.1.8 | Hurd | 0 s, 2 errors: `__float128`/`__TC__` unsupported on this target (`bits/floatn.h:83,97`); no hang |

The first bundle, built from `-H` output, was incomplete: `-H` doesn't list
headers reached from forced `-include` files, and the control stopped at once
on `libc-misc.h`. The `-M` dependency list is complete; the control then
reproduced the hang exactly. That's why the control row exists.

**Result: the hang belongs to clang *running on* the Hurd.** Same clang
version, same target, same 139 input files: it loops forever on the Hurd and
finishes instantly on Linux. Not separated: whether the cause is Debian's
Hurd-specific clang patches, a miscompile of clang for hurd-amd64, or the Hurd
runtime (`libc0.3`). That would take an upstream-built clang running on the
Hurd.

**Side finding:** upstream clang (Void's build) lacks `__float128` for the
Hurd target; Debian's Hurd clang has it. Cross-compiling glibc for the Hurd
from Linux with upstream clang needs that support.

## Stage 11: GNU Mach and MIG output with clang; `nested.py` made configuration-consistent (2026-10-01)

Details and results are in `docs/clang-inventory.md` §1–2. Provenance:

| File | Produced by | SHA-256 / note |
|---|---|---|
| `amd64/mig-clang.csv` | `tools/phase0/mig-clang.sh hurd-0.9.git20260919 clang-21` | `9f263fb168e7c7f8e104d40a16e27327dc667ee78b08e22a2e6f95bfa40e5710` |
| GNU Mach clang build log (VM, `~/spike/gnumach-clang-21-make.log`) | `spike-gnumach.sh clang-21 GCC_INSTALL=… "CC=clang-21 -fno-integrated-as"` | `79423ef8351084d559f4323785f8e6d1ba84e5de2bd9cfb49092661d65c5dc73` (configure log `1ea27e29a5dba546d3892d4bcf463e6c0bbf1c12590063d782b1997c249d5688`) |
| GNU Mach gcc build log (VM, `~/spike/gnumach-gcc-make.log`) | `spike-gnumach.sh gcc` | `794ed761394a6c63b6cda2cf5bb10517505de4905af0fb3d28e5265b177bb3fd`; baseline, complete kernel |
| `amd64/nested-gnumach-scan.tsv` | `nested.py gnumach-1.8+git20260805` | `9da7f00c5bb0f38c04c2dd83f242ae354584a56f5bb37b28bbda7d937d0baf0f`; 32 rows, see below |

**`nested.py` change.** Scanning GNU Mach exposed a third failure shape. The
scanner followed the *first* branch of every conditional independently, and
in `i386/intel/pmap.c` `#ifdef __x86_64__` is followed later by
`#ifndef __x86_64__`. Taking the first branch of each mixes two
configurations, the brace stack ends one level deep, and every later function
in the file (37 of them) read as nested, although clang compiled the file
cleanly. Now every macro is assumed true at its first test and that value is
reused (`branch_taken`), so the depth after each `#endif` follows one
consistent configuration. All branches are still scanned.

- Regression: Hurd and glibc output is **byte-identical** to before (md5 of
  the full TSVs), so all 306 earlier rows and 80/80 trampolines stand.
- A further rule, preferring "not taken" for unbalanced chains without
  `#else`, fixed `53c78xx.c` but broke `pmap.c` and `i82365.c`. It was
  **removed**, and the shape is kept as a documented known limitation
  (`tools/phase0/tests/nested/known-limits/`).
- Unit tests: `tools/phase0/tests/nested/run.sh` (`shapes.c`, `config.c`,
  both pass). Script sha256
  `d5207cff66ec223e68f94f28235f105dfc57c22db74411880186c8a12d5deb10`.

## Stage 12: the Hurd's full clang error surface (2026-10-01, VM 111)

**Command:** `tools/phase0/spike-hurd-clang.sh clang-21`, and
`spike-hurd-clang.sh gcc` as the baseline: an in-tree copy, configured once,
then `make -k` per directory over all 57 library and program directories.
Results in `docs/clang-inventory.md` §4.

| File | SHA-256 |
|---|---|
| clang logs (VM, `~/spike/hurd-clang-21/logs/*.log`, concatenated in name order) | `7d1e6249f4269a1fd361c6b7c4893d5e8108eec79c8a72938627a90e2a42e058` |
| gcc logs (VM, `~/spike/hurd-gcc/logs/*.log`, concatenated) | `2175051f90bf53e5133b4e30b33f471afb3763209d941d8727e332ad28a48750` |
| `amd64/hurd-clang-errors.csv` (`hurd_clang_errors.py`; includes the 2 `version.h` artifact rows, which the inventory excludes) | `1a6b7e760646160dbcfaa54a3e1cdf2ec57b0e922bb9f09463da157ce0c41b5b` |

**Coverage check:** object files named on each compiler's command lines,
per directory. Clang has all 1 492 of GCC's, plus `sutils/clookup.o`. The
remaining differences in raw command counts (1 734 vs 1 758) are GCC's link
steps, which clang never reached.

## Stage 13: GCC baselines (2026-10-01, VM 111)

Phase 0 asks for the Hurd, GNU Mach and glibc to be built from source; these
are also the controls for the clang inventory.

| Component | Command | Result | Log SHA-256 |
|---|---|---|---|
| glibc 2.43-6 | `tools/phase0/spike-glibc.sh gcc g++` | rc 0, 0 errors, `libc.so` + `libm.so`, 19 min | make `e70523c38a91260e7a2e3f1554b575d0e846889302c55e1afadc2f93a8258b53`, configure `98365a16c3e866faa7c689164c87cc0b2fab19741018745b3abf38ce5f96ce5a` |
| GNU Mach | `tools/phase0/spike-gnumach.sh gcc` | rc 0, complete kernel (stage 11) | `794ed761394a6c63b6cda2cf5bb10517505de4905af0fb3d28e5265b177bb3fd` |
| the Hurd, per directory | `tools/phase0/spike-hurd-clang.sh gcc` | all 1 492 objects (stage 12) | `2175051f90bf53e5133b4e30b33f471afb3763209d941d8727e332ad28a48750` |
| the Hurd, full top-level build | `tools/phase0/hurd-full.sh gcc` | rc 0, ~5 min: all servers (`auth`, `exec`, `ext2fs`, `proc`, …) and 47 shared libraries. The script's "errors: 1" is a grep hit on a quoted source line in a warning (a `printf` format containing `error:%d`), not a compile error; `make` reported 0 failures | make `5c1c6c41b7c71e38896c7a45cd1d9474b7eb45d76c6299dfe1576a425404d8a4`, configure `3e5a0b5cb4c4eb94d63dc5ca3bc335432b02183a005a45b3624db56ac1ee6d68` |

`spike-glibc.sh` now takes the compiler as an argument and names its logs
after it (`glibc-gcc-make.log`). Stage 9's clang logs keep their original
names (`glibc-make.log`), so their recorded checksums are unaffected.

**A trap found on the way:** the first full Hurd build reused the
per-directory tree and failed in 4 s with "No rule to make target
`version.h`". The per-directory run's failed compiles had left dependency
files (e.g. `libdiskfs/opts-version.d`) naming a bare `version.h`, which no
rule matches once the top level generates the real one. A full build has to
start from a fresh copy (`hurd-full.sh` does).

With these, Phase 0's "build Hurd, GNU Mach and glibc from source" is done on
the target architecture: all three build cleanly with GCC on VM 111.
