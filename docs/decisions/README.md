# Design journal

One file per decision, using the template in §15 of the plan. Numbers are
permanent; a superseded decision keeps its number and gains a
`Status: superseded by NNNN` line rather than being edited away.

## Index

| # | Title | Status | Blocks |
|---|---|---|---|
| 0000 | Project name: Joist | accepted | — |
| 0001 | Native IPC model | accepted; open items settled by the 2026-10-03 amendment | Phase 3 |
| 0002 | Clang everywhere, pre-committed GCC fallback for glibc | accepted (Clang branch) | Phase 7 |
| 0003 | SMP + no runtime allocation + bounded WCET | accepted | Phase 3 |
| 0004 | Task and thread objects | accepted; amended 2026-10-05 (parent authority) | — |
| 0005 | Port sets → badge demultiplexing | accepted | Phase 5 |
| 0006 | Out-of-line memory → typed bulk transfer | accepted | Phase 5 |
| 0007 | External memory objects → page-fault IPC | accepted | Phase 6 |
| 0008 | `vm_*` → memory manager RPCs | accepted | Phase 4 |
| 0009 | Exception ports → fault IPC | accepted | Phase 3 |
| 0010 | Thread state and control capabilities | accepted | — |
| 0011 | `fork` / VM inheritance → task create + COW clone | accepted | Phase 4 |
| 0012 | Device interface → IRQ/IO/MMIO/DMA capabilities | accepted | Phase 10 |
| 0013 | Privileged ports → boot-time delegation | accepted | Phase 2 |
| 0014 | Mach features outside §6: futexes, yield, queue limits | accepted | Phase 3 (yield), Phase 7 (futexes) |
| 0015 | Boot protocol: Limine | accepted | Phase 2 |
| 0016 | Kernel concurrency: one lock, interrupts off, preemption points | accepted | Phase 3 |
| 0017 | The kernel lock revisited: one lock kept, the timer written outside it | accepted | Phase 3 |
| 0018 | One CPU woken for each thread, in a fixed order | accepted | Phase 3 |
| 0019 | The contended line is a worst case, reported, not held to the stability test | accepted | Phase 3 |
| 0020 | Page-sized mappings: part of a frame, the page tables as the record | accepted | Phase 4 |
| 0021 | Page tables paid for by memory the memory manager donates | accepted | Phase 4 |
| 0022 | Revoke by killing the object; the capability copy for `fork` in batches | accepted | Phase 4 |
| 0023 | `mm`: how it runs (passive), how tasks reach it, what it does | accepted, amended 2026-10-28 (lazy no-senders) and 2026-10-29 (SHARE by both sides' consent; a fork confirmed, an orphan going with its parent); corrected 2026-10-29 (`MM_SLOT` is skipped by a clone's `cap_copy_batch`, not a stop); amended 2026-10-30 (no per-client limit until the process server, Phase 7) | Phase 4 |
| 0024 | `mm`: guard pages (`MM_NONE`), and what a file-backed region changes | accepted 2026-10-29 (`MM_NONE` keeps a page's contents; a helper thread for file servers) | Phase 4 (guard pages), Phase 6 (files) |
| 0025 | A fallback scheduling context for passive servers (`mm` stranded by a client killed mid-request) | accepted | Phase 4 |
| 0026 | Toolchain trust: reproducible builds, a full-source bootstrap, and diverse double-compiling | accepted | all phases (toolchain) |

**All entries were accepted on 2026-10-01** by the project owner, at the close
of Phase 0 (`docs/phase0-exit.md`). The recommendations taken were: 0002 on
the Clang branch (with two recorded conditions), 0005 with readiness
notifications for `select`, and 0014 as written. Each still has its own open
items (below). Under the journal's rules, a later change of mind is a new,
superseding entry, not an edit. The Phase 1 host model is where several of
these get their first real test.

Later entries and changes, each accepted by the project owner: the
amendment to 0001 that settles its five parameters (2026-10-03, Phase 1
work item 6), the amendment to 0004 on parent authority over descendants
(2026-10-05), and **0015** (2026-10-05, at the start of Phase 2).

## §6 coverage

Phase 0's exit criterion requires a decision entry for every row of §6's
Mach-to-Joist mapping table. All 16 rows:

| §6 row | Decision |
|---|---|
| Task | 0004 |
| Thread | 0004 |
| Port receive right | 0001 |
| Port send right | 0001 |
| Send-once right | 0001 |
| Port set | 0005 |
| `mach_msg` RPC | 0001 |
| Out-of-line memory | 0006 |
| No-senders / dead-name notification | 0001 |
| External memory objects (`memory_object_*`) | 0007 |
| `vm_allocate`, `vm_map`, `vm_protect` | 0008 |
| Exception ports | 0009 |
| `thread_get_state` / `set_state` / `suspend` | 0010 |
| `task_create` with VM inheritance (`fork`) | 0011 |
| Device interface (`device_open`, …) | 0012 |
| `host_priv`, `device_master` | 0013 |

## What the measurements changed

Three entries reached a conclusion the plan did not anticipate, which is the
main argument for having done Phase 0 before Phase 1:

- **0005** downgrades port sets from a "known-hard" row to an ordinary one.
  `mach_port_move_member` fires 41 times in an entire build (7 in the
  configure-only trace first quoted), so runtime reconfiguration, the thing
  that made the row look risky, is not a real pattern. No kernel port-set
  object is needed. **But** client-side `select` waits on several replies
  through a port set, which badges don't cover; the 2026-10-01 amendment
  recommends a readiness-notification protocol.
- **0006** keeps out-of-line memory hard, and bounds it: 54 of 140 OOL
  parameters carry `dealloc`, so exactly those need ownership transfer, and
  `io_read` is one of them while `io_write` is not. Three transfer mechanisms
  selected per parameter, not one.
- **0010** found that 11 136 GDT calls per build on i386 are thread-local
  storage. The x86_64 re-run shows what replaces them: `thread_set_state`
  flavour `i386_FSGS_BASE_STATE` (7 821 sets, 2 472 gets). glibc's x86_64
  port already treats TLS as thread state. Its "`thread_suspend` fires once"
  was withdrawn: 734 per full build, all on shells' signal-delivery path.

- **0014** covers what §6 never listed. Futexes (698 waits per build) go
  into glibc on top of notification objects, not into the kernel; yield is
  one `Yield` call; queue limits disappear (coalescing → notifications,
  packet buffering → 0012's driver rings).

## Still open inside these entries

Each entry has its own open items; the ones that block the earliest work:

- ~~0001 open items 1–5~~: settled by the 2026-10-03 amendment (accepted),
  from the Phase 1 model and new measurements. Item 4's two-level table is
  copied leaf by leaf, which is what 0011's capability-copy loop needs. The
  amendment also replaces bare `Send` with deferred replies.
- 0007's fault-reply read-ahead question and 0008's delegated-range escape
  hatch are both Phase 4 performance decisions that glibc will be written
  against. Neither can wait for Phase 11.
- 0012's device-interface conclusion rests on a boot trace with known coverage
  gaps. Re-check after a real boot trace. 0014 adds the network packet path
  (today a deep Mach message queue) to its design items.
- ~~0014's yield count~~: measured 2026-10-01. One start-up `swtch_pri` per
  process, nothing else. Only long-running servers' `thread_switch` is
  unmeasured.
