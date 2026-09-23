# Design journal

One file per decision, using the template in §15 of the plan. Numbers are
permanent; a superseded decision keeps its number and gains a
`Status: superseded by NNNN` line rather than being edited away.

## Index

| # | Title | Status | Blocks |
|---|---|---|---|
| 0000 | Project name: Joist | accepted | — |
| 0001 | Native IPC model | proposed | Phase 3 |
| 0002 | Clang everywhere, pre-committed GCC fallback for glibc | proposed | Phase 7 |
| 0003 | SMP + no runtime allocation + bounded WCET | proposed | Phase 3 |
| 0004 | Task and thread objects | proposed | — |
| 0005 | Port sets → badge demultiplexing | proposed | Phase 5 |
| 0006 | Out-of-line memory → typed bulk transfer | proposed | Phase 5 |
| 0007 | External memory objects → page-fault IPC | proposed | Phase 6 |
| 0008 | `vm_*` → memory manager RPCs | proposed | Phase 4 |
| 0009 | Exception ports → fault IPC | proposed | Phase 3 |
| 0010 | Thread state and control capabilities | proposed | — |
| 0011 | `fork` / VM inheritance → task create + COW clone | proposed | Phase 4 |
| 0012 | Device interface → IRQ/IO/MMIO/DMA capabilities | proposed | Phase 10 |
| 0013 | Privileged ports → boot-time delegation | proposed | Phase 2 |

Nothing is `accepted` except 0000. Phase 0's exit criterion is that the entries
*exist* and cover §6; accepting them is a separate act that should follow the
Phase 1 host model, which is where several of these get their first real test.

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
  `mach_port_move_member` fires 7 times in an entire build, so runtime
  reconfiguration — the thing that made the row look risky — is not a real
  pattern. No kernel port-set object is needed.
- **0006** keeps out-of-line memory hard, and bounds it: 54 of 140 OOL
  parameters carry `dealloc`, so exactly those need ownership transfer, and
  `io_read` is one of them while `io_write` is not. Three transfer mechanisms
  selected per parameter, not one.
- **0010** found an i386-specific dependency the x86_64 target will not have:
  11 136 GDT calls per build are thread-local storage. On x86_64 that is
  `FS`/`GS` base and the calls have no counterpart. The first place the
  testbed's architecture changed an answer rather than just a number.

## Still open inside these entries

Each entry has its own open items; the ones that block the earliest work:

- 0001 open item 4 (capability space representation) also constrains 0011's
  capability-copy loop. Settle them together.
- 0007's fault-reply read-ahead question and 0008's delegated-range escape
  hatch are both Phase 4 performance decisions that glibc will be written
  against. Neither can wait for Phase 11.
- 0012's device-interface conclusion rests on a boot trace with known coverage
  gaps. Re-check after a real boot trace.
