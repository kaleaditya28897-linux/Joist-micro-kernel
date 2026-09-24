# 0002 — Clang everywhere, with a pre-committed GCC fallback for glibc

Date: 2026-09-23
Status: proposed — policy accepted. Nested-function count measured 2026-09-23
(see amendment); the rest of the glibc error surface is still unmeasured.

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

### Amendment, 2026-09-23: the deciding number

**At least 209 of the 252 nested functions (83%) capture enclosing locals.**
7 more reference only sibling nested functions; 36 appear self-contained.
Split by tree: Hurd **149 capture** of 179, glibc **60 capture** of 73.

**209 is a floor, not a measurement.** The analysis is a regex heuristic that
has been revised four times, and each revision moved the count up (201 → 176 →
185 → 209) as hand-audits of the `none` bucket found more captures — 2 of 6
wrong in one sample, 3 of 7 in another, a different bug each time. A known
false `none` still survives. Method and the full audit trail are in
`docs/phase0-data/README.md` stage 5; per-function data in
`docs/phase0-data/nested-captures.csv`. A GCC `-Wtrampolines` build is running
as the authoritative cross-check on the expensive subset.

Applying the sizing below to that split, at 15 minutes for a self-contained
function and an hour for one needing closure conversion:

| | capture | sibling | none | Estimate |
|---|---:|---:|---:|---:|
| glibc | 60 | 3 | 10 | ~64 h (~1.6 weeks) |
| Hurd | 149 | 4 | 26 | ~157 h (~4 weeks) |
| Both | 209 | 7 | 36 | ~221 h (~5.5 weeks) |

Since 209 is a floor, so are these hours.

**This does not by itself trigger the fallback, and it is important to say why.**
The budget is one month of Phase 7 for *the Clang-for-glibc work*, and glibc's
own nested functions come to roughly 1.6 weeks — inside the budget. It is the
combined figure that exceeds a month, and most of that is the **Hurd**, which
is a separate tree and is needed whichever compiler glibc uses (the Decision
section already allows glibc to fall back while the Hurd stays Clang, and the
Hurd's `utils/` — 61 of its 179 — is command-line tools, not servers).

So the nested-function count argues for splitting the question, not for taking
the fallback:

- *glibc nested functions alone:* affordable.
- *Hurd nested functions:* a real cost, but not chargeable to this decision.
- *Still unmeasured:* every other clang error class in glibc. The glibc clang
  build was interrupted by a power outage on 2026-09-23 partway through, and
  `docs/phase0-data/clang-hurd-errors.txt` covers only the 8 Hurd files clang
  reached. **Until that build completes, the decision stays `proposed`.**

### Amendment, 2026-09-24: the compiler cross-check, and a defect in the count

The GCC `-Wtrampolines` build finished. **80 distinct trampolines in the Hurd**
(44 warnings across 8 library directories, 63 across 16 program directories;
utils 39, exec 8, ext2fs 6, storeio 4, eth-multiplexer 3, fatfs 2, sutils 1,
and genuine zeros in trans, proc, term, pflocal and five others). Full detail
in `docs/phase0-data/README.md` stage 6; per-function data in
`docs/phase0-data/trampolines.csv`.

Two results, and the second is the important one.

**The regex under-counts, as predicted.** 70 of the 80 are in the 252-row
inventory, and a trampoline proves capture, so all 70 should have read
`capture`. Sixty-six did; four did not (three `none`, one `sibling`). The
corrected floor is **213 of 252**, and the failure mode is confirmed to be
one-directional — false `none`, never false `capture`.

**The inventory is incomplete.** Ten trampoline functions are absent from the
252 rows entirely, including three in files the inventory lists zero rows for.
`nested.py`'s brace-depth counter desynchronises — measured directly: it
computes depth 0 for a function plainly nested inside `main` — and silently
drops every nested function after the desync point in that file. Two files
contribute zero rows each. **252 is an undercount of unknown size.**

**Effect on this decision.** Both numerator and denominator move up, so the
nested-function conversion is *larger* than the earlier estimate, never
smaller. It does not overturn the conclusion that glibc's own share is
affordable — every one of the four proven misclassifications and all ten
missing functions are in the Hurd, not glibc — but the Hurd-side figure now has
a floor and no established ceiling.

**This blocks nothing in 0002 and everything in the Hurd-side estimate.** The
glibc/GCC fallback question turns on the glibc error surface, which is still
unmeasured because the power outage killed that build. That remains the reason
this decision is `proposed`. But the Hurd conversion estimate in the table
above should not be quoted until `nested.py` is replaced by something that does
not hand-roll brace counting. `trampolines.csv` is the regression test for its
replacement: a correct scanner must find all 80.

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
