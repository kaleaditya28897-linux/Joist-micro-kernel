# 0002 — Clang everywhere, with a pre-committed GCC fallback for glibc

Date: 2026-09-23
Status: proposed — policy accepted, outcome pending the Phase 0 measurement

## Context

The project builds everything with Clang/LLVM (§13). glibc has historically
required GCC, and building it with Clang is not fully supported upstream; parts
of the Hurd and MIG-generated code also assume GCC extensions.

The risk is not that the fallback is wrong. The risk is spending an unbounded
amount of Phase 7 — the riskiest phase in the roadmap — fixing someone else's
build system, and deciding to stop only when morale runs out.

## Options considered

**A — Fix Clang build issues in glibc/Hurd as part of the port.** Matches the
mission exactly. Unbounded work inside the phase least able to absorb it.

**B — Use GCC for glibc from the start.** Simplest, but concedes the
Clang-everywhere goal without ever measuring whether it was achievable.

**C — Goal A, with a budget and a fallback to B that is decided in advance.**

## Decision

Option **C**.

1. **Goal:** Clang everywhere, glibc included.
2. **Budget:** Phase 0 spikes a Clang build of glibc and the Hurd and sizes the
   fix. If the work exceeds **one month of the Phase 7 budget**, the fallback
   triggers automatically.
3. **Fallback:** glibc is built with **GCC**; everything Joist owns — kernel,
   RS, DS, memory manager, MIG backend, new servers — stays Clang. Two
   compilers in the build is acceptable; an unbounded yak-shave is not.

The fallback is taken on a **measurement**, not on a judgement call made in
month four of Phase 7. That is the whole point of writing it down now.

## Status of the measurement

**Preliminary, 2026-09-23: in progress, no branch taken yet.** Data and
provenance are in `docs/phase0-data/` (README, *Stage 4*); the scripts are in
`tools/phase0/spike.sh` and `tools/phase0/nested.py`.

Setup: Debian GNU/Hurd i386 testbed, clang 21.1.8, hurd 0.9.git20260919 and
glibc 2.43 Debian sources, each configured with `CC=clang` and built
out-of-tree with `make -k`.

**Findings so far:**

1. **Toolchain availability.** clang 21 installs from debian-ports.
   **`lld` does not exist for hurd-i386**, so "Clang everywhere" means Clang
   plus GNU ld on this target for now. Re-check on hurd-amd64.
2. **glibc 2.43 configures with clang without complaint.** Upstream's
   configure accepts clang 18 and newer (`configure.ac:588`). The build is
   running. After ~18 minutes (still in `mach/`) it had 6 distinct errors: 5
   from the nested functions in `mach/devstream.c`, and 1 clang-specific
   (`include/stdlib.h`: "cannot apply asm label to function after its first
   use"), glibc's symbol-aliasing machinery that has no nested-function cause.
   Update this entry when the build finishes (`clang-glibc-errors.txt`).
3. **The Hurd fails on one root cause: GCC nested functions.** The build
   stopped after 8 minutes in `libshouldbeinlibc`, `libbpf` and `libirqhelp`,
   which nearly everything else links against. All 38 distinct errors are
   nested functions, or calls to them that become implicit declarations.
   Because the build stops early, the *count* comes from a source scan, not
   from the compiler:

   | Tree | Nested functions |
   |---|---:|
   | Hurd (all of it) | 179 |
   | glibc Hurd port (`hurd/`, `sysdeps/mach/`, `mach/`) | 73 |

   The scan agrees exactly with clang on the 8 files clang reached. In the
   Hurd, `utils/` alone has 61; after that come
   `libstore/` 13, `libshouldbeinlibc/` 12, `exec/` 11, `storeio/` 6, `libfshelp/` 5.

**Sizing, not yet decisive.** Each nested function has to become a static
function. Where it captures locals of its enclosing function, that means
closure conversion: pass a context struct, and for ones used as callbacks
(most are), thread it through the callback API. With 252 functions, at 15
minutes each (no captures) that is ~60 hours; at an hour each (captures
plus callback plumbing) ~250 hours, roughly 6 person-weeks. The budget line
(**one month of Phase 7**) falls inside that range, so the measurement is not
yet decisive. **The deciding number is how many capture enclosing locals.**
That's the next step, along with the finished glibc error list, and with the
knock-on errors that the nested functions currently hide in the rest of the
Hurd tree (a second `make -k` after hoisting the 8 blocking files would expose
them).

One scope note for when this is decided. Of the Hurd's 179,
61 are in `utils/` (command-line tools), not in servers or libraries.
If only the glibc half of this decision falls back to GCC, as the Decision
section already allows, the Hurd's own conversion is still needed for
"everything Joist owns stays Clang" wherever Joist reuses Hurd servers.

## Consequences

- Easier: Phase 7 has a known worst case; nobody has to re-litigate the
  decision under schedule pressure.
- Harder: the build may end up using two compilers, so CI must build and test
  both toolchains rather than assuming one.
- An early, cheap lesson is worth having: **`CC ?= clang` does not work in
  GNU Make.** `CC` is a built-in variable with origin `default`, so `?=` never
  fires and the build silently uses `cc` — which is GCC on most distributions.
  This was already found and fixed in `mcp-server/Makefile`; every Makefile in
  this project must guard on `$(origin CC)` instead. A policy that only holds
  when someone remembers to check `make`'s output is not a policy.

## Priority check

Respects the order. Reuse (of upstream glibc as-is) is the goal being protected
by the fallback, and it is explicitly allowed to lose: if Clang-only were
required for Reliability we would pay option A's cost. It is not — the
compiler choice for glibc does not affect the kernel's guarantees.
