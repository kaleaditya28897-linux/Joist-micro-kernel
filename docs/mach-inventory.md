# Mach dependency inventory

Phase 0 deliverable (§11). Every Mach call the Hurd actually uses, which
component uses it, and the planned mapping onto Joist's native API (§6).

**Status (2026-09-23):** dynamic side collected (partial; see *Coverage*).
Static cross-reference done by `grep`, not yet by reading every call site.
Decisions not yet written. All numbers come from `docs/phase0-data/`; see its
README for provenance and how to reproduce each one.

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

Every number below is in `phase0-data/mach-call-counts.csv` (all 199 calls,
zeros included). The count and static columns of the table are generated from
the CSVs, not typed in.

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
  `swtch` and the rest are syscalls, not RPCs. Worse, the hottest calls in this
  table are normally traps too (glibc's `syscall_*` shortcuts), and appear as
  RPCs only because rpctrace's proxy ports force the fallback. See *Traps*
  below.
- OOL descriptors are not annotated by rpctrace, so this data says nothing
  about the OOL → grant risk.

### Observed calls

Count columns are calls seen in `boot` / `shell` / `build`. *Used by* is
inferred from call patterns and must be confirmed statically.

| Mach call | Used by | Static / dynamic | Count (boot / shell / build) | Joist mapping | Decision |
|---|---|---|---|---|---|
| `mach_port_deallocate` | everything (glibc, libports, MIG stubs) | both (53 calling components) | 141 / 1464 / 343547 | capability drop (§6 send right) | — |
| `mach_port_insert_right` | glibc `fork`, libports | both (21 calling components) | 41 / 341 / 57434 | capability copy/mint | — |
| `mach_port_get_refs` | glibc `fork` | both (4 calling components) | 14 / 234 / 45745 | none — user refcounts are a Mach artifact; libports-local | — |
| `mach_port_mod_refs` | glibc, libports | both (21 calling components) | 30 / 139 / 26600 | none — libports-local refcount | — |
| `mach_port_allocate_name` | glibc `fork` (child keeps parent's names) | both (1 calling component) | 8 / 121 / 20686 | **caller-chosen capability slot** — not in §6 | — |
| `mach_port_allocate` | libports, glibc | both (19 calling components) | 36 / 145 / 16473 | endpoint create (§6 receive right) | — |
| `mach_port_set_qlimit` | libports / glibc | both (6 calling components) | 22 / 84 / 9387 | **no equivalent** in synchronous IPC — not in §6 | — |
| `mach_port_extract_right` | glibc `fork` | both (4 calling components) | 3 / 45 / 7443 | capability copy into another task's space | — |
| `mach_port_destroy` | glibc | both (18 calling components) | 0 / 1 / 5803 | endpoint destroy | — |
| `mach_port_names` | glibc `fork` (walks the whole namespace) | both (5 calling components) | 2 / 29 / 4199 | **capability-space enumeration** — not in §6 | — |
| `mach_port_move_member` | libports (port sets) | both (10 calling components) | 3 / 0 / 7 | §6 port set row — rare but present, confirms libports moves ports at runtime | — |
| `mach_port_set_protected_payload` | libports | both (2 calling components) | 3 / 0 / 0 | **badges** — the Hurd already demultiplexes by payload; strengthens the §6 port-set mapping | — |
| `mach_port_request_notification` | libports | both (10 calling components) | 2 / 0 / 0 | native lifetime notifications (§5.4) | — |
| `vm_deallocate` | glibc, MIG (OOL) | both (25 calling components) | 49 / 241 / 55580 | memory manager RPC | — |
| `vm_map` | glibc `ld.so`/`mmap`, exec | both (13 calling components) | 137 / 381 / 54240 | memory manager RPC | — |
| `vm_protect` | `ld.so` (RELRO), glibc | both (6 calling components) | 57 / 213 / 30138 | memory manager RPC | — |
| `vm_allocate` | glibc `malloc`, stacks | both (20 calling components) | 30 / 171 / 11957 | memory manager RPC | — |
| `vm_region` | glibc `fork` | both (6 calling components) | 3 / 23 / 2302 | memory manager RPC (address-space walk) | — |
| `vm_get_size_limit` | glibc (`getrlimit`) | both (1 calling component) | 3 / 23 / 2302 | memory manager RPC — GNU Mach extension | — |
| `vm_statistics` | `ps`/`free`-style tools | both (6 calling components) | 0 / 1 / 4 | memory manager query | — |
| `thread_resume` | glibc `fork`, pthreads | both (7 calling components) | 18 / 74 / 11421 | thread control capability | — |
| `thread_create` | glibc `fork`, pthreads, signal thread | both (7 calling components) | 17 / 53 / 7264 | Thread | — |
| `thread_set_state` | glibc `fork`, pthreads, signals | both (5 calling components) | 17 / 59 / 7265 | thread control capability | — |
| `thread_get_state` | glibc `fork`, signals | both (6 calling components) | 16 / 50 / 4785 | thread control capability | — |
| `thread_set_special_port` | glibc (per-thread exception port, presumably) | both (1 calling component) | 4 / 38 / 4783 | fault IPC handler registration | — |
| `thread_abort` | glibc signal delivery | both (4 calling components) | 1 / 21 / 2482 | thread control capability — **not in §6** | — |
| `thread_suspend` | glibc | both (5 calling components) | 0 / 13 / 1 | thread control capability | — |
| `task_info` | glibc / proc queries | both (3 calling components) | 0 / 0 / 9243 | task query | — |
| `task_set_special_port` | glibc `fork`, exec | both (6 calling components) | 7 / 61 / 7085 | bootstrap/exception capability handoff | — |
| `task_create` | glibc `fork` | both (7 calling components) | 2 / 15 / 3064 | task create + COW clone (§6) | — |
| `task_terminate` | glibc `_exit`/exec | both (8 calling components) | 0 / 17 / 3059 | task destroy | — |
| `task_threads` | glibc `fork` | both (6 calling components) | 1 / 15 / 2481 | thread enumeration — not in §6 | — |
| `task_get_special_port` | glibc startup | both (1 calling component) | 4 / 23 / 2302 | bootstrap capability | — |
| `task_priority` | ext2fs startup | both (3 calling components) | 1 / 0 / 0 | scheduling | — |
| `i386_set_gdt` | glibc TLS setup | both (1 calling component) | 19 / 63 / 7848 | **not in §6** — i386 TLS via GDT; x86_64 uses FS base, so the mapping differs by arch | — |
| `i386_get_gdt` | glibc `fork` | both (1 calling component) | 2 / 40 / 3288 | as above | — |
| `gsync_wait` | glibc pthreads/locks | both (3 calling components) | 1 / 13 / 1 | **futex — not in §6** | — |
| `gsync_wake` | glibc pthreads/locks | both (4 calling components) | 0 / 12 / 0 | futex wake — pairs with `gsync_wait` | — |
| `thread_set_name` | ext2fs (libpager threads) | both (1 calling component) | 2 / 0 / 0 | debug only | — |
| `memory_object_init` | kernel → pager (ext2fs's libpager). rpctrace printed it as `device_pager_init_pager`, which shares msgid 2200 | both (implemented in hurd:libpager) | 1 / 0 / 0 | page-fault IPC setup (§6 external memory objects) | — |

Totals: 697 / 4223 / 770 219 Mach kernel RPCs. 40 of the 199 calls in
`gnumach.msgids` were observed; every call  zeros included  is in
`phase0-data/mach-call-counts.csv`.
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

- Read the call sites behind the known-hard rows below (grep finds them; it
  does not tell you what they depend on).
- Trace a translator directly (`rpctrace` as the translator command in
  `settrans`) to get `memory_object_*` and notification traffic.
- Work around the `gsync_wait` deadlock (patch rpctrace to forward `gsync_*`
  on a separate thread, or trace a subhurd), then re-run the build through
  compile and link.

## x86_64 re-run (2026-09-30)

Everything above was measured on i386. The same workloads were re-run on the
x86_64 testbed; `phase0-data/amd64/i386-vs-amd64.csv` has every call side by
side (provenance in `phase0-data/README.md` stage 8). The profile is
architecture-independent except for thread-local storage: the 11 136
`i386_set_gdt`/`i386_get_gdt` calls become `thread_set_state`/`get_state`
with flavour `i386_FSGS_BASE_STATE` (7 821 sets, 2 472 gets). See decision
0010's amendment. On x86_64 the boot proxy also captured live pager traffic
(`memory_object_data_supply`, `memory_object_lock_request`,
`memory_object_ready`) before rpctrace crashed.

## Traps (syscalls, invisible to rpctrace)

From `/usr/include/i386-gnu/mach/syscall_sw.h` (sha256 `b03bf2c2…d6acc`) and
GNU Mach's trap table (`kern/syscall_sw.c`, gnumach 1.8+git20260805 source).
Per-trap data is in `phase0-data/traps.csv`, produced by
`tools/phase0/traps.py`.

**The header declares 47 traps; GNU Mach implements 24 of them.** The other
23 are CMU-era leftovers with no entry in the kernel's trap table (`nw_*`,
`task_by_pid`, `pid_by_task`, `init_process`, `map_fd`, `rfs_make_symlink`,
`htg_syscall`, `set_ras_address`). Joist can ignore them. The table also has
two traps the header does not declare, `syscall_device_write_request` and
`syscall_device_writev_request`; glibc excludes both from its shortcuts.

**The `syscall_*` traps are the hot path, and rpctrace hides that.** glibc's
`mach/Makefile` generates a *shortcut* wrapper (`mach/shortcut.awk`) for every
`syscall_X` trap. `X()` calls the trap first and falls back to the MIG RPC
`X_rpc()` only if the trap returns `MACH_SEND_INTERRUPTED`. The kernel returns
that only when the target port is not a kernel object (`kern/ipc_mig.c`,
comment above `syscall_vm_map`). Under rpctrace, the task port *is* a proxy,
so every call falls back and gets counted. **The counts above are right; the
mechanism is not.** Outside rpctrace, the table's hottest calls are single
traps, not message round trips.

| Trap | # | Used by | Count (boot / shell / build) |
|---|---:|---|---|
| `evc_wait` | -17 | 2 components | invisible to rpctrace |
| `evc_wait_clear` | -18 | no caller found | invisible to rpctrace |
| `mach_msg_trap` | -25 | 2 components | invisible to rpctrace |
| `mach_reply_port` | -26 | 17 components | invisible to rpctrace |
| `mach_thread_self` | -27 | 14 components | invisible to rpctrace |
| `mach_task_self` | -28 | 75 components | invisible to rpctrace |
| `mach_host_self` | -29 | 15 components | invisible to rpctrace |
| `mach_print` | -30 | 8 components | invisible to rpctrace |
| `swtch_pri` | -59 | 6 components | invisible to rpctrace |
| `swtch` | -60 | 4 components | invisible to rpctrace |
| `thread_switch` | -61 | 5 components | invisible to rpctrace |
| `syscall_vm_map` | -64 | every caller of `vm_map()` via glibc `mach-shortcuts` | 137 / 381 / 54240 (as the RPC fallback) |
| `syscall_vm_allocate` | -65 | every caller of `vm_allocate()` via glibc `mach-shortcuts` | 30 / 171 / 11957 (as the RPC fallback) |
| `syscall_vm_deallocate` | -66 | every caller of `vm_deallocate()` via glibc `mach-shortcuts` | 49 / 241 / 55580 (as the RPC fallback) |
| `syscall_task_create` | -68 | every caller of `task_create()` via glibc `mach-shortcuts` | 2 / 15 / 3064 (as the RPC fallback) |
| `syscall_task_terminate` | -69 | every caller of `task_terminate()` via glibc `mach-shortcuts` | 0 / 17 / 3059 (as the RPC fallback) |
| `syscall_task_suspend` | -70 | every caller of `task_suspend()` via glibc `mach-shortcuts` | 0 / 0 / 0 (as the RPC fallback) |
| `syscall_task_set_special_port` | -71 | every caller of `task_set_special_port()` via glibc `mach-shortcuts` | 7 / 61 / 7085 (as the RPC fallback) |
| `syscall_mach_port_allocate` | -72 | every caller of `mach_port_allocate()` via glibc `mach-shortcuts` | 36 / 145 / 16473 (as the RPC fallback) |
| `syscall_mach_port_deallocate` | -73 | every caller of `mach_port_deallocate()` via glibc `mach-shortcuts` | 141 / 1464 / 343547 (as the RPC fallback) |
| `syscall_mach_port_insert_right` | -74 | every caller of `mach_port_insert_right()` via glibc `mach-shortcuts` | 41 / 341 / 57434 (as the RPC fallback) |
| `syscall_mach_port_allocate_name` | -75 | every caller of `mach_port_allocate_name()` via glibc `mach-shortcuts` | 8 / 121 / 20686 (as the RPC fallback) |
| `syscall_thread_depress_abort` | -76 | every caller of `thread_depress_abort()` via glibc `mach-shortcuts` | 0 / 0 / 0 (as the RPC fallback) |
| `thread_set_self_state` | -77 | no direct caller found | not observed |

Consequences for §6:

1. The §6 row "`vm_allocate`, `vm_map`, `vm_protect` → memory manager RPCs"
   turns what are traps today (`vm_map`, `vm_allocate`, `vm_deallocate`) into
   IPC round trips to a userspace memory manager. At ~55k calls each in the
   build, mostly on the process-creation path, this is the first thing Phase 11
   should measure. (`vm_protect` has no shortcut; it is already an RPC today.)
2. `mach_port_deallocate`, `mach_port_insert_right`, `mach_port_allocate` and
   `mach_port_allocate_name` are traps today, and are the four most frequent
   port operations. On Joist they are capability operations, which should be
   kernel calls too (seL4-style), so this matches. But they must stay
   syscalls, not become RPCs to a userspace server.
3. `mach_msg_trap`, `mach_reply_port`, `mach_task_self`, `mach_thread_self`
   and `mach_host_self` are universal. `swtch`, `swtch_pri` and
   `thread_switch` are glibc's spin-lock and `sched_yield` back-off. They need
   a Joist equivalent (a yield / directed-yield call) that §6 doesn't list.
4. `evc_wait` is referenced in glibc's `mach/` headers only. `mach_print` is
   kernel-debug printing. Neither needs a mapping.

## Static results (source cross-reference, 2026-09-23)

`tools/phase0/static.py` searched hurd 0.9.git20260919 and glibc 2.43 (Debian
sources, quilt `.pc/` backups excluded) for each of the 196 non-reply calls in
`gnumach.msgids`. A *call site* is `name (` or `__name (`; an
*implementation* is `…_S_name (`. The per-call result, with the components
named, is `phase0-data/static-call-sites.csv`.

The raw output lists 71 calls with no call site and no implementation. That
overstates it: three groups are implemented under other naming conventions,
which a targeted grep confirmed, and one group belongs to the kernel.

| Group | Calls | Actually |
|---|---:|---|
| `notify` (`mach_notify_*`) | 6 | **used.** Implemented as `do_mach_notify_*` in libports, console, devnode, eth-filter, boot |
| `exc` (`exception_raise`) | 1 | **used.** Implemented as `catch_exception_raise` in glibc `hurd/` (signal thread) |
| `memory_object_default` | 2 | **used by mach-defpager only** (`seqnos_memory_object_create`, `…_data_initialize`) |
| `device_pager_*` | 9 | **kernel-internal.** Same msgids as `memory_object_*`; nothing in userspace |
| everything else | 53 | **no user found** |

The 53 unused, by subsystem:

- **mach_host (24):** processor sets and processor control (`processor_*`,
  `processor_set_*` except `processor_set_tasks`), `task_assign_default`,
  `thread_assign*`, `thread_policy`, `thread_depress_abort`,
  `task_max_priority`, `host_processors`, `host_set_time64`,
  `host_adjust_time64`.
- **mach_debug (11):** all of it.
- **mach (9):** `mach_ports_register`/`lookup`, the emulation-vector calls,
  `task_ras_control`, `thread_get_special_port`, `vm_machine_attribute`,
  `memory_object_destroy`.
- **mach4 (4):** thread and task PC sampling readers.
- **mach_port (3):** `mach_port_rename`, `set_mscount`, `set_seqno`.
- `device_open_new`, `vm_pages_phys`.

Calls with **exactly one user** (47) are listed per component in the CSV. The
ones that matter for the open questions:

- **mach-defpager only**, which is being rewritten anyway, so they can be
  ignored (open question 1): `memory_object_create`,
  `memory_object_data_initialize`, `memory_object_data_unavailable`.
- **libpager only:** the whole `memory_object_*` pager side (10 calls). The
  Phase 6 libpager rewrite owns all of it; nothing else touches the protocol.
- **glibc `sysdeps/mach/hurd` only:** `mach_port_allocate_name` (the fork
  namespace copy) and all the `i386_*` segment calls.
- **Debugging/admin tools only** (open question 2): `vm_inherit` (utils),
  `task_assign` (sutils), plus the PC-sampling enable/disable in glibc
  (profiling). All of `mach_debug` is unused even by tools.

Reconciliation with the dynamic side: every call observed at runtime has a
static user. The one apparent exception, `device_pager_init_pager`, turned out
to be `memory_object_init` printed under a colliding msgid.

## Known-hard rows, flagged in advance

These are already known not to be one-line swaps (§6), and should be filled in
first because they size Phase 5:

- **Out-of-line memory → shared memory grant.** A semantics change, not a
  mechanism swap: Mach OOL is copy-on-write with sender-side deallocation. Every
  use site must be examined for whether it depended on copy semantics or merely
  on moving a large buffer cheaply. Expect multi-week work.

  **Evidence so far (2026-09-23)** is in `phase0-data/ool-params.csv`, from
  `tools/phase0/ool.py`: the `.defs` preprocessed the way MIG does, and every
  parameter whose type is an unbounded array. **140 OOL-capable parameters
  in 77 routines across 19 `.defs` files; 54 carry `dealloc`**
  (the sender's copy is deallocated on send). Ordered by build-trace frequency,
  these are the ones that actually fired:

  | RPC | Calls (build) | OOL-capable parameters |
  |---|---:|---|
  | `io_read` | 66202 | `data` out **dealloc** |
  | `io_write` | 27211 | `data` in |
  | `dir_readdir` | 4218 | `data` out **dealloc** |
  | `exec_startup_get_info` | 2302 | `argv` out **dealloc**, `envp` out **dealloc**, `dtable` out **dealloc**, `portarray` out **dealloc**, `intarray` out **dealloc** |
  | `file_exec_paths` | 2301 | `argv` in, `envp` in, `fdarray` in, `portarray` in, `intarray` in, `deallocnames` in, `destroynames` in |
  | `auth_getids` | 648 | `euids` out, `auids` out, `egids` out, `agids` out |
  | `auth_makeauth` | 555 | `other_handles` in, `euids` in, `auids` in, `egids` in, `agids` in |
  | `file_set_translator` | 11 | `passive` in |
  | `file_get_storage_info` | 0 | `ports` out **dealloc**, `ints` out **dealloc**, `offsets` out **dealloc**, `data` out **dealloc** |

  The review order follows from this. `io_read` is the one that matters: the
  server hands over its buffer with `dealloc`, so on Mach the client gets
  pages the server no longer has. A grant would have to model that as a
  transfer, not as sharing. `io_write` sends *without* `dealloc`, so the client
  keeps its buffer and relies only on "moved cheaply", which is the easy case
  for a grant. MIG only goes out-of-line above the inline threshold, so how
  many of the 66k `io_read`s actually used OOL is not in this data; rpctrace
  does not show it.
- **Port sets → badges on one endpoint.** libports' per-port handlers and its
  ability to move a port between sets at runtime are not obviously the same
  shape as badge demultiplexing. Confirm against real libports usage.

  **Evidence so far (2026-09-23):** every `mach_port_move_member` call site in
  the Hurd and glibc (26 sites) falls into one of four patterns:

  1. **Put a new port into its bucket's set:** libports
     `create-internal.c:110`, `import-port.c:102`,
     `reallocate-from-external.c:78`, `reallocate-port.c:66`,
     `transfer-right.c:99`. This is a one-time placement at creation; each
     bucket is one set. It maps onto "one endpoint per bucket, one badge per
     port".
  2. **Re-home a port on reauthentication:** libdiskfs `protid-make.c:53`,
     libnetfs and libtrivfs `io-reauthenticate.c`, pfinet and lwip
     `io-ops.c`. A fresh port for the new credentials goes into the bucket's
     set. Same shape as 1: a new badge on the same endpoint.
  3. **Take a port out of every set:** libports `claim-right.c:41` (move to
     `MACH_PORT_NULL`) when a receive right is handed out of libports. That's
     the inverse of 1: revoke the badge, then transfer the endpoint.
  4. **A temporary set of *reply* ports:** glibc `hurd/hurdselect.c:335` and
     `:357`. `select`/`poll` moves one reply port per descriptor, plus the
     signal port, into a set, then receives on the set. **This does not fit
     badges on one endpoint.** It's a wait on several outstanding replies at
     once, and under synchronous call/reply it has no direct equivalent. It
     needs either an asynchronous notification-based `io_select`, or a
     notification-bound endpoint group, which is §6's second option. This is
     the case to design around. *(Not yet confirmed by reading the code:
     this is inferred from the call arguments (`sigport`, `portset`) and from
     the known design of Hurd `select`. The testbed went offline before
     `hurdselect.c` could be read. Confirm first.)*

  Also in the list: `boot/boot.c` (8 sites, subhurd bootstrap),
  `startup/startup.c` (2), `mach-defpager` (3, being rewritten).

  Also: libports already sets protected payloads
  (`mach_port_set_protected_payload`, observed in the boot trace), so pattern
  1's demultiplexing is already by payload. The badge mapping for patterns 1–3
  looks cheap. Pattern 4 is the real cost.
- **`memory_object_*` (external pagers) → page-fault IPC.** Drives the Phase 6
  paging protocol and the libpager rewrite.
- **Exception ports → fault IPC.** Drives glibc signal delivery.

## Open questions

1. Which `.defs` calls are referenced only by `mach-defpager` (which is being
   rewritten anyway) and can therefore be ignored entirely?
2. Which are referenced only by debugging tools rather than the running system?
3. What does the Hurd use that is *not* in the `.defs` files — direct syscalls,
   `mach_msg` idioms, assumptions about message layout?
