# 0010 — `thread_get_state` / `thread_set_state` / `thread_suspend` → thread control capabilities

Date: 2026-09-23
Status: accepted (2026-10-01)
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
  currently written against the i386 GDT calls. *(Corrected 2026-09-30: true on
  i386 only; the x86_64 port already sets TLS through `set_state`. See the
  amendment.)* On x86_64 this is simpler than
  what it replaces, but it is new code in the riskiest phase (Phase 7).
- **An i386-specific finding on an x86_64-targeted project.** 0002 and the
  testbed document already note the testbed is i386 for availability reasons;
  this row is the first concrete place where the architecture difference
  changes an answer rather than just the numbers. When the x86_64 testbed
  exists, re-run the trace and check what replaces the 11 136 GDT calls.
- `thread_suspend` firing once means it can be implemented simply and need not
  be fast. Do not optimise it. *(Withdrawn 2026-10-01: 734 in the full build,
  all on the signal-delivery path. See the amendment.)*

## Amendment, 2026-09-30: the x86_64 re-run

The trace was re-run on the x86_64 testbed (VM 111) with the same workloads;
`docs/phase0-data/amd64/i386-vs-amd64.csv` has every call side by side, and
`docs/phase0-data/README.md` stage 8 has the provenance. Build workload:

| | i386 | x86_64 |
|---|---:|---:|
| `i386_set_gdt` + `i386_get_gdt` | 11 136 | **0** |
| `thread_set_state` | 7 265 | **15 056** |
| `thread_get_state` | 4 785 | 4 945 |
| `task_threads` (≈ one per fork) | 2 481 | 2 471 |

By flavour on x86_64 (`<mach/x86_64/thread_status.h>`), counted from the
trace arguments:

| Flavour | set | get | What it is |
|---|---:|---:|---|
| 1 `i386_THREAD_STATE` | 2 292 | 0 | full registers, once per process start (= `task_get_special_port`, 2 292) |
| 5 `i386_REGS_SEGS_STATE` | 4 943 | 2 472 | registers + segments: signal delivery and fork |
| 7 `i386_FSGS_BASE_STATE` | **7 821** | **2 472** | TLS base |
| 8 `i386_XFLOAT_STATE` | 0 | 1 | extended FP |

**What replaced the GDT calls:** flavour 7. Its 7 821 sets match the 7 848
`i386_set_gdt` calls to within 0.4%, and its 2 472 gets are one per thread
copied by `fork` (2 471 `task_threads`). glibc's x86_64 port already does
exactly this, in `sysdeps/mach/hurd/x86_64/htl/pt-setup.c:87` and
`htl/pt-machdep.c:66,72`.

This changes two things in this entry:

1. **The Consequences bullet saying glibc's TLS setup "must be rewritten" is
   wrong for x86_64.** The x86_64 port already treats TLS as thread state set
   through `set_state`, which is what the Decision section asks for. The
   rewrite is needed only if Joist ever targets i386, which the plan does not.
2. **"Exactly one state flavour per architecture" has a cost the Decision did
   not state.** glibc writes three flavours and reads two. The TLS base alone
   is 7 821 writes per build, each of which today touches only two registers.
   With a single fixed record, each becomes a whole-record write, or a
   read-modify-write if the caller doesn't hold the rest of the state. Two
   ways to keep the single-record rule without that cost, to be settled
   before Phase 7:
   - make the TLS base a separately settable thread attribute (`set_tls_base`),
     which is honest about it being the hot path; or
   - enable `FSGSBASE` (`CR4.FSGSBASE`) so threads set their own base with
     `wrfsbase` and **no kernel call at all**. That removes ~7.8k calls per build,
     but it makes the base register user-controlled state that the kernel has
     to save and restore on context switch (it already has to, for
     `set_state`).

Status stays `proposed`.

## Amendment, 2026-10-01 (later): `thread_suspend` is on the signal path

The "fires once" figure above came from a trace that stopped at the first
`make`, because of rpctrace's `gsync_wait` deadlock. With that fixed
(`docs/phase0-data/README.md` stage 14), the full build shows
**`thread_suspend` 734**, `thread_abort` 4 146, `thread_get_state` 8 494 and
`thread_set_state` 22 695. All 734 suspends come from shells (`/bin/bash` 419,
`/bin/sh` 315), across 212 processes, and every one of those processes also
aborts. That's glibc's signal delivery: suspend the target thread, abort its
in-flight RPC, rewrite its state, resume. `SIGCHLD` drives it on every child
exit.

So the Consequences bullet "`thread_suspend` firing once means it can be
implemented simply and need not be fast. Do not optimise it" is **withdrawn**.
It's still modest (about 0.16 per `task_create`), and simple is still right,
but it sits on the signal-delivery path. The thing worth designing is that
path as a whole: suspend → abort → get/set state → resume is one operation
from glibc's point of view, and four kernel calls today.

## Accepted, 2026-10-01

Accepted, including the 2026-10-01 amendments. One choice inside it stays
open and must be settled before Phase 7: how the TLS base is set (a separate
`set_tls_base` call, or `FSGSBASE` with no kernel call).

## Priority check

Respects the order. Dropping Mach's multiple state flavours costs Reuse (glibc
and `proc` need edits) and buys a bounded, fixed-size kernel interface, which
is Reliability.
