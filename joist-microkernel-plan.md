# Joist: A C Microkernel for GNU Hurd

**Kernel name:** Joist
**Author:** Aditya
**Status:** Planning / pre-implementation
**Language:** Pure C (C17, freestanding for the kernel) everywhere — kernel, new servers, and reused Hurd code. Small amounts of assembly only where the hardware demands it (boot entry, context switch, interrupt stubs).
**Compiler:** Clang/LLVM (clang, lld, llvm-objcopy, clang-tidy, clang static analyzer) for everything Joist owns. glibc may fall back to GCC under the pre-committed policy in §13.

---

## Mission statement

> **Joist carries the GNU Hurd.**
>
> Joist's mission is to give the GNU system a microkernel it can trust: small enough to understand, strict enough to never lose control of its own resources, and quiet enough that the servers above it can fail and recover without the user ever noticing.
>
> We build Joist in plain C, compiled with Clang, and we earn its safety through discipline: a strict coding standard, static analysis, sanitizers, and testing that never stops. We hold it to the reliability of QNX and the resilience of Minix 3, and we keep it simple enough that one person can read all of it. We reuse the work of the Hurd community wherever it serves those goals, and we rewrite without hesitation wherever it does not.
>
> Like its namesake, Joist is a plain load-bearing member. Nobody admires a joist, nobody thinks about a joist, and everything above it depends on it holding. That is the job.

---

## 1. Intent

Build **Joist**, a new microkernel in pure C compiled with Clang, that replaces GNU Mach underneath the GNU Hurd, and reuse as much of the existing Hurd userland (servers, libraries, glibc port, Debian GNU/Hurd packages) as possible.

The end product is a complete GNU system that:

1. Is **as reliable as QNX** — deterministic, bounded, real-time capable, no unbounded kernel resource usage, predictable IPC latency.
2. Is **as resilient as Minix 3** — drivers and system servers can crash and be restarted transparently, without taking the system down.
3. Is **fully POSIX compatible** — real software (coreutils, bash, gcc, Python, etc.) runs unmodified; aim for passing the Open POSIX Test Suite.
4. Follows **KISS** — the kernel does the minimum that must be privileged; everything else is a replaceable userspace server.

### 1.1 Priority order (when goals conflict)

| Rank | Goal | Meaning in practice |
|------|------|---------------------|
| 1 | **Reliability (QNX)** | If reusing Hurd code would introduce unbounded behaviour, priority inversion, or kernel memory exhaustion, we do not reuse it. |
| 2 | **Resilience (Minix 3)** | Every server must have a defined crash/restart story. Code that cannot be made restartable is rewritten. |
| 3 | **POSIX compatibility** | Full conformance is the target, but never at the cost of 1 or 2. |
| 4 | **Reuse of Hurd** | Reuse is a means to save time, not a goal. It loses every tie against 1–3. |
| 5 | **Performance** | Fast IPC is required, but correctness and bounded behaviour come first. |

**KISS is a design principle applied at every level, not a ranked goal.**

---

## 2. Why Mach has to go

The Hurd's architecture (translators, user-extensible filesystem namespace, everything-is-a-server) is sound. Most of its practical problems trace back to GNU Mach:

- **Slow IPC.** Mach messages are asynchronous, kernel-buffered, and copied. The Hurd makes many RPCs per POSIX call, so IPC cost compounds.
- **No resource accounting.** Message queues, port rights, and VM objects consume kernel memory on behalf of any task. A misbehaving or malicious task can exhaust the kernel.
- **Complexity.** The Mach interface (ports, port sets, send-once rights, external memory objects, device interface, host/processor sets) is large and hard to reason about or verify.
- **Priority inversion.** Asynchronous IPC gives no natural path for priority inheritance from client to server.
- **Driver model.** Historically drivers lived in the kernel (Linux 2.0-era glue); modern Hurd has moved storage and networking to userspace (rumpdisk, netdde, lwip), which we build on.
- **No resilience model.** Nothing supervises servers. If `proc`, `auth`, `exec`, or the root filesystem dies, the system dies.

---

## 3. Lessons from prior attempts

**Hurd on L4 (early–mid 2000s)** stalled. The recurring lessons:

- L4's minimal IPC did not provide what the Hurd relies on from Mach ports: kernel-tracked capabilities that can be transferred, and **no-senders / dead-name notifications** that let servers free resources when clients go away. Emulating these in userspace was slow and fragile.
- The effort drifted into research redesigns (Coyotos, Viengoos) instead of shipping a working system.

**Takeaways for this project:**

1. The new kernel must natively provide capability transfer and lifetime notifications. Do not try to emulate them on top of something that lacks them.
2. **Ship a bootable Hurd first, redesign later.** Compatibility before elegance.
3. Keep scope fixed. Research ideas go into a "later" list, not the main branch.

---

## 4. Reuse strategy: what we keep, adapt, replace

### 4.1 Classification

| Component | Decision | Notes |
|-----------|----------|-------|
| GNU Mach | **Replace** | The whole point. |
| MIG (Mach Interface Generator) | **Adapt (retarget)** | Keep the `.defs` IDL; add a new backend that emits stubs for the new kernel's IPC. This is the single biggest reuse lever. |
| Hurd `.defs` interfaces (`io.defs`, `fs.defs`, `process.defs`, `auth.defs`, …) | **Keep** | The protocol between Hurd servers stays the same. |
| libports | **Adapt** | Core of every server; port/receive-right handling rewritten over new capabilities. |
| libpager | **Rewrite** | Mach external-pager protocol replaced with a simpler page-fault-to-server protocol. |
| libdiskfs, libnetfs, libtrivfs, libfshelp, libiohelp, libihash, libstore | **Keep / light adapt** | Mostly Mach-agnostic above libports/libpager. |
| ext2fs, tmpfs, procfs, storeio, term, pflocal, lwip/pfinet, password, console | **Keep, then harden** | Adapted via retargeted MIG + libports. Made restartable (see §7). |
| proc, auth, exec, startup | **Keep first, rewrite later** | Critical, stateful servers. Reuse for bring-up; rewrite in clean C for resilience when needed. |
| mach-defpager | **Rewrite** | Tied tightly to Mach memory objects. |
| rumpdisk (NetBSD rump drivers), netdde | **Keep** | Already userspace drivers. Need IRQ/IO/DMA capabilities from new kernel. |
| glibc Hurd port (`sysdeps/mach`, `sysdeps/mach/hurd`) | **Adapt** | Replace `sysdeps/mach` layer with `sysdeps/joist`; keep the Hurd layer as much as possible. |
| Debian GNU/Hurd package set | **Keep** | Source of userland once glibc works. |
| Reincarnation server, data store | **New (C)** | Minix 3 concepts, not in Hurd. |

### 4.2 The central strategy: Mach-shaped, not Mach-implemented

We do **not** implement the Mach kernel interface in the new kernel. Instead:

1. The new kernel exposes a **small native API** designed for reliability (§5).
2. The native API is designed so that **every Mach concept the Hurd actually uses has a clean mapping** onto it (§6).
3. The mapping is done in **userspace libraries** (retargeted MIG stubs, adapted libports, glibc sysdeps), not in the kernel.

This keeps the kernel small and verifiable while letting Hurd server code run largely unchanged.

---

## 5. Kernel design

### 5.1 Principles

- **Mechanism, not policy.** Scheduling policy parameters, memory management policy, driver policy, and security policy live in userspace.
- **No dynamic allocation in the kernel after boot** except from memory explicitly donated by a task (seL4-style "untyped" memory or per-task kernel quotas). Every kernel object is paid for by someone.
- **Bounded everything.** Every system call has a bounded worst-case execution time. No unbounded loops over user-controlled structures.
- **Dangerous code is contained.** Assembly, raw pointer arithmetic on physical memory, page-table manipulation, and user-memory copying live in a few audited modules (`arch/`, `mem/`, `usercopy.c`). Every such function carries a comment stating its preconditions and invariants.
- **User pointers are never dereferenced directly.** All access to user memory goes through `copy_from_user` / `copy_to_user` style functions with bounds and fault handling.
- **Strict coding standard.** A written subset of C17 based on MISRA C and CERT C: no VLAs, no recursion in the kernel, no dynamic allocation after boot, no implicit integer conversions, fixed-width integer types, every return value checked, every `switch` has a `default`.
- **Target size:** under ~15k lines of C in the core kernel, excluding arch-specific code. Treat this as a **tripwire, not a promise**: crossing it does not fail the build, it triggers a review asking what crept in and whether it belongs in userspace. For calibration, seL4's verified kernel is around 10k lines with a comparable object set, so the number is tight but not fantasy.

### 5.2 Kernel objects

| Object | Purpose |
|--------|---------|
| **Task** | Address space + capability space. Maps to Mach task. |
| **Thread** | Schedulable execution context, belongs to a task. |
| **Endpoint** | IPC rendezvous point. A receive capability on an endpoint = Mach receive right. |
| **Capability** | Unforgeable reference to a kernel object with rights bits (send, receive, send-once, grant, etc.) and a server-assigned **badge**. |
| **Notification object** | Async, non-blocking signal bits (QNX "pulses" / seL4 notifications). Used for IRQs, timers, and lifetime notifications. |
| **Memory region / frame** | Physical memory capability; mapped into tasks. |
| **IRQ handler** | Capability that routes a hardware interrupt to a notification object. |
| **IO port / MMIO / DMA** | Capabilities granting a driver only the hardware it needs (Minix 3-style least privilege). |
| **Scheduling context** | CPU budget + period + priority; enables temporal isolation (QNX adaptive partitioning analogue). |

### 5.3 IPC

- **Synchronous send/receive/reply** as the primary primitive (QNX `MsgSend`/`MsgReceive`/`MsgReply` model, L4-style fast path).
- **Short messages in registers**, longer messages copied **directly thread-to-thread** by the kernel with no intermediate buffering.
- **Large transfers via shared memory grants**, replacing Mach out-of-line (OOL) copy-on-write memory. **This is a semantics change, not a drop-in swap** — see the note after the table in §6.
- **Priority inheritance / scheduling-context donation**: a server runs at the priority of the client it is serving. This eliminates the main source of priority inversion.
- **Call + ReplyRecv** combined system calls for the server loop fast path.
- **Timeouts** on every blocking operation.
- **Asynchronous notifications** only as bit-sets on notification objects (never queued messages), so they cannot exhaust memory.

### 5.4 Capability lifetime notifications (critical for Hurd)

The Hurd depends on Mach's **no-senders** and **dead-name** notifications to clean up per-client state. The kernel provides:

- **No-senders:** when the last send capability to an endpoint badge is deleted, the kernel signals the endpoint owner's notification object with that badge.
- **Dead-name:** when an endpoint is destroyed (e.g. server crash), holders of send capabilities get an error on next use, and optionally a notification.
- **Send-once:** a capability type that is consumed on first use (used for replies).

These must be implemented with **bounded** cost (e.g. reference counting per badge with preallocated accounting slots).

### 5.5 Memory management

- Kernel manages only page tables and frame capabilities.
- **Page faults are delivered as IPC** to the task's registered pager (a userspace server).
- A userspace **memory manager** implements `mmap`, copy-on-write, anonymous memory, and file-backed memory by talking to filesystem servers.
- **Swap** is handled by a rewritten default pager in userspace.
- Copy-on-write for `fork()` is implemented by the memory manager using read-only mappings + fault handling.

### 5.6 Scheduling

- Fixed-priority preemptive scheduler with 256 priority levels (real-time capable).
- Scheduling contexts with budget/period for temporal isolation.
- SMP from the design stage (per-CPU run queues, explicit locking design), even if early bring-up is uniprocessor.

**Constraint interaction (SMP x no-allocation x bounded WCET).** These three commitments constrain each other and cannot be designed independently:

- *No allocation after boot* means every per-CPU structure (run queues, IPC fast-path scratch, notification accounting slots) is **preallocated per CPU at boot**, sized from a configured CPU maximum. There is no growth path at runtime.
- *Bounded WCET* forbids unbounded spinning, so lock hold times must be bounded by construction: a documented **global lock order**, no lock taken across an IPC transfer, and no loop over a user-controlled count while holding a lock.
- *Object deletion under SMP* (capability revoke, endpoint destroy) normally wants deferred reclamation, but RCU-style grace periods need somewhere to queue retired objects, which the no-allocation rule denies. The reclamation scheme must therefore use **preallocated per-CPU retire slots with a bounded drain**, and revoke must be able to block the caller rather than allocate.

This is decision **0003** (see §15) and must be settled before Phase 3 IPC work begins, not discovered during it.

### 5.7 What is NOT in the kernel

Filesystems, networking, drivers (except timer and interrupt controller basics), process management policy, POSIX anything, signals, paging policy, device naming, security policy, time-of-day.

### 5.8 Target platforms

1. **x86_64 in QEMU** (first and primary target; Debian GNU/Hurd has an x86_64 port to draw userland from).
2. Real x86_64 hardware (ThinkPad-class machines are good test targets).
3. **aarch64** later (the Hurd userland has no aarch64 port today, so this is a larger effort).

Boot via **Multiboot2** (GRUB) or **Limine**, loading the kernel plus bootstrap server modules.

---

## 6. Mapping Mach concepts onto the new kernel

| Mach concept (as used by Hurd) | New kernel mechanism | Where the mapping lives |
|---|---|---|
| Task | Task | glibc sysdeps, proc |
| Thread | Thread | glibc sysdeps (pthreads) |
| Port receive right | Endpoint + receive capability | libports |
| Port send right | Badged send capability | libports, MIG stubs |
| Send-once right | Send-once / reply capability | MIG stubs |
| Port set | Multiple badges on one endpoint, or a notification-bound endpoint group | libports |
| `mach_msg` RPC | Synchronous call/reply | Retargeted MIG backend |
| Out-of-line memory | Shared memory grant | MIG backend |
| No-senders / dead-name notification | Native lifetime notifications (§5.4) | libports |
| External memory objects (`memory_object_*`) | Page-fault IPC to userspace pager | Rewritten libpager + memory manager |
| `vm_allocate`, `vm_map`, `vm_protect` | Memory manager RPCs | glibc sysdeps |
| Exception ports | Fault IPC: page faults to the task's pager, which may pass them on; every other fault to its exception handler (0009, 0027) | Memory manager (pager), glibc signal thread (exception handler) |
| `thread_get_state` / `thread_set_state` / `thread_suspend` | Thread control capabilities | glibc (signal delivery), proc, debuggers |
| `task_create` with VM inheritance (used by `fork`) | Task create + memory manager COW clone | glibc `fork`, memory manager |
| Device interface (`device_open`, etc.) | IRQ/IO/MMIO/DMA capabilities given to userspace drivers | rumpdisk, netdde glue |
| `host_priv`, `device_master` privileged ports | Explicit capabilities handed out at boot by the root task | startup / bootstrap |

**Rule:** if a Mach feature is used by the Hurd but cannot be mapped with bounded cost, we change the Hurd code, not the kernel.

**Two rows in that table are not one-line swaps, and the table understates them:**

- **Out-of-line memory → shared memory grant.** Mach OOL transfer is copy-on-write with sender-side deallocation semantics: the sender may hand over a region and have it vanish from its own map, and the receiver gets a private copy-on-write view whose lifetime is independent of the sender. A shared-memory grant has none of that by default — it is shared, not copied, and its lifetime is a negotiation. Every OOL use in the Hurd (`io_read`/`io_write` bulk paths, `dir_readdir`, `file_get_storage_info`, argument passing in `exec`) has to be examined for whether it depended on the copy semantics or merely on "a big buffer moved cheaply". The MIG backend cannot paper over the difference; where it cannot, the `.defs` caller changes. Budget this as multi-week work inside Phase 5, not a code-generation detail.
- ~~**Port sets → badges on one endpoint.**~~ **Resolved in Phase 0, decision 0005.** The concern was libports' ability to move a port between sets at runtime. `mach_port_move_member` fires 3 times at boot, 0 in a shell session and 7 across an entire package build, against `mach_port_deallocate` at 343 547 in the same trace — port set membership is static in practice. Badge demultiplexing on one endpoint is sufficient, no kernel port-set object is needed, and this row is no longer known-hard. Out-of-line memory, above, keeps that status.

---

## 7. Resilience layer (Minix 3 goal)

### 7.1 Components

- **Reincarnation server (RS)** — written in C. Starts, supervises, heartbeats, and restarts system servers and drivers according to policy.
- **Data store (DS)** — written in C. Servers publish state they need after a restart (e.g. the list of open handles, configuration, driver device state). DS itself must be simple enough to be trusted, and is itself supervised.
- **Crash detection** — the kernel notifies RS when a supervised task faults or exits (via notification object). RS also sends periodic heartbeat requests and treats a missed deadline as a hang.

### 7.2 Server classes

Every server is assigned one class. No server ships without a class.

| Class | Examples | Restart strategy |
|-------|----------|------------------|
| **A — Stateless / idempotent** | storeio, rumpdisk, netdde, console | Restart immediately. Clients retry pending requests transparently. |
| **B — Recoverable state** | ext2fs, tmpfs, lwip/pfinet, term, pflocal | Restart; recover state from DS and/or on-disk journal; clients re-open handles via glibc retry logic. |
| **C — Critical core** | proc, auth, exec, memory manager, RS, DS | Rebuild state from kernel (list of tasks/capabilities) + DS + client re-registration. Rewrite from scratch (in C, to Joist's coding standard) if the existing code cannot support this. |

### 7.3 Client-side recovery

A restarted server is worthless if its clients cannot pick the conversation back up. This is the hardest part of the resilience goal — harder than RS and DS — because it lives in glibc, touches every file descriptor, and has to preserve POSIX semantics that were never written with a vanishing server in mind.

**Structure.** All of it lives in **one recovery module** in the Hurd sysdeps, not scattered through glibc. The module owns: the per-fd recovery record, the reconnect state machine, and the idempotency classification table. Everything else calls into it.

#### 7.3.1 The recovery record

Each open file descriptor carries enough state to be rebuilt:

| Field | Purpose |
|---|---|
| Reconnect key | How to find the object again (see §7.3.2). |
| Open flags | `O_RDWR`, `O_APPEND`, `O_NONBLOCK`, `O_CLOEXEC`, … |
| File offset | Owned by the *file description*, not the descriptor (see §7.3.3). |
| Auth state | Which identity the object was opened under, so the reconnect re-authenticates as the same principal, not as the caller at reconnect time. |
| Recoverability class | `recoverable` / `best-effort` / `unrecoverable` (§7.3.4). |

The record is client-side state. It must not depend on the server having survived.

#### 7.3.2 Reconnect keys, and where path-based recovery breaks

Re-opening "by saved path with saved flags" is the easy case and not the common one. The cases that must each have a decided answer:

- **Ordinary file, still linked.** Re-lookup by path. Correct, and the base case.
- **Unlinked-but-open file** (`open` then `unlink`, `O_TMPFILE`). No path exists to re-open. Either the server persists an identity token across restart (inode + generation, republished to DS) that survives as an unlinked-but-referenced object, or the fd is classed **unrecoverable**. There is no third answer; pick one in Phase 0.
- **Path re-bound during the outage.** The path may now resolve to a *different* object (renamed, replaced, a translator re-settled). The reconnect must validate identity — inode plus generation number, not path alone — and fail rather than silently hand the application a different file.
- **Pipes and `pflocal` sockets.** Both endpoints' state lives in a server with no on-disk backing. If `pflocal` restarts, the pipe contents are gone. These are **best-effort at most**; data loss is visible and must be reported, not hidden.
- **Network sockets** (lwip/pfinet). A TCP connection does not survive the stack restarting. Class as unrecoverable; deliver `ECONNRESET`.
- **`mmap` regions.** A file-backed mapping is a standing relationship between the memory manager and the filesystem server, not a request/reply. Restarting the filesystem under a live mapping is its own problem; the memory manager must either hold the mapping's pages or fault the region to `SIGBUS`. Decide in Phase 4, not Phase 9.
- **Current working directory and root.** These are ports too, and they go stale exactly like fds. They need recovery records as well.

#### 7.3.3 Sharing: descriptors, descriptions, and fork

The recovery record hangs off the **file description**, not the descriptor, because `dup`, `dup2`, and `fork` create multiple descriptors sharing one description with one shared offset. Consequences:

- Reconnection must happen **once per description**, not once per descriptor, or the offset gets restored several times.
- Two processes sharing a description after `fork` will both notice the death. The recovery must be idempotent between them, which in practice means the reconnect is serialised per description and the loser of the race adopts the winner's result.
- A description shared across `fork` cannot be re-opened by a child that has since `exec`'d away the path it was opened from. This is another source of **unrecoverable**.

#### 7.3.4 Idempotency classification

Every RPC in the reused `.defs` interfaces is classified, and the classification is **checked in as a table, not inferred at runtime**:

- **Idempotent** — `io_read` at an explicit offset, `io_stat`, `dir_lookup`. Safe to replay after reconnect.
- **Non-idempotent** — `io_write` in append mode, directory mutations, anything allocating. Replaying risks double execution. If the reply was lost, the client cannot know whether the operation happened.
- **Unclassified is an error.** A new RPC without a classification fails the build, the same way a server without a class (§7.2) does not ship.

For the non-idempotent case where the request was sent but no reply arrived, the honest answer is to fail the call (`EIO`) rather than guess. Silent double-execution is worse than a visible error.

#### 7.3.5 What the application sees

- Recovery is attempted **transparently** for `recoverable` descriptions; the application sees a delay, nothing more.
- `best-effort` and `unrecoverable` descriptions return a normal POSIX error (`EIO`, `ECONNRESET`, `EBADF` after close-on-error), because an application that cannot tell the difference between "server restarted cleanly" and "your data is gone" is a bug factory.
- Recovery must not deadlock: it is subject to the same timeouts as any other blocking operation (§5.3), and a bounded number of reconnect attempts.
- Interaction with signals: a reconnect in progress is interruptible, and `SA_RESTART` semantics apply to the *original* call, not to the reconnect.

#### 7.3.6 Status

This subsection is the design sketch, not the design. Phase 0 produces `docs/client-recovery.md` with a decided answer for every case in §7.3.2 and a first cut of the §7.3.4 table, because these answers constrain the kernel's lifetime notifications (§5.4) and the memory manager (§5.5) — both of which are built long before Phase 9.

### 7.4 Driver isolation

Each driver gets only the capabilities it needs: its IRQ(s), its IO/MMIO ranges, a DMA-restricted memory region (use the IOMMU when available). A buggy driver cannot touch other hardware or memory.

### 7.5 Resilience acceptance tests

- Fault injection harness: kill any class A/B server at random points under load (build, `dd`, network transfer). The system must keep running and the workload must complete or fail cleanly.
- Class C servers: kill-and-recover tests for each, one at a time.
- Target: thousands of injected crashes per night in CI without a system-level failure.

---

## 8. Reliability layer (QNX goal)

- **Priority inheritance through IPC** (§5.3) everywhere, including in the Hurd servers' request queues (libports must not reorder requests in ways that defeat it).
- **Bounded kernel memory**: every kernel object is backed by memory donated by the requesting task. Kernel cannot be exhausted by userspace.
- **Temporal isolation**: scheduling contexts prevent one runaway task starving the system.
- **Watchdog**: hardware watchdog petted by RS; if RS itself hangs, the machine resets cleanly.
- **Deterministic IPC path**: no allocation, no unbounded loops, measured worst-case latency tracked in CI.
- **No panics in normal operation**: kernel code uses explicit error codes; `panic()` (and `assert`-style invariant checks) reserved for true invariant violations, and the panic handler logs to serial and halts (or reboots, configurable).

### 8.1 Verification and testing for the kernel

- **Host-side unit tests**: kernel data structures (capability tables, run queues, allocators) are written to compile both freestanding and on the host, so they can be tested with a plain C test harness (e.g. a minimal in-tree framework, or Unity/cmocka).
- **Clang sanitizers on host builds**: AddressSanitizer, UndefinedBehaviorSanitizer, MemorySanitizer, and ThreadSanitizer run over every host test.
- **UBSan in the kernel**: a minimal in-kernel UBSan runtime (trap mode, `-fsanitize=undefined -fsanitize-trap=undefined`) in debug builds.
- **Static analysis**: `clang --analyze` / `scan-build` and `clang-tidy` (with CERT and bugprone checks) on every commit, warnings treated as errors.
- **Fuzzing**: libFuzzer on host-built parsers and IPC message decoding; syzkaller-style random syscall sequences against the kernel in QEMU.
- **Bounded model checking**: CBMC for capability-space and IPC invariants; Frama-C (WP) for the most critical functions if time allows.
- **TLA+ (or similar) model** of IPC + capability lifetime + notifications before implementing them.

---

## 9. Security and trust model

Capabilities give Joist a security *mechanism*; they do not by themselves state who is trusted with what. That has to be written down, because the resilience layer (§7) deliberately introduces a component — DS — that holds recovery state for most of the system.

### 9.1 Trusted computing base

Ranked by what their compromise costs:

| Component | Trusted for | If compromised |
|---|---|---|
| **Kernel** | Everything: isolation, capability integrity, scheduling. | Total loss. This is why it is small, audited, and verified (§8.1). |
| **Toolchain** (compiler, linker, binary tools) | Compiling exactly what the source says. | Total loss, invisibly: a subverted compiler inserts code no source review can see. Mitigated by decision **0026**: reproducible builds, Clang and lld verified by diverse double-compiling against a GCC from a full-source bootstrap (`live-bootstrap`, from a hex0 seed). Not covered: the sources themselves, the build machine's own OS, `xorriso`, QEMU, firmware. |
| **Bootstrap chain** (firmware, GRUB/Limine, boot modules) | Delivering an unmodified kernel and root task. | Total loss, invisibly. Measured/verified boot is **out of scope for now** and recorded as such. |
| **Root task** | Holds every capability at boot and hands out the rest. | Total loss. It must be small, do nothing but partition and delegate, and then drop what it no longer needs. |
| **RS** (reincarnation server) | Starting and restarting servers; holds the capabilities to create tasks and grant hardware access. | Can start arbitrary servers with arbitrary privilege. Second only to the root task. |
| **DS** (data store) | Custody of other servers' recovery state. | See §9.2 — the interesting case. |
| **Memory manager** | Address-space contents of every task. | Reads and writes all user memory. |
| **auth, proc, exec** | POSIX identity, process table, program loading. | Loss of POSIX-level security; kernel isolation still holds. |
| **Filesystem servers** | Only the data they store. | Bounded to their own subtree. |
| **Drivers** | Only their own hardware (§7.4). | Bounded by their IRQ/MMIO/DMA capabilities and the IOMMU. |

The important property: everything below `auth` in that table is **bounded** — its compromise does not become kernel compromise. Everything above it is TCB, and the TCB is the thing we keep small.

### 9.2 DS is a high-value target, and is treated as one

DS holds, by design, the state that class B and C servers need to come back: open-handle tables, configuration, driver device state. That makes it the single most attractive component in the system to attack, and it is *not* in the kernel's TCB, so kernel verification does not protect it.

Rules, to be enforced from the moment DS exists:

- **DS stores opaque blobs.** It does not interpret, validate, or act on any server's state. It is a keyed store, not a participant in any protocol.
- **Per-publisher isolation.** A server can read and write only its own namespace in DS. There is no cross-server read. A compromised `term` cannot read `auth`'s recovery state.
- **DS never holds capabilities on behalf of others.** Recovery records contain names and identity tokens, never transferable capability rights. A server coming back re-acquires its capabilities from RS, which knows the policy, not from DS, which knows only bytes.
- **Nothing secret goes in DS.** No keys, no credentials, no password material. If a server needs a secret to resume, it re-derives or re-requests it.
- **Restart from DS is validated.** A server treats what it reads back from DS as untrusted input and validates it exactly as it would a message from a client. Corrupt state must produce a clean failure, not a compromised server.
- **DS compromise degrades resilience, not security.** With these rules, the worst a hostile DS can do is make restarts fail or return garbage that publishers reject. That is the property to preserve in review.

### 9.3 Hardware and isolation assumptions

- **IOMMU is required for the driver-isolation claim.** Without it, any DMA-capable device can write anywhere in physical memory and §7.4 is aspirational. On hardware with no usable IOMMU, the system still runs, but the driver isolation guarantee is explicitly downgraded and must be reported at boot rather than quietly assumed.
- **DMA capabilities are range-restricted** and mapped through the IOMMU where present.
- **Side channels** (Spectre-class, cache timing) are **out of scope** for the initial design. Recorded here so it is a decision rather than an oversight; revisit once the system boots.
- **Multi-user threat model is inherited from POSIX** — `auth` and the filesystem servers enforce it. Joist does not add a new security policy; §5.1 puts policy in userspace deliberately.

### 9.4 What is deliberately not addressed yet

Verified/measured boot, disk encryption, mandatory access control, sandboxing beyond capability partitioning, and side-channel hardening. Each is a "later" list item (§3), not a gap someone should be surprised by.

---

## 10. POSIX strategy

The Hurd already implements most of POSIX in glibc on top of servers. We inherit that and fill gaps.

- **glibc:** introduce `sysdeps/joist` to replace the `sysdeps/mach` layer; keep `sysdeps/mach/hurd` changes minimal. Treat glibc's own Hurd code (under `hurd/` in the glibc tree) as reusable but expect edits wherever it calls Mach directly.
- **fork():** task create + memory manager copy-on-write clone + capability duplication (mirrors what Hurd glibc already does with Mach VM inheritance). Also implement `posix_spawn` natively for speed.
- **Signals:** keep the Hurd model (per-process signal thread + message port), backed by native fault IPC and thread control capabilities. Verify behaviour against POSIX edge cases (`sigaction` flags, `SA_RESTART`, signal masks across `fork`/`exec`).
- **Threads:** NPTL-like pthreads on native threads; futex-equivalent via a small kernel primitive or notification objects.
- **mmap / msync:** via the userspace memory manager and filesystem servers.
- **Known Hurd POSIX gaps** must be inventoried in Phase 0 and tracked as issues.

### 10.1 POSIX conformance testing

- Open POSIX Test Suite
- glibc test suite
- Linux Test Project (portable subset)
- Real-world: build Debian packages natively (the ultimate test)

---

## 11. Roadmap

Each phase has an **exit criterion**. Do not start the next phase until the current one's exit criterion is met.

### Phase 0 — Study and inventory (1–2 months)

- Run Debian GNU/Hurd (x86_64) in QEMU. Build Hurd, GNU Mach, and glibc from source.
- Try building GNU Mach, the Hurd, MIG output, and glibc with Clang; record every failure in `docs/clang-inventory.md`.
- Use `rpctrace` on real workloads (boot, shell, compile) to record **which Mach calls the Hurd actually uses**.
- Read GNU Mach's interface definitions (the `.defs` files under `include/mach/`) and mark each call: *used / unused / used-by-one-thing*.
- Write the **Mach dependency inventory**: every Mach call used, by which component, and the planned mapping (§6).
- Inventory **file-descriptor state** as the Hurd and glibc actually keep it: what hangs off a descriptor vs. a file description, how `dup`/`fork` share it, and what identity a server can republish across a restart. This feeds `docs/client-recovery.md` (§7.3.6).
- Write `docs/client-recovery.md`: a decided answer for every case in §7.3.2 and a first cut of the idempotency table (§7.3.4). These answers constrain the kernel's lifetime notifications and the memory manager, both built long before Phase 9.
- **Size the Clang/glibc problem against a fixed budget** (§13) and record the outcome as decision 0002.
- Read Minix 3 RS/DS papers and QNX Neutrino system architecture docs; read seL4 and L4 IPC papers; read the Hurd-on-L4 post-mortems.
- **Exit:** a written inventory + a decision log entry for every row in §6; `docs/client-recovery.md` exists with no case marked "undecided"; decisions 0001–0003 written (0000, the project name, is already settled — see §17).

### Phase 1 — Semantic model on the host (1–2 months)

- Build a host-side C model of the kernel's object model: tasks, threads, capabilities, endpoints, badges, notifications, lifetime notifications.
- Run it as a library under the host test harness with all sanitizers enabled; test invariants (no leaked capabilities, notifications always delivered, bounded memory).
- Optional TLA+ model of IPC + capability deletion races.
- **Exit:** invariants proven or property-tested; the API is written down as `docs/abi.md`.

### Phase 2 — Kernel bootstrap (2–3 months)

- Freestanding C kernel for x86_64 built with Clang (`-ffreestanding -nostdlib`): boot (Multiboot2/Limine), serial console, GDT/IDT, paging, physical frame allocator, APIC timer.
- Load a root task from a boot module and run it in user mode.
- Minimal debug syscalls (print to serial).
- **Exit:** a userspace "hello" task runs; kernel survives fuzzing of its tiny syscall surface.

### Phase 3 — IPC and capabilities (3–4 months)

- Implement capabilities, endpoints, sync IPC with fast path, reply capabilities, notifications, lifetime notifications, priority/scheduling-context donation.
- Benchmark round-trip IPC latency in QEMU and on real hardware; record in CI.
- **Exit:** IPC benchmarks stable; fault-injected client/server tests pass; no kernel memory growth under adversarial tests.

### Phase 4 — Memory management (2–3 months)

- Page-fault IPC, userspace memory manager, anonymous memory, COW, shared memory grants.
- **Exit:** a test program can `fork`-equivalent (via memory manager clone) and run COW correctly under stress.

### Phase 5 — MIG retarget and libports (2–3 months)

- **First:** the region capability (decision 0008's escape hatch; Phase 4's choice 7, carried from its exit by the owner, 2026-10-30): a range of one task's addresses, a backing frame and a rights ceiling, inside which its holder maps, unmaps and protects pages itself; `mm` keeps the frame and can kill the region. It is also this phase's receive window for moves (0006). Model first, then the kernel; measured against plain RPCs to `mm`, as 0008 asks.
- Add a new MIG backend emitting stubs for native IPC (keep the `.defs` input format unchanged).
- Adapt libports to native capabilities and lifetime notifications.
- Start glibc's `sysdeps/joist` (decision 0031): static, built with Clang, scoped by an inventory to what the reused servers and their test clients call (the Mach layer, `htl` threads, `malloc` over a region); an estimate over 6 weeks goes back to the owner.
- Port a trivial translator (e.g. a `libtrivfs`-based "hello" translator) as the first reused Hurd server, running on that glibc.
- Give the memory manager its limits per task tree (decision 0028): a reservation and ceilings on pages and tasks, admission control on adoption, build-time defaults, and `MM_SET_LIMIT` for the root.
- **Exit:** unmodified-logic Hurd server code runs on the new kernel via retargeted stubs; the region capability built, its calls in the model with CBMC and mutations, and measured against plain RPCs to `mm`; a fork bomb and a memory hog, each in its own tree, leave a third task able to fault new pages and fork.

### Phase 6 — Paging protocol and libpager (2–3 months)

The old plan bundled this with glibc and the Hurd boot chain. It is separated because ext2fs cannot work at all until it lands, which makes it a dependency of everything in Phases 7–8 rather than a task alongside them.

- Define the page-fault-to-server protocol that replaces Mach external memory objects (§5.5).
- Split fault routing (decision 0027): a task's pager receives page faults only and may pass one on (`JOIST_FAULT_PASS`, with a POSIX code) to the task's exception handler, which receives every other fault. Model first; the memory manager moves to `task_set_pager`.
- Rewrite libpager against it.
- Exercise it with a synthetic pager and the memory manager from Phase 4 — no filesystem server required yet.
- **Exit:** a synthetic userspace pager services faults correctly under stress, including concurrent faults, eviction, and pager death; libpager's test translator runs; a fault the pager passes on reaches the exception handler, and a hung exception handler doesn't stop the memory manager serving other tasks.

### Phase 7 — glibc port (3–5 months)

The riskiest phase in the plan, and the one most likely to overrun. It gets its own exit criterion so an overrun is visible instead of being absorbed by a phase that looks like it is making progress elsewhere.

- Continue `sysdeps/joist` from Phase 5 (decision 0031: the Mach layer, `htl` threads and `malloc` over a region are there already); keep `sysdeps/mach/hurd` edits minimal.
- Static linking first. Dynamic linking and the loader second, as a separately demonstrable step.
- Implement `fork` (task create + memory manager COW clone + capability duplication), `posix_spawn`, signals over native fault IPC (the signal thread as the exception handler of decision 0027, so `SIGSEGV` and `SIGBUS` arrive with their codes), and pthreads on native threads.
- Land the client-recovery module skeleton (§7.3) now, even though restarts do not exist until Phase 9 — retrofitting per-description recovery records into a finished glibc port is far more expensive than building them in.
- **Exit:** a statically linked C program using files, `fork`, `exec`, signals, and threads runs correctly against a stub filesystem; then the same dynamically linked.

### Phase 8 — Bootstrap the Hurd (3–5 months)

- Bring up the Hurd boot chain on the new kernel: root filesystem (tmpfs or ext2fs from a boot module), exec, startup, proc, auth.
- Port ext2fs on the Phase 6 paging protocol.
- `proc` takes over setting the memory manager's limits (`MM_SET_LIMIT`, decision 0028), mapping `RLIMIT_AS`, `RLIMIT_NPROC` and its session policy onto task trees.
- **Exit:** boot to a `/bin/sh` prompt; run coreutils; a fork bomb and a memory hog, each in its own session, leave every other session able to fault and fork.

### Phase 9 — Resilience (3–4 months)

- Implement RS and DS in C.
- Classify every running server (§7.2); implement restart for class A, then class B.
- Complete glibc client-side recovery (§7.3) on the module skeleton landed in Phase 7, using the case decisions from `docs/client-recovery.md`.
- Build the fault-injection CI harness.
- **Exit:** random kills of class A/B servers under load do not bring the system down.

### Phase 10 — Drivers and hardware (3–6 months)

- IRQ, IO port, MMIO, DMA/IOMMU capabilities.
- Bring up rumpdisk (storage), lwip or pfinet + netdde (networking), PCI arbiter, ACPI.
- Run on a real x86_64 laptop.
- **Exit:** boots from real disk, networking works, drivers survive restart tests.

### Phase 11 — Core server hardening (ongoing, 6+ months)

- Make proc, auth, exec, memory manager restartable (class C). Rewrite from scratch where the existing code resists. The memory manager restarts from state frames its restarter also holds, every record validated against the kernel (decision 0029).
- SMP bring-up and stress testing.
- Real-time latency measurements; scheduling contexts exposed to userspace policy.
- **Exit:** every server has a tested restart path; SMP stable under 72-hour soak tests.

### Phase 12 — POSIX conformance and ecosystem (ongoing)

- Work through Open POSIX Test Suite and glibc tests to closure.
- Build Debian packages natively.
- **Exit:** self-hosting (the system builds its own kernel, glibc, and Hurd).

### Phase 13 — Native optimisation (later)

- Profile hot RPC paths (`io_read`, `io_write`, `dir_lookup`, `stat`) and give them native fast paths or shared-memory protocols.
- Only after reliability and resilience exit criteria are met.

**Rough total:** the listed estimates sum to roughly 21–33 months of work for Phases 0–8 (shell prompt), and that is the optimistic reading — for one person working part-time, 3–5 years to a shell prompt is the realistic figure. Say so up front rather than discovering it in year two. Phases 0–8 is the first big milestone; everything after it is open-ended.

---

## 12. Repository layout (proposal)

*Status (2026-10-06): the tree so far follows this, with these differences.
`kernel/` also has `elf/` (boot-module validation), `sys/` (system calls,
`copy_from_user`), `lib/`, `include/`, `test/` and `fuzz/`; `cap/`, `ipc/`
and `sched/` arrive in Phase 3. Added at the top level: `user/` (Phase 2's
test programs), `boot/` (Limine configuration), `tools/` (the scripts behind
each phase's data) and `mcp-server/` (the plan as query tools).*

```
joist/
├── kernel/            # freestanding C kernel
│   ├── arch/x86_64/   # boot, paging, interrupts, context switch (assembly lives here)
│   ├── cap/           # capability space
│   ├── ipc/           # endpoints, notifications, fast path
│   ├── sched/         # scheduler, scheduling contexts
│   └── mem/           # frames, page tables
├── model/             # host-side semantic model + property tests (Phase 1)
├── servers/
│   ├── rs/            # reincarnation server (C)
│   ├── ds/            # data store (C)
│   └── mm/            # memory manager (C)
├── mig/               # MIG with native backend (fork of upstream)
├── hurd/              # fork of upstream Hurd with adaptation patches
├── glibc/             # fork of upstream glibc with sysdeps/joist
├── tests/
│   ├── fault-inject/  # resilience harness
│   ├── bench/         # IPC and syscall latency
│   └── posix/         # conformance runner
└── docs/
    ├── abi.md              # native kernel ABI
    ├── mach-inventory.md   # every Mach call the Hurd uses, and its mapping
    ├── client-recovery.md  # §7.3 case decisions + idempotency table
    ├── security.md         # trust model, TCB, hardware assumptions (§9)
    ├── clang-inventory.md  # Phase 0 Clang build failures
    └── decisions/          # design journal (one file per decision)
```

Keep Hurd, glibc, and MIG changes as clean patch series on top of upstream so they can be rebased and possibly upstreamed.

---

## 13. Tooling

- **Compiler:** Clang, pinned to one LLVM major version in the build scripts; linker `ld.lld`; `llvm-objcopy`, `llvm-objdump`, `llvm-ar`. Built from a pinned LLVM release and verified by diverse double-compiling (decision **0026**); every build reproducible (`tools/repro-check.sh`).
- **Kernel flags (x86_64):** `--target=x86_64-unknown-none-elf -std=c17 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow` (add `-fstack-protector-strong` with an in-kernel canary once the basics work).
- **Build system:** plain Makefiles (KISS). No CMake or meta-build layers.
- **Formatting and linting:** `clang-format` with a checked-in style file; `clang-tidy` config checked in.
- **glibc toolchain policy (decided).** glibc has historically required GCC, and building it with Clang is not fully supported upstream. Parts of the Hurd and MIG-generated code also assume GCC extensions. The policy is:

  1. **Goal:** Clang everywhere, glibc included.
  2. **Budget:** Phase 0 spikes a Clang build of glibc and the Hurd and sizes the fix. If the work exceeds **one month of the Phase 7 budget**, the fallback triggers automatically.
  3. **Fallback (pre-committed, not re-litigated):** glibc is built with **GCC**; everything Joist owns — kernel, RS, DS, memory manager, MIG backend, new servers — stays Clang. Two compilers in the build is acceptable; an unbounded yak-shave inside the riskiest phase is not.

  The point of pre-committing is that the fallback is taken on a measurement, not on morale in month four of Phase 7. Record the measurement and the outcome as decision 0002.
- QEMU + GDB (`-s -S`) for kernel debugging; serial log as primary output.
- Cross toolchain: GCC/binutils for the Hurd target to build C userland.
- CI: build, host tests, QEMU boot test, IPC benchmark, fault-injection run on every merge.

---

## 14. Risks and mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Mach semantics leak through glibc/Hurd in unexpected places | Delays bring-up | Phase 0 inventory with `rpctrace`; decide mapping for every call before coding. |
| Scope creep into research redesign (the Hurd/L4 failure mode) | Project never ships | Fixed roadmap with exit criteria; "later" list for ideas. |
| Class C servers (proc, auth) cannot be made restartable in C | Resilience goal not met | Budget for clean-room C rewrites; resilience outranks reuse. |
| libports/libpager assumptions deeper than expected | Rewrite cost | Treat libpager as a rewrite from the start; spike libports early (Phase 5). |
| Performance worse than GNU Mach during bring-up | Morale, usability | Benchmark from Phase 3; compare against GNU Mach on the same workloads. |
| Solo-developer bandwidth | Timeline slips | Small milestones, each demonstrable; engage the Hurd community (bug-hurd mailing list, IRC) early. The roadmap states the realistic multi-year figure so slippage is measured against an honest baseline. |
| glibc/Hurd do not build cleanly with Clang | Delays Phase 7 | Phase 0 sizing spike against a fixed one-month budget; the GCC-for-glibc fallback is pre-committed (§13), so the decision costs no further deliberation. |
| Client-side recovery turns out to be the real cost of resilience | Resilience goal slips or ships hollow | Design it in Phase 0 (`docs/client-recovery.md`), land the module skeleton in Phase 7, not Phase 9; classify unrecoverable cases honestly rather than faking transparency (§7.3). |
| DS compromised or corrupted | Resilience lost; possible privilege escalation if DS were trusted | DS stores opaque, per-publisher-isolated blobs, holds no capabilities and no secrets, and publishers validate what they read back (§9.2). |
| No usable IOMMU on target hardware | Driver isolation claim is void | Detect at boot and report the downgrade explicitly; never assume isolation silently (§9.3). |
| SMP + no-runtime-allocation + bounded WCET prove mutually incompatible in practice | Redesign mid-Phase 3 | Settle as decision 0003 before Phase 3 starts; preallocated per-CPU pools and a documented lock order (§5.6). |
| Memory-safety bugs in C | Crashes, security holes | Coding standard, sanitizers, static analysis, fuzzing, and model checking in CI from day one; small kernel size keeps review feasible. |
| Upstream Hurd/glibc move on | Merge pain | Thin, well-separated patches; rebase regularly. |

---

## 15. Design journal template

Create one file per decision in `docs/decisions/NNNN-title.md`:

```
# NNNN — Title
Date:
Status: proposed | accepted | superseded by NNNN
Context: what problem, what constraints
Options considered: A / B / C with trade-offs
Decision:
Consequences: what this makes easier / harder
Priority check: does this respect the order Reliability > Resilience > POSIX > Reuse > Performance?
```

---

## 16. References to study

- GNU Hurd documentation and source (hurd, gnumach, mig, glibc Hurd sysdeps); Debian GNU/Hurd.
- "Towards a New Strategy of OS Design" (Thomas Bushnell) — original Hurd design rationale.
- Hurd-on-L4 / Viengoos notes and critiques (Walfield, Brinkmann).
- Minix 3 papers by Andrew Tanenbaum, Jorrit Herder et al. on reliability, the reincarnation server, and live update.
- QNX Neutrino System Architecture guide (message passing, priority inheritance, adaptive partitioning).
- Jochen Liedtke's L4 papers ("On µ-Kernel Construction", "Improving IPC by Kernel Design").
- seL4 papers and manual (capabilities, untyped memory, scheduling contexts, formal verification).
- The OSDev wiki (x86_64 bootstrap, paging, APIC, interrupts).
- MISRA C and SEI CERT C coding standards.
- Minix 3 and seL4 C source as references for disciplined kernel C.

---

## 17. Immediate next steps

*Status (2026-10-30): these were Phase 0's steps. All were done and Phase 0
closed on 2026-10-01 (`docs/phase0-exit.md`); Phases 1, 2, 3 and 4 are
closed too (`docs/phase1-exit.md`, `docs/phase2-exit.md`,
`docs/phase3-exit.md`, `docs/phase4-exit.md`). Phase 5 is next; its starting
points are the carried-forward lists in those exit records. The list is
kept as the record of how the project started.*

1. Set up Debian GNU/Hurd x86_64 in QEMU and build Hurd + GNU Mach + glibc from source.
2. Run `rpctrace` on boot, a shell session, and a package build; start `docs/mach-inventory.md`.
3. Write decision **0001**: native IPC model (sync + notifications, badge semantics, lifetime notifications).
4. Spike the Clang build of glibc and the Hurd against the one-month budget; write decision **0002** (§13).
5. Write decision **0003**: SMP, no-runtime-allocation, and bounded WCET taken together (§5.6). This blocks Phase 3.
6. Inventory file-descriptor state and draft `docs/client-recovery.md` (§7.3.6). This blocks Phase 4 and Phase 7.
7. Start the Phase 1 host-side C model with the sanitizer-enabled test harness.
8. **Register `joist` — repository and symbol prefix — before the first public commit.** The name was settled deliberately (decision **0000**): the earlier working name "Sherpa" was dropped because it collides with several existing software projects and because it borrows the name of a real people for a piece of infrastructure. Candidates were checked against the systems/OS space; `joist_` is short, unambiguous when spoken, not an existing pattern name or architecture, and makes a claim the kernel can actually keep. Register the repository, the `joist_` C symbol prefix, and the `libjoist*` library names together.
