# Phase 6 plan: fault routing, the paging protocol and libpager

**Status: accepted by the project owner, 2026-11-20,** with its proposed
choices: a pool of 4 helpers, at most one busy on any one backing server,
no timeout (choice 3); a dead server's cached pages kept (choice 6);
eviction by a scan of the tasks' maps, measured (choice 5).

Phase 6 of §11: *Paging protocol and libpager*.

## The goals this plan answers to

§1.1, in order. Every choice below ends with how it ranks against them.

1. **Reliability (QNX):** `mm` is one passive server every task depends
   on. Nothing in this phase may let a client, a file server or an
   exception handler stop it serving other tasks (0024, 0027).
2. **Resilience (Minix 3):** a file server that dies is a fault the
   program sees (`SIGBUS`), never a hang (0007), and it is settled here,
   not in Phase 9 (`docs/client-recovery.md`, the `mmap` row).
3. **POSIX:** `mmap` of a file, `MAP_PRIVATE` and `MAP_SHARED`, `msync`,
   `SIGSEGV` and `SIGBUS` with their codes.
4. **Reuse of the Hurd:** libpager's interface to its users
   (`pager_read_page`, `pager_write_page`, `pager_unlock_page`,
   `pager_report_extent`, `pager_notify_evict`) kept, so ext2fs's and
   storeio's pager code moves over unchanged in Phase 8.
5. **Performance:** a fault served from a file page, measured against a
   demand-zero fault and against Phase 4's figures.

## Inputs

- **The plan's Phase 6** (`joist-microkernel-plan.md` §11):
  - define the page-fault-to-server protocol that replaces Mach's external
    memory objects (§5.5);
  - split fault routing (0027): a task's pager gets page faults only and
    may pass one on (`JOIST_FAULT_PASS`, with a POSIX code) to the task's
    exception handler, which gets every other fault. Model first; `mm`
    moves to `task_set_pager`;
  - rewrite libpager against the protocol;
  - exercise it with a synthetic pager and Phase 4's `mm`, no filesystem
    server yet.
  - **Exit:** a synthetic userspace pager services faults correctly under
    stress, including concurrent faults, eviction, and pager death;
    libpager's test translator runs; a fault the pager passes on reaches
    the exception handler, and a hung exception handler doesn't stop the
    memory manager serving other tasks.
- **Decisions:**
  - 0007 (option C): the kernel knows one pager shape, a synchronous fault
    Call answered with resume. `mm` is the only pager the kernel talks to.
    File servers answer `mm` with ordinary requests; they never face the
    kernel. A dead backing server is `SIGBUS`, not a hang. No kernel page
    cache, no kernel-initiated pageout. Read-ahead left out of the first
    protocol.
  - 0027: two fault roles a task, `task_set_pager` and `JOIST_FAULT_PASS`,
    at most one pass; the model's tests, CBMC and mutations listed there.
  - 0024 §2 (accepted): file-backed regions in `mm`. A request that needs a
    file server goes to a helper thread of `mm`'s (the fault's reply saved
    with `reply_save`); a per-file page cache; `MAP_SHARED` dirty tracking
    and write-back; truncation and a dead server as `SIGBUS`. Left open:
    the helper's scheduling context.
  - 0022 (`mm` hands no frame to anyone), 0023 (`mm` passive), 0028 (limits
    per tree), 0029 (`mm`'s restart state), 0032 (bulk data through the
    caller's windows), 0001 (`reply_save`).
- **What exists:** `mm` serves anonymous memory only, from one passive
  thread, with no eviction and no helper threads. The kernel has one fault
  role per task (`task_set_fault`). libports, joistmig, libtrivfs and glibc
  run on Joist (Phase 5). The Hurd's libpager is 27 files, about 3 000
  lines, all `memory_object_*` underneath; the Hurd has no test translator
  for it, so this phase writes one.

## Choices this plan makes (proposed)

1. **Fault routing (0027, accepted) first, model first.**
   - `task_set_pager(task, slot, flags)`, one slot and a flags byte a
     task; `JOIST_FAULT_PASS` with a POSIX code in `r[1]`; at most one pass.
   - The model, its property tests, CBMC and mutations, then the kernel
     call; `mm` installs `MM_SLOT` as the pager at adoption, and replies
     `PASS` (`SEGV_MAPERR`, `SEGV_ACCERR`) where it now ends the thread.
   - glibc's signal thread is Phase 7. Here a test exception handler
     (a thread of the test's own) receives the passed faults.
   - *Priority check:* removes the only path by which a client could stall
     `mm` (0027).
2. **The paging protocol: `mm` and a backing server** (a new decision,
   0033, written at step 4 for acceptance; its shape proposed here).
   - **Mapping a file:** a client gets a *pager capability* for the file
     from its server (the Hurd's `io_map`, which returns memory object
     ports today), and passes it to `mm` in a new `MM_REQ_MAP_FILE`
     (address, length, offset, permissions, private or shared). glibc's
     `mmap` of a file becomes `io_map` then `MM_REQ_MAP_FILE`, where it was
     `io_map` then `vm_map` of the memory object.
   - **`mm`'s requests to the backing server:** read one page at an offset
     (the reply: the page, or past the end, or an error), write one page
     back, and the file's size. A page crosses in the IPC buffer (4 KiB,
     0001), the helper's buffer being the destination pool page, so the
     kernel's copy lands in place (0024). One page a request; read-ahead
     waits for a measurement (0007).
   - **Who faces whom:** the backing server never sees a client's fault
     or a kernel Call; `mm` never makes a synchronous call from its main
     loop to a backing server (0024). The kernel is unchanged by this
     protocol.
   - *Priority check:* one kernel-facing shape (0007); a backing server can
     slow only the faults on its own files (choice 3).
3. **`mm`'s helpers** (0024's option 1, its open point).
   - A fault on a file page that isn't cached: `mm` saves the fault's reply
     (`reply_save`), queues the request to a helper, and goes back to its
     loop. The helper calls the backing server, puts the page in the cache,
     maps it, and answers the saved reply (resume, or `PASS` with
     `BUS_OBJERR`/`BUS_ADRERR`).
   - **Proposed:** a fixed pool of helper threads (4), each with its own
     scheduling context (`mm`'s, budgeted), and **at most one helper
     busy on any one backing server**. A hung server then holds one
     helper, and its own files' faults wait in its queue; other servers'
     faults go on. No timeout on a backing server's reply (a slow disk is
     not an error); a dead one ends the call (`E_PEER`).
   - Rejected for now: time lent by the faulting client through a second
     endpoint (the client's context would be held by a server it didn't
     call, and `reply_save` already separates the fault from the work).
   - *Priority check:* bounded threads and contexts; one hostile server
     stalls only its own files.
4. **File pages in `mm`.**
   - **A page cache:** (backing server, file, page) → a pool page, shared
     by every mapping of that page; a bounded table.
   - **`MAP_PRIVATE`:** the cached page mapped read-only; a write copies
     it, as COW does now, and the copy is anonymous from then on.
   - **`MAP_SHARED`:** mapped read-only first; the first write fault marks
     the page dirty and maps it writable. `msync` (a new `MM_REQ_SYNC`),
     unmap and eviction write dirty pages back.
   - **Past the end of the file:** `PASS` with `BUS_ADRERR`.
   - **Accounting (0028):** a cached page is charged to the tree that
     first faulted it in; a mapping of a cached page charges nothing more
     until it copies.
5. **Eviction.** `mm` evicts file pages when its pool falls below a low
   mark: clean pages are dropped, dirty ones written back first (by a
   helper). A page is unmapped from every task that maps it. **Proposed:**
   found by a scan of the tasks' maps (bounded: `MM_MAX_TASKS` tasks), not
   a reverse map (a new record a page). Measured, and replaced if the scan
   costs too much. Anonymous memory is not evicted (swap is a default
   pager's job, later).
6. **A backing server that dies** (0007, `client-recovery.md`):
   - faults on its pages not in the cache: `PASS` with `BUS_OBJERR`;
   - pages already cached and mapped: **proposed:** kept and still
     served (clean ones from the cache); dirty ones can't be written back
     and are dropped at unmap or eviction, the loss counted. Phase 9's
     client recovery may later re-attach a region to a restarted server;
     nothing here stops that.
   - The death is seen through its capability's lifetime notification (I4),
     as `mm` sees a task's.
7. **libpager on Joist.**
   - Rewritten underneath, its users' interface kept: the pager capability
     is a libports port (a badge, 0005), its demuxer serves 0033's
     requests (read a page, write a page, the size) by calling
     `pager_read_page`, `pager_write_page`, `pager_report_extent`;
     `pager_sync`, `pager_flush` and `pager_return` become requests to
     `mm`; `pager_notify_evict` is called on `mm`'s eviction notice.
   - Joist's files laid over the Hurd's in `hurd/joist/libpager/`, as for
     libtrivfs and libports.
   - Functions with no meaning without Mach's memory objects
     (`pager_offer_page`, attributes, the precious-page protocol) are
     listed and refused or reduced.
8. **libpager's test translator** (new; the Hurd has none). A libtrivfs
   translator serving one file of a few MiB through libpager: its pages
   computed from their offset, writable, written pages kept in its memory.
   A test client maps it through glibc's `mmap` (`MAP_PRIVATE` and
   `MAP_SHARED`), reads, writes, `msync`s, and checks what a second
   mapping and the translator see. Killed mid-run: the client gets
   `SIGBUS`-shaped passes (no signals until Phase 7: the test's exception
   handler sees them).
9. **The synthetic pager** for the stress test: a backing server of
   Joist's own (no libpager), with fault injection: slow pages, errors,
   short files, death at a chosen request. Clients on 1, 2 and 4 CPUs
   fault concurrently on shared and private mappings while the pool is
   kept small enough to force eviction.

## Testing

| What | How |
|---|---|
| Fault routing (0027) | the model first: unit tests for every route (0027's list), the invariant (a thread in at most one fault Call; a passed fault never returns to the pager), property tests with `task_set_pager`, CBMC on the changed fault paths, 0027's four mutations; the kernel's ABI test; user tests: `SEGV`-shaped passes reach a handler thread through `mm`, and a hung exception handler while `mm` serves other tasks |
| `mm`'s file pages | the refinement test's abstract machine gains files (a page's value is the file's until written; a shared write reaches the file at sync); host tests with a simulated backing server; mutations (a dirty page dropped silently, a private write reaching the file, a page past the end served, a cached page charged twice) |
| Helpers | host tests: a hung backing server holds one helper, other servers' faults finish; a dead one ends its faults with `BUS_OBJERR`; never more than one helper busy on one server |
| Eviction | clean and dirty pages under a small pool; every mapper unmapped; a dirty page written before it goes |
| The synthetic pager, on Joist | stress on 1, 2 and 4 CPUs, emulated and under KVM: concurrent faults on one page and many, eviction forced, the pager killed mid-run, fault injection seeds |
| libpager and its translator | the test client through glibc's `mmap`; killed mid-run |
| Measured, recorded | a file-page fault (cached; from the server) against a demand-zero fault; eviction's scan |
| Phase 5's suites | all of them, unchanged, in every full run |

## Order of work

1. ~~**This plan,** accepted by the owner.~~ Done 2026-11-20.
2. ~~**Fault routing in the model:** `task_set_pager`, `PASS`, the
   invariant, property tests, CBMC, mutations.~~ Done 2026-11-21
   (*Progress*).
3. ~~**Fault routing in the kernel and `mm`:** the ABI call, `mm` as the
   pager, `PASS` for what it now ends; the user tests, including the hung
   exception handler.~~ Done 2026-11-21 (*Progress*).
4. ~~**Decision 0033, the paging protocol** (choices 2-6 written out), for
   the owner's acceptance.~~ Accepted 2026-11-21 (*Progress*).
5. **File pages in `mmlib`, host side:** the page cache, private and
   shared mappings, write-back, eviction, a dead server; the refinement
   test with files; mutations.
6. **`mm` on Joist with helpers;** the synthetic pager and the stress
   test.
7. **libpager on Joist,** and glibc's `mmap` of a file.
8. **libpager's test translator.**
9. **Exit:** the exit run, the measurements, the record.

## Risks

- **`mm` grows.** Helpers, a page cache, write-back and eviction roughly
  double it. Its restart (0029) must still be possible: each new record is
  listed against 0029 as it is made.
- **`reply_save` in a passive server with helpers** has not been used on
  this scale. The saved replies are bounded (one a faulting thread), but
  a fault whose task dies while a helper works on it must be answered
  into nothing, not leaked.
- **Eviction's scan** may be too slow with many tasks; the fallback, a
  reverse map, is a record a cached page.
- **libpager's internals** assume Mach's asynchronous memory-object
  protocol (precious pages, lock requests). Phase 8's ext2fs depends on
  what is kept; anything dropped is listed now.
- **Bare metal** is still not available; every real-CPU figure is under
  KVM on the P53.

## Accepted (2026-11-20)

The plan as written, with its three proposals the owner was asked about:
the helpers (choice 3), a dead server's cached pages (choice 6) and
eviction's scan (choice 5).

## Progress

### Step 2: fault routing in the model (2026-11-20 to 2026-11-21)

Decision 0027 built in the kernel core, as accepted (by a P53 hand-off,
`/srv/joist/handoff/p6-routing`); the choices inside it are the
implementation's, so *(changeable)*.

- **Built:** `joist_task_set_pager(k, cs, task_slot, slot, flags)`, task
  level only, checked as `task_set_fault` (a slot in the task's own space
  or none; `NO_THREAD_CAP` the only flag); one slot and a flags byte in the
  space record. A page fault goes to the pager if its slot names an
  endpoint with SEND, alive; otherwise (no pager, an empty slot, a dead
  endpoint) and for every other kind, to the exception handler (the
  thread's, then the task's), as before.
- **PASS:** `JOIST_FAULT_PASS` (2) from the pager, with a code in `r[1]`.
  Nothing else of that reply reaches the thread (no words, no
  capabilities); the thread wakes, its context back, and the kernel makes
  the fault Call to the exception handler: the same `r[1]`-`r[3]`
  (address, architecture code, pc), and in `r[0]` beside kind and access
  `JOIST_FAULT_PASSED` (bit 16) and the code's low 32 bits (bits 32-63:
  four message registers, all used). At most one pass: a PASS from the
  exception handler, passed or not, ends the thread like any verdict but
  RESUME. No usable exception handler: the task ends (the root's thread
  alone), 0009's rule. Both calls on the faulting thread's context.
- **What the thread record keeps:** the route of its fault call (1 byte,
  in existing padding) and the fault as first sent (4 words): the
  kernel's thread record 384 → 416 bytes, the space record +8; 4 608
  bytes in all at the kernel's 128 threads and 64 spaces.
- **Invariant (`joist__check_ipc`):** faulting exactly when on a route;
  the pager's and a passed call are page faults; the passed mark exactly
  on the passed route. "A passed fault never returns to the pager": a
  per-step check in the property executor and an assertion in CBMC (a
  thread on the passed route before an operation is never on the pager's
  after it).
- **Tests:** 16 unit tests (`test_fault.c`), one for each route 0027
  lists and the edges (a PASS reply's capabilities not delivered; the
  handler dead meanwhile; the root; a pass queued, and left unanswered:
  suspended; through a saved reply; both hops charged to the faulting
  thread; a pager in the faulting task passing with no handler; one
  endpoint in both roles). The property executor gains `task_set_pager`,
  page faults half the time, PASS verdicts from `reply`, `reply_saved` and
  `reply_recv`, and checks of each fault's route and each pass's code;
  floors: 557 faults to a pager, 36 passes, 16 that ended the task, 29
  PASS from an exception handler in the full run. 323 tests in each of the
  six builds of `make -C model check`.
- **Mutations:** 0027's four (a second pass allowed; a page fault to the
  exception handler with a pager set; another fault to the pager; the
  code dropped) and 12 more (`route-*`), 3 for `mm`'s codes; four older
  ones re-pointed at the moved code. `make -C model mutate`: 391 of 391.
- **CBMC:** operations 30 (`task_set_pager`, 418 s) and 34
  (`thread_abort`, 940 s) verify on joist-dev2, with two threads and no
  IPC buffers; each operation keeps every invariant, the 0027 one
  included, and no thread goes from the passed route to the pager's. 31-33
  (`joist_fault`, and `reply` and `reply_saved` with any verdict) are in
  the harness but **did not fit** joist-dev2's 11 GB: with three threads,
  two, one space, the threads fixed (`JOIST_CBMC_FIXED`), split by the
  fault's route (`31/r1`-`r3`), one thread, and the root's space fixed so
  no task can be destroyed, each run was killed while CBMC built its
  formula (425 to 3 479 s). Left out of `CBMC_OPS` and to the unit and
  property tests and the mutations, as 3, 6 and 19 are. Tried again on
  the laptop (23 GB, capped at 20 GB), the owner's choice: op 31 ran out
  of memory in 209 s, op 32 passed 16 GB within half an hour, and the
  owner stopped them (2026-11-21): **31-33 are a known limit**, beside
  Phase 5's 3, 6 and 19. Found on the way: a first op-31 result in
  216 s meant nothing, since my 4-word copy loop failed its unwinding
  assertion and CBMC cuts every path past one (the loop is unrolled now).

### Step 3: fault routing in the kernel and `mm` (2026-11-20 to 2026-11-21)

- **The call:** system call 89, `task_set_pager` (task, pager slot,
  flags), `docs/abi.md`, `j_task_set_pager` in `user/lib/joist.h` with
  `J_FAULT_PASS`, `J_FAULT_PASSED`, `J_FAULT_CODE` and the codes; a host
  test (`abi_task_set_pager_takes_its_registers`), the user library's
  numbers checked against the kernel's, the ABI fuzzer's list, 2 kernel
  mutations. `run.c`'s root-task check counts a pager for page faults.
- **`mm`:** installs `MM_SLOT` with `task_set_pager` at adoption; answers
  PASS with `SEGV_MAPERR` (outside every region) or `SEGV_ACCERR` (a
  guard, or an access the region forbids), decided by `mm_fault_refusal`
  in `mmlib` (one host test, 3 mutations). A fault it would serve but
  can't (no page: the tree's ceiling, the pool, the kernel refusing)
  passes `SIGBUS`/`BUS_ADRERR` (the owner, 2026-11-21).
  `MM_REPORT_REFUSED` prints passed faults with their code.
- **What changed for the programs that relied on `mm` ending a thread:**
  with no exception handler the task now goes where the thread went.
  `mmroot`'s read-only write, outside and guard cases now check the task
  is gone and `mm` forgets it. `faultroot` doesn't use `mm` (unchanged).
  `cowstress` makes no access `mm` refuses (its runs pass unchanged).
  `glibcroot`: a program a passed fault ends is gone at once (its thread
  count falls: "the program ended", "no exit status", as for a thread
  ended before), so a multi-threaded program no longer lingers to TIMEOUT
  with its other threads; comments updated; no glibc change (no exception
  handler until Phase 7).
- **User tests** (`mmroot`, run on 1, 2 and 4 CPUs emulated, on 1, 2 and 4
  under KVM, and on the checking kernel): "passed": a client's three bad
  accesses (outside, a write to a read-only region, a guard) reach a
  handler of the root's own with kind, the passed mark, the code, address,
  access and pc; the handler rewrites the registers (past the 3-byte
  access, RAX set) and resumes; faults `mm` serves never reach it. "hung
  handler": a handler that takes the passed fault and keeps it
  (`reply_save`) for ever, while `mm` serves a fork's demand-zero and
  copy-on-write faults and a basic client; then the client goes and `mm`
  forgets it.
- **Runs:** `make -C kernel check` (joist-dev2: the P53 has no Limine) all
  passing, 103 selftests, 33 artifacts reproducible; `make -C kernel
  mutate` 63 of 63; `make -C model check` and `mutate` whole; `make -C
  glibc/test check`. glibc not rebuilt (no glibc file changed).
- **Found:** my first user test left the root's receive slots pointing at
  its handler's capability slot, so a later fork's child capabilities
  missed; and each client costs the root its image's pages for good, so
  the hung test runs a basic client, not a steady one, to leave the trees
  test its pages. Three 4-CPU KVM timing tests failed once beside a CBMC
  proof and mutation jobs; quiet, they pass on this tree and on 25dbe81.
- **The owner's answers (2026-11-21):** glibc's codes alone don't name the
  signal (`SEGV_ACCERR` and `BUS_ADRERR` are both 2), so a pass's code
  carries glibc's `si_code` in bits 0-15 and the signal in bits 16-23
  (`JOIST_PASS_CODE`, `JOIST_PASS_SIGNAL`, `JOIST_PASS_SI_CODE`; the kernel
  carries it untouched); and a fault `mm` would serve but can't passes
  `SIGBUS`/`BUS_ADRERR`, so with no handler the task ends, not the thread
  alone. Applied in the review.

### Step 4: decision 0033, the paging protocol (2026-11-21, accepted)

`docs/decisions/0033-the-paging-protocol-mm-and-backing-servers.md`: a
pager capability a file (`io_map`'s result) passed to `mm` in
`MM_REQ_MAP_FILE`; five requests from `mm`'s helpers (`PG_HELLO`,
`PG_READ`, `PG_WRITE`, `PG_UNLOCK`, `PG_EVICTED`), one page each through
the IPC buffer pointed at the pool page; three from the server
(`MM_REQ_PG_SYNC`, `FLUSH`, `SIZE`) on a capability `mm` badges a file;
the page cache, private and shared mappings, eviction and a dead server as
the plan chose; passes with signal and code (the owner, 2026-11-21: glibc's
code in bits 0-15, the signal in 16-23; a fault `mm` can't serve for want
of pages passes `SIGBUS`/`BUS_ADRERR`). The owner's choice: `cap_read`
returns an endpoint capability's identity (object index and generation),
so `mm` can tell two capabilities to one file or one server apart: a small
kernel change, model first, in step 5.
