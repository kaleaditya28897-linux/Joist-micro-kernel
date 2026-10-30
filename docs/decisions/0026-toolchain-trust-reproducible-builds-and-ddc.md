# 0026 — Toolchain trust: reproducible builds, a full-source bootstrap, and diverse double-compiling

Date: 2026-10-29
Status: **accepted** (the owner, 2026-10-29). The owner chose the approach
(diverse double-compiling, "include the full-source bootstrap"), and to
grow the build VM's disk for it.
Revises: the plan's §9.1 (a toolchain row in the trusted computing base)
and §13 (the toolchain: Joist is built with a compiler it has verified,
not a distribution's binary).

## Context

Everything Joist ships is compiled. A compiler binary that has been
subverted can insert code into what it compiles, including into new
builds of itself, so reading the source of the kernel, `mm` or the
compiler shows nothing (Thompson, *Reflections on Trusting Trust*, 1984).
Today Joist is built with Debian's Clang 21 and `ld.lld`, binaries Joist
didn't build, and its ISO holds Debian's Limine binaries. §9.1 doesn't list
the toolchain at all.

**Diverse double-compiling** (DDC; Wheeler, 2005 and his 2009 thesis)
detects such a compiler:
- **Stage 1:** compile the compiler's source with a second, unrelated
  compiler, the *trusted* one.
- **Stage 2:** compile the same source again with that result.
- **The comparison:** if stage 2 is bit-for-bit the same as the
  compiler's own self-build, the binary in use corresponds to its source.
- **When it fails to detect:** only if the trusted compiler is subverted
  in the same way, which an attacker would have to arrange across two
  independent compilers and their histories.

DDC needs builds that are **reproducible**: the same source, compiler and
flags give the same bytes. Without that, a mismatch proves nothing.

How strong DDC is depends on the trusted compiler. A GCC from another
distribution trusts that distribution's binaries. A **full-source
bootstrap** trusts only a seed small enough to read in full. The Bootstrappable
Builds project's `live-bootstrap` starts from `hex0`, a few hundred bytes of
hand-auditable machine code, then builds an assembler, a small C compiler
(M2-Planet, GNU Mes, TinyCC) and, through older GCCs, a current GCC, from
source alone. Guix's full-source bootstrap does the same, but runs under
Guix's own binary Guile.

## Options considered

1. **Reproducible builds only.** Detects nondeterminism and some tampering
   between machines, not a subverted compiler that subverts consistently.
2. **DDC with a distribution's GCC as the trusted compiler.** Cheap; trusts
   that distribution.
3. **DDC with a GCC from a full-source bootstrap** (this decision, the
   owner's choice). The trusted compiler traces back to an auditable seed.

For the bootstrap, `live-bootstrap` is preferred over Guix: its chain runs
from the seed itself, without a distribution's binaries in it.

## Decision

### Layer 1: Joist builds are reproducible
- **Every artifact Joist builds** (the kernel ELF, the user programs and
  servers, the ISO) is the same bytes from the same source and toolchain:
  - whatever directory it is built in (`-ffile-prefix-map`);
  - whenever it is built (`SOURCE_DATE_EPOCH` from the last commit; fixed
    dates in the ISO);
  - whoever builds it, with any umask, locale or time zone.
- **`make repro-check`** builds everything twice under different
  directories, users, times and locales, and compares the bytes. A
  difference fails the check, and `diffoscope` shows it.
- **Run:** in the full check; its result goes in every exit record.

### Layer 2: a trusted compiler from a full-source bootstrap
- **`live-bootstrap`**, pinned to a release, is run in a QEMU guest on the
  build VM from its `hex0` seed to a current GCC (C and C++, recent enough
  to build LLVM 21).
- **Recorded:** the seed's hash, the release, and the hashes of the GCC it
  produced. Anyone with the same seed and sources can rebuild it and
  compare. The seed is small enough to read, and someone does read it
  before the first use.

### Layer 3: DDC of Joist's compiler
- **Source A:** LLVM pinned to one 21.x release (Clang, `lld`,
  `llvm-objcopy`, `llvm-objdump`, `llvm-ar`), with fixed CMake options for
  a reproducible build.
- **Path T:** the bootstrapped GCC compiles A (stage 1); stage 1
  compiles A (stage 2T).
- **Path D:** Debian's Clang 21 compiles A (stage 1'); stage 1'
  compiles A (stage 2D).
- **The result:** 2T and 2D must be bit-for-bit identical. Each stage 2 is
  then the same function of A whichever compiler started its chain, so
  neither the bootstrapped GCC nor Debian's Clang has inserted anything
  that survives into it.
- **Afterwards:** Joist is built only with that verified stage 2, kept as
  a pinned toolchain with its hashes. Debian's Clang is used only to build
  path D.
- **The Limine binaries** are built from pinned source with the verified
  toolchain instead of copied from Debian.
- **Rerun** whenever the pinned LLVM release changes, and once per phase
  exit; the record says when it last passed.

### What this does not cover (§9.1's new row says so)
- **The source itself:** DDC shows the binary matches its source, not that
  the source is benign (LLVM's, GCC's, Joist's own).
- **The machine the builds run on:** its kernel, C library, `make`,
  CMake, Python and shell. The build VM's Debian is trusted for running
  them. A later step can run Layer 3 inside the bootstrapped environment
  itself, to shrink this.
- **`xorriso`, QEMU and the firmware.** QEMU only matters for testing.
  The ISO's own bytes are covered by `repro-check`, not by DDC.
- **Hardware.**

## Consequences

- **§9.1** gains a row: *Toolchain* (the compiler, linker and binary
  tools), trusted for compiling what the source says. If compromised:
  total loss, invisibly. Mitigated by Layers 1-3 and limited as above.
- **§13:** "Clang, pinned to one LLVM major version" becomes "Clang and
  lld built from a pinned LLVM release and verified by DDC (0026)".
- **Cost:**
  - Layer 1 is small (flags, the ISO's dates, one make target).
  - Layer 2 takes several hours of machine time per run.
  - Layer 3 is four LLVM builds, roughly 1.5-3 hours each on the build
    VM's 8 vCPUs, with parallel links limited to fit its 7.9 GB.
  - The build VM needs more disk, about 60 GB free against 25 GB today:
    its disk image is grown (the owner, 2026-10-29).
- **Order:**
  - Layer 1 first, since everything else needs it.
  - Then Layer 2, then Layer 3, as long jobs on the VM between kernel
    work, after the fallback context (0025) is in the kernel.
- **A failure is a finding, not a fix-up:** a mismatch in Layer 3 is
  investigated with `diffoscope` until it is explained. Either it is a
  nondeterminism (fixed in the build) or it isn't, and a mismatch that
  isn't explained blocks the release.

## Priority check

- **Correctness and security:** closes the largest unlisted gap in §9.1.
  An invisible compromise of everything is now detectable, back to a
  seed one can read.
- **Simplicity:** no change to the kernel or servers; build flags and
  scripts.
- **Performance:** none at run time.
- **Reuse:** the Bootstrappable Builds project's seed and chain, Wheeler's
  method and the Reproducible Builds project's tools, rather than anything
  of our own.

## Amendment 1: proposed, awaiting the owner (2026-10-30)

From carrying out layers 2 and 3 (docs/toolchain/record.md). Where the
decision was silent or reality differed, the option that trusts less was
taken. Nothing here is accepted until the owner says so.

1. **live-bootstrap is pinned by commit**, not to a release: it has no
   tagged release. The commit is b1ceced7 (master, 2026-09-17). Its
   stage0-posix is at its tag Release_1.9.1.
2. **Layer 3 runs inside the bootstrapped system,** not on the build VM's
   Debian. Every stage except path D's stage 1' is built in live-bootstrap's
   final system, booted under QEMU/KVM on the build VM. Stage 1' is built
   by Debian's Clang on Debian, as path D needs.
   - This **shrinks "the machine the builds run on":** the stage 2 builds
     no longer run on Debian's kernel, C library, make, CMake, Python or
     shell. They run on the bootstrap's own Linux 4.14, musl, make and
     Python, and on CMake and ninja built there from signed source by the
     bootstrapped GCC. Still trusted for this: QEMU, KVM and the build VM's
     kernel, which could change anything in the guest.
   - **live-bootstrap supports only 32-bit x86**, so the trusted GCC
     makes only i686 code. Every stage is therefore a static i686
     program. The verified Clang and lld run on Debian x86_64 as they are,
     and make x86_64 code for Joist like any Clang.
   - **Stage 1' links Debian's i686 C library and libstdc++,** not the
     guest's. Those were compiled by the bootstrapped GCC, and path D would
     otherwise not be independent of path T.
   - **Not covered,** a new item for "What this does not cover": the
     musl, libstdc++ and libgcc code linked into the verified binaries
     was compiled by the bootstrapped GCC, and DDC does not check it. It
     traces to the seed through the bootstrap, not through DDC.
3. **Every LLVM stage does its floating point in SSE2**
   (`-march=pentium4 -mfpmath=sse`). The first comparison failed on this:
   the bootstrapped GCC's i686 default is x87, Debian's Clang's is SSE2,
   and LLVM's heuristics use floats. The fix is the same on both paths and
   ignores no bytes; record.md has the investigation.
