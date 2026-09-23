# Mach dependency inventory

Phase 0 deliverable (§11). Every Mach call the Hurd actually uses, which
component uses it, and the planned mapping onto Joist's native API (§6).

**Status:** dynamic side collected 2026-09-23 (partial — see *Coverage*).
Static side (`.defs` read-through) not started. Decisions not yet written.

## Why this exists

The plan's central strategy is *Mach-shaped, not Mach-implemented* (§4.2): the
kernel exposes a small native API, and every Mach concept the Hurd uses is
mapped onto it in userspace. That is only safe if we know what the Hurd
actually uses — as opposed to what Mach offers. Mach's interface is large; the
subset the Hurd depends on is much smaller, and the difference is the work we
get to skip.

Phase 0's exit criterion is a decision-log entry for every row in §6. This
file is the evidence behind those entries.

## Method

1. **Static side.** Read the `.defs` files under `gnumach/include/mach/` and
   mark every call *used / unused / used-by-one-thing*. Cross-reference against
   the Hurd tree with `grep` for the generated stub names.
2. **Dynamic side.** Run `rpctrace` on three real workloads — boot to a login
   prompt, an interactive shell session, and a package build — and record which
   calls actually fire and how often. Frequency matters: it orders the Phase 5
   MIG work and the Phase 11 fast-path work.
3. **Reconcile.** A call that is statically referenced but never fires is a
   different risk from one that fires thousands of times per second.

## Table format

| Mach call | Used by | Static / dynamic | Count (boot / shell / build) | Joist mapping | Decision |
|---|---|---|---|---|---|

`Joist mapping` references §6 of the plan. `Decision` is the
`docs/decisions/NNNN` entry that settles it, or `trivial` where §6's row needs
no elaboration.

## Dynamic results (rpctrace, 2026-09-23)

Collected on the testbed (Debian GNU/Hurd i386, GNU Mach 1.8+git20260224) with
`tools/phase0/workloads.sh`, tallied with `tools/phase0/tally.py`. Raw logs are
on the VM in `~demo/trace/` (`build.txt` is 132 MB and is not committed).

Only calls defined in `/usr/share/msgids/gnumach.msgids`, i.e. the kernel's
own interfaces, are counted. Hurd server RPCs (`io_*`, `dir_*`, `proc_*`, …)
are excluded.

### Coverage — read this before trusting the zeros

- **rpctrace deadlocks on `gsync_wait`.** It forwards the kernel RPC
  synchronously, so once a traced thread blocks in `gsync_wait`, the
  `gsync_wake` that would release it never gets through. Every workload hit
  this. Hence the partial coverage:
  - **boot** is a *proxy*. rpctrace cannot follow the real boot; it traces a
    task it starts. The workload is: start a fresh `ext2fs` translator on a
    64 MB image, use it, tear it down. It hung during ext2fs startup
    (`paging_requests` thread), so only translator initialisation is covered.
  - **shell**: one traced `bash` session hung, both `-i` and non-interactive.
    Each command was traced as its own process instead. 3 of 10 ran to
    completion; the other 7 are covered up to the hang.
  - **build** (`dpkg-buildpackage -b` of `hello` 2.12.3) covers setup,
    `dh_clean` and the whole `./configure` (≈3000 `task_create`s). It hung at
    the first `make`, so **compilation and linking are not covered**.
- **Only the traced tasks' calls are visible.** Calls made by servers outside
  the trace are not: the root ext2fs, `exec`, `proc`, `auth`, the default
  pager, and the kernel's upcalls to them. That is why `memory_object_*`,
  `mach_notify_*` and `exception_raise` all read 0. **A zero is not evidence of
  non-use.** Those rows must come from the static side, or from tracing a
  translator directly.
- **Traps are invisible.** `mach_msg`, `mach_reply_port`, `mach_task_self`,
  `mach_thread_self`, `mach_host_self`, `swtch`, `swtch_pri`, `thread_switch`
  and `syscall_thread_switch` are syscalls, not RPCs. rpctrace never sees them,
  yet `mach_msg` is under every line of this table. Add them to the inventory by
  hand from `mach/syscall_sw.h`.
- OOL descriptors are not annotated by rpctrace, so this data says nothing
  about the OOL → grant risk.

### Observed calls

Count columns are calls seen in `boot` / `shell` / `build`. *Used by* is
inferred from call patterns and must be confirmed statically.

| Mach call | Used by | Static / dynamic | Count (boot / shell / build) | Joist mapping | Decision |
|---|---|---|---|---|---|
| `mach_port_deallocate` | everything (glibc, libports, MIG stubs) | dynamic | 141 / 40 / 343547 | capability drop (§6 send right) | — |
| `mach_port_insert_right` | glibc `fork`, libports | dynamic | 41 / 4 / 57434 | capability copy/mint | — |
| `mach_port_get_refs` | glibc `fork` | dynamic | 14 / 0 / 45745 | none — user refcounts are a Mach artifact; libports-local | — |
| `mach_port_mod_refs` | glibc, libports | dynamic | 30 / 5 / 26600 | none — libports-local refcount | — |
| `mach_port_allocate_name` | glibc `fork` (child keeps parent's names) | dynamic | 8 / 0 / 20686 | **caller-chosen capability slot** — not in §6 | — |
| `mach_port_allocate` | libports, glibc | dynamic | 36 / 5 / 16473 | endpoint create (§6 receive right) | — |
| `mach_port_set_qlimit` | libports / glibc | dynamic | 22 / 3 / 9387 | **no equivalent** in synchronous IPC — not in §6 | — |
| `mach_port_extract_right` | glibc `fork` | dynamic | 3 / 0 / 7443 | capability copy into another task's space | — |
| `mach_port_destroy` | glibc | dynamic | 0 / 0 / 5803 | endpoint destroy | — |
| `mach_port_names` | glibc `fork` (walks the whole namespace) | dynamic | 2 / 0 / 4199 | **capability-space enumeration** — not in §6 | — |
| `mach_port_move_member` | libports (port sets) | dynamic | 3 / 0 / 7 | §6 port set row — rare but present, confirms libports moves ports at runtime | — |
| `mach_port_set_protected_payload` | libports | dynamic | 3 / 0 / 0 | **badges** — the Hurd already demultiplexes by payload; strengthens the §6 port-set mapping | — |
| `mach_port_request_notification` | libports | dynamic | 2 / 0 / 0 | native lifetime notifications (§5.4) | — |
| `vm_deallocate` | glibc, MIG (OOL) | dynamic | 49 / 8 / 55580 | memory manager RPC | — |
| `vm_map` | glibc `ld.so`/`mmap`, exec | dynamic | 137 / 19 / 54240 | memory manager RPC | — |
| `vm_protect` | `ld.so` (RELRO), glibc | dynamic | 57 / 10 / 30138 | memory manager RPC | — |
| `vm_allocate` | glibc `malloc`, stacks | dynamic | 30 / 9 / 11957 | memory manager RPC | — |
| `vm_region` | glibc `fork` | dynamic | 3 / 1 / 2302 | memory manager RPC (address-space walk) | — |
| `vm_get_size_limit` | glibc (`getrlimit`) | dynamic | 3 / 1 / 2302 | memory manager RPC — GNU Mach extension | — |
| `vm_statistics` | `ps`/`free`-style tools | dynamic | 0 / 0 / 4 | memory manager query | — |
| `thread_resume` | glibc `fork`, pthreads | dynamic | 18 / 1 / 11421 | thread control capability | — |
| `thread_create` | glibc `fork`, pthreads, signal thread | dynamic | 17 / 1 / 7264 | Thread | — |
| `thread_set_state` | glibc `fork`, pthreads, signals | dynamic | 17 / 1 / 7265 | thread control capability | — |
| `thread_get_state` | glibc `fork`, signals | dynamic | 16 / 1 / 4785 | thread control capability | — |
| `thread_set_special_port` | glibc (per-thread exception port, presumably) | dynamic | 4 / 1 / 4783 | fault IPC handler registration | — |
| `thread_abort` | glibc signal delivery | dynamic | 1 / 0 / 2482 | thread control capability — **not in §6** | — |
| `thread_suspend` | glibc | dynamic | 0 / 0 / 1 | thread control capability | — |
| `task_info` | glibc / proc queries | dynamic | 0 / 0 / 9243 | task query | — |
| `task_set_special_port` | glibc `fork`, exec | dynamic | 7 / 2 / 7085 | bootstrap/exception capability handoff | — |
| `task_create` | glibc `fork` | dynamic | 2 / 0 / 3064 | task create + COW clone (§6) | — |
| `task_terminate` | glibc `_exit`/exec | dynamic | 0 / 0 / 3059 | task destroy | — |
| `task_threads` | glibc `fork` | dynamic | 1 / 0 / 2481 | thread enumeration — not in §6 | — |
| `task_get_special_port` | glibc startup | dynamic | 4 / 1 / 2302 | bootstrap capability | — |
| `task_priority` | ext2fs startup | dynamic | 1 / 0 / 0 | scheduling | — |
| `i386_set_gdt` | glibc TLS setup | dynamic | 19 / 2 / 7848 | **not in §6** — i386 TLS via GDT; x86_64 uses FS base, so the mapping differs by arch | — |
| `i386_get_gdt` | glibc `fork` | dynamic | 2 / 0 / 3288 | as above | — |
| `gsync_wait` | glibc pthreads/locks | dynamic | 1 / 1 / 1 | **futex — not in §6** | — |
| `thread_set_name` | ext2fs (libpager threads) | dynamic | 2 / 0 / 0 | debug only | — |
| `device_pager_init_pager` | ext2fs startup (unexplained) | dynamic | 1 / 0 / 0 | device interface — investigate | — |

Totals: 697 / 116 / 770 219 Mach kernel RPCs. 39 of the 197 calls in
`gnumach.msgids` were observed; the 158 that were not are listed by `tally.py`.
Of those, `mach_host` (processor sets, host time), `mach_debug`, `mach4`
(PC sampling, proxies) and the emulation-vector calls look like the best
candidates for *unused*. They still need the static check.

### What the data says so far

1. **`fork` dominates everything.** The call ratios per `task_create` (≈1.4
   `mach_port_names`, ≈0.75 `vm_region`, ≈0.8 `task_threads`, ≈7
   `mach_port_allocate_name`, ≈2.4 `mach_port_extract_right`) show glibc's
   `fork` walking the parent's whole port namespace and re-creating it name-for-name
   in the child. Joist's COW `fork` (§6 `task_create` row) therefore also needs
   **capability-space enumeration and caller-chosen slot allocation**, or a
   kernel/manager-side "clone capability space" operation. This is the
   biggest §6 gap the data shows.
2. **Four Mach features in use have no §6 row:** `gsync_*` (futex),
   `i386_set_gdt` (TLS), `mach_port_set_qlimit` (queue limits — meaningless
   under synchronous IPC; decide whether to drop), and `thread_abort`
   (interrupting a thread blocked in an RPC — signal delivery relies on it,
   and it interacts with §5's call/reply semantics).
3. **`mach_port_set_protected_payload` is already in use.** libports already
   demultiplexes by payload rather than purely by port set, which makes the
   port-set → badge mapping look cheaper than §6 feared.
   `mach_port_move_member` is also used, though rarely, so the "move between
   sets at runtime" concern stands.
4. **VM traffic is `vm_map`/`vm_deallocate`/`vm_protect`-heavy** (~140k in the
   build), all per process start. This orders the Phase 11 fast-path work:
   process creation, not steady-state IPC, is where Mach calls concentrate.

### Next steps for this file

- Static pass over `/usr/include/mach/*.defs` and the Hurd tree, for the
  known-hard rows below, the invisible traps and every zero above.
- Trace a translator directly (`rpctrace` as the translator command in
  `settrans`) to get `memory_object_*` and notification traffic.
- Work around the `gsync_wait` deadlock (patch rpctrace to forward `gsync_*`
  on a separate thread, or trace a subhurd), then re-run the build through
  compile and link.

## Known-hard rows, flagged in advance

These are already known not to be one-line swaps (§6), and should be filled in
first because they size Phase 5:

- **Out-of-line memory → shared memory grant.** A semantics change, not a
  mechanism swap: Mach OOL is copy-on-write with sender-side deallocation. Every
  use site must be examined for whether it depended on copy semantics or merely
  on moving a large buffer cheaply. Expect multi-week work.
- **Port sets → badges on one endpoint.** libports' per-port handlers and its
  ability to move a port between sets at runtime are not obviously the same
  shape as badge demultiplexing. Confirm against real libports usage.
- **`memory_object_*` (external pagers) → page-fault IPC.** Drives the Phase 6
  paging protocol and the libpager rewrite.
- **Exception ports → fault IPC.** Drives glibc signal delivery.

## Open questions

1. Which `.defs` calls are referenced only by `mach-defpager` (which is being
   rewritten anyway) and can therefore be ignored entirely?
2. Which are referenced only by debugging tools rather than the running system?
3. What does the Hurd use that is *not* in the `.defs` files — direct syscalls,
   `mach_msg` idioms, assumptions about message layout?
