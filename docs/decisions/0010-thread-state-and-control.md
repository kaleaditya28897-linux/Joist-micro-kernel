# 0010 — `thread_get_state` / `thread_set_state` / `thread_suspend` → thread control capabilities

Date: 2026-09-23
Status: proposed
Covers: §6 row *`thread_get_state` / `thread_set_state` / `thread_suspend`*.

## Context

§6 maps these onto thread control capabilities, used by glibc for signal
delivery, by `proc`, and by debuggers.

**Evidence** (`docs/phase0-data/mach-call-counts.csv`, build workload):
`thread_set_state` 7 265, `thread_get_state` 4 785, `thread_resume` 11 421,
`thread_abort` 2 482, `thread_suspend` **1**, `task_threads` 2 481,
`i386_set_gdt` 7 848, `i386_get_gdt` 3 288.

Three things this says:

1. **`thread_suspend` fires once in an entire build.** The Hurd does not
   suspend threads as a matter of course; `thread_abort` (2 482) is the
   primitive it actually leans on, to interrupt an in-progress RPC.
2. `set_state` exceeding `get_state` by 1.5× is signal delivery: read the
   state once, write it back to enter the handler, write it again to return.
3. **`i386_set_gdt` at 7 848 is thread-local storage**, not scheduling. On
   i386 the Hurd installs a GDT entry per thread for TLS. This is an
   architecture-specific call that has no x86_64 equivalent — x86_64 uses
   `FS`/`GS` base registers — and the testbed being i386 is what made it
   visible. See Consequences.

## Decision

- A **thread control capability** authorises: read register state, write
  register state, abort an in-progress IPC, suspend, resume, terminate. It is
  a capability, so holding it is the authority — there is no ambient "am I
  allowed to poke this thread" check.
- It is handed out in exactly three ways: to the creator of a thread, to a
  fault handler for the faulting thread (0009), and by explicit delegation
  (how a debugger gets one, via `proc`).
- **Register state is an architecture-specific fixed-size record.** The kernel
  exposes exactly one state flavour per architecture, not Mach's family of
  flavours. A fixed record is bounded and needs no kernel-side dispatch.
- **`abort` is specified as part of the IPC contract** (0001), because what it
  means is "cancel this thread's in-progress `Call` and make it return
  `EINTR`". Defining it as a thread operation independently of IPC is how it
  ends up with unclear semantics.
- **TLS is a thread attribute, not a GDT call.** The kernel provides a
  per-thread TLS base register as part of the thread's state record, set at
  create time and via `set_state`. `i386_set_gdt`/`i386_get_gdt` do not map
  across; they are replaced, not ported.

## Consequences

- Easier: one capability covers the whole debug/signal surface, and the fixed
  state record keeps the kernel's arch interface small.
- Harder: glibc's TLS setup must be rewritten rather than adapted — it is
  currently written against the i386 GDT calls. On x86_64 this is simpler than
  what it replaces, but it is new code in the riskiest phase (Phase 7).
- **An i386-specific finding on an x86_64-targeted project.** 0002 and the
  testbed document already note the testbed is i386 for availability reasons;
  this row is the first concrete place where the architecture difference
  changes an answer rather than just the numbers. When the x86_64 testbed
  exists, re-run the trace and check what replaces the 11 136 GDT calls.
- `thread_suspend` firing once means it can be implemented simply and need not
  be fast. Do not optimise it.

## Priority check

Respects the order. Dropping Mach's multiple state flavours costs Reuse (glibc
and `proc` need edits) and buys a bounded, fixed-size kernel interface, which
is Reliability.
