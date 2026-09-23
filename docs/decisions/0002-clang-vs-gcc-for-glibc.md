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

Not yet performed. It requires Debian GNU/Hurd sources and a working build
environment; the testbed VM for this is being provisioned (see
`docs/testbed.md`). Record the result here as an amendment, and update this
file's Status to `accepted` with whichever branch was taken.

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
