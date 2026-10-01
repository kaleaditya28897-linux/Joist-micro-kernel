# Clang build inventory

Phase 0 deliverable (§11): build GNU Mach, the Hurd, MIG output and glibc with
Clang, and record every failure. The sizing judgement built on this lives in
decision 0002; this file is the per-component record behind it.

All builds are on the x86_64 testbed (VM 111, `docs/testbed.md`) unless
stated, with Debian clang 21.1.8 (`1:21.1.8-10`). Provenance (commands,
versions, raw-log checksums) is in `docs/phase0-data/README.md`, under the
stage named in each section.

## Summary

| Component | Result | Blocking issues | Detail |
|---|---|---|---|
| MIG output | **clean** | none | §1 |
| GNU Mach | **one nested function** from building; every object compiles | build system: `GCC_INSTALL`, `gensym` markers | §2 |
| glibc | libc pass complete; libm blocked | extern-inline guard (Hurd headers), nested functions; clang-on-Hurd hang | §3 |
| the Hurd | partial (i386 only) | nested functions | §4 |

## 1. MIG output

`tools/phase0/mig-clang.sh`: for each of the Hurd's 45 `hurd/*.defs`,
generate user and server stubs with `mig` (GNU MIG 1.8+git20231217), then
compile each with clang and with gcc 16.2 (the control) at `-O2 -Wall`. Data:
`phase0-data/amd64/mig-clang.csv` (sha256
`9f263fb168e7c7f8e104d40a16e27327dc667ee78b08e22a2e6f95bfa40e5710`).

- **84 stubs from the 42 real interfaces: all compile with clang. 0 errors,
  0 warnings**, the same as gcc.
- 3 `.defs` produce no stubs because `mig` refuses them: `hurd_types.defs` and
  `ioctl_types.defs` are type-only include files without a `subsystem`, and
  `ioctls.defs` pulls in a C header. The Hurd's own build never runs `mig` on
  them. Not a failure.

MIG's generated C needs no work for clang. That matters for Phase 5: the
retargeted MIG backend starts from output that is already clang-clean.

## 2. GNU Mach

GNU Mach 1.8+git20260805 (Debian source), `--host=x86_64-gnu
--enable-platform=at`, `tools/phase0/spike-gnumach.sh`. Baseline: the same
script with `gcc` (16.2) builds a complete kernel (rc 0, a 3.2 MB static
x86_64 ELF) from 177 objects.

**Result: clang compiles the same 177 objects as GCC.** The only compile
error is one GCC nested function, `check_filter_list` in
`device/net_io.c:1158`, which stops the final link. Warnings: 307, nearly all
unused-command-line-argument (185) and C23-extension notices (71) from
`-std=gnu23`. Nothing that blocks.

Two build-system issues had to be worked around first, both GCC assumptions
rather than code:

1. **`GCC_INSTALL`.** `Makefile.am:46` builds the kernel with `-nostdinc
   -I $(GCC_INSTALL)/include` to get the compiler's freestanding headers
   back, and `GCC_INSTALL` is parsed from the `install:` line of
   `$(CC) -print-search-dirs`. clang prints no such line, so the path
   becomes `-I /include` and every file fails on `<stdint.h>`. **Fix:** use
   `$(CC) -print-file-name=include`, which both compilers answer. Workaround
   used: `make GCC_INSTALL=$(clang -print-resource-dir)`.
2. **`gensym` markers.** The assembly constants file (`gensym.awk` →
   `i386/i386/i386asm.symc`) plants lines like `* APIC_ID mAgIc$32` in inline
   `asm` and extracts them from the compiler's output. GCC copies inline asm
   through unread; clang's integrated assembler parses it and rejects the
   `*` lines (19 errors). Linux hit the same problem with `asm-offsets` and
   uses markers that are valid assembly (`.ascii "->NAME …"`). **Fix:** that
   change in `gensym.awk`. Workaround used: `CC="clang -fno-integrated-as"`,
   which means inline asm in C files was checked by GNU as, not clang, in
   this run.

Also: run `autoreconf -fi` first, as Debian's build does. Its patches touch
`configure.ac`, and the tarball's generated files come from automake 1.18
(the testbed has 1.19).

**Nested functions in GNU Mach.** The compiler is the authority for the built
configuration: 1 (above). `nested.py` over the whole tree
(`phase0-data/amd64/nested-gnumach-scan.tsv`, 32 rows) also finds 9 in
`tests/test-task.c` and 1 in `linux/pcmcia-cs/glue/pcmcia_glue.h`. Both are
genuine by inspection (indented inside an enclosing function), but neither
is compiled by the x86_64 build. It also reports 21 rows in
`linux/src/drivers/scsi/53c78xx.c` that are **false positives**: a known
scanner limitation, documented in `nested.py`. They're in old Linux driver
code the x86_64 build does not compile either.

So GNU Mach's clang work is small and fully listed: one nested function, two
build-system fixes.

## 3. glibc

Detail in decision 0002 and `phase0-data/README.md` stages 9–10. In short:

- libc's pass (all 52 subdirectories) completed. 625 error sites, each with
  a root cause in `phase0-data/amd64/clang-glibc-root-causes.csv`.
- **281** sites: one interaction. `features.h` disables
  `__USE_EXTERN_INLINES` for clang builds of glibc, and five Hurd-port headers
  hide declarations, inline definitions and one `#include` behind it.
- **249 + 43** sites: GCC nested functions (77 in the Hurd build) and
  their knock-ons.
- A small tail: a VLA in a struct (`hurdstartup.c`), one asm label
  (`include/stdlib.h`), generated offset programs, ~15 unattributed sites
  mostly in the x86_64 signal trampoline.
- **libm cannot be built *on* the Hurd:** clang 19, 21 and 22 loop forever in
  the frontend on glibc's math headers. The same input finishes instantly
  with the same clang on Linux, so build glibc on Linux, cross to the Hurd.
  Upstream clang lacks `__float128` for the Hurd target; Debian patches it in.

## 4. The Hurd

Measured on the i386 testbed only (VM 110, retired), with clang 21: the build
stopped after 8 minutes in `libshouldbeinlibc`, `libbpf` and `libirqhelp`,
which most of the tree links against. All 38 distinct errors are GCC nested
functions or calls to them (`phase0-data/clang-hurd-errors.txt`).

The nested-function count is complete regardless: **222** in the Hurd tree
(`phase0-data/nested-functions.csv`, scanner validated against all 80
GCC-proven trampolines). What is *not* measured is every other clang error
class in the Hurd, because the libraries fail first and stop everything
downstream. That's the remaining gap in this inventory.
