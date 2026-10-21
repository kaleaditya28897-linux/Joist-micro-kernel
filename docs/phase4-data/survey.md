# Phase 4 kernel-side design survey

For the session drafting `docs/phase4-plan.md`. Research only; no tree file
changed. Read: plan §5.5, §5.7, §7.3.3, §8.1, §9, §11 Phases 4-7;
`docs/abi.md` (*Frames and mappings*, *Thread control and faults*, *Open*);
decisions 0003, 0006, 0007, 0008, 0011, 0013; `docs/phase3-exit.md`;
`docs/client-recovery.md` §4; `kernel/core/{map,cspace,fault}.c`,
`internal.h`, `include/joist/core.h`, `arch/x86_64/run.c` (pool, hooks,
shootdown), `mem/vm.h`.

**Provenance.** Anything about Joist cites the tree. What I say about other
systems comes **from memory, with no sources checked in this session**.
File names and call names are my best recollection. **(unsure)** marks the
ones I trust least. Nothing about other systems was looked up.

---

## Summary of recommendations (one page)

**Facts in today's tree that drive everything below.**
- The kernel's tables are global and fixed: `JOIST_MAX_OBJECTS` 1024, `JOIST_MAX_MAPPINGS` 512 (`kernel/Makefile` `KSIZES`), 16 spaces, 8 192 slots per space, a pool of 64 leaves.
- `map` and `unmap` scan the whole mapping table, and `new_frame` scans the whole object table: O(table) per call.
- A mapping is a whole frame of at most `JOIST_MAP_SPAN` = 512 pages that stays inside one page table. So a frame larger than 2 MiB cannot be mapped at all.
- `frame_split` needs `refs == 1` and the frame unmapped.
- A mapping costs one quota unit, but the real page tables come from a kernel pool reserved at boot: `POOL_FRAMES` = 16 + 3×512 + 8 = 1 560 frames.
- Record sizes, measured with a probe built against `core.h`: object 128 B, capability 16 B, mapping 32 B.
- A capability records `{obj, type, rights, badge}` and nothing else. There is no parent link, so nothing records derivation.

| # | Question | Recommendation |
|---|---|---|
| 1 | Page-granular COW | **Map a sub-range of a frame** (`map(frame, page offset, n ≤ 512, task, va, perms)`). Replace the global mapping table with the page tables plus a **per-page-table shadow page** (frame id and rights ceiling per PTE) and a per-frame **map count**. `mm` keeps a few large frames and does page allocation and COW reference counts itself. Reject one frame per page: 1 GiB would need 262 144 objects (32 MiB), caps and mappings, against tables of 1 024/512. |
| 2 | Kernel calls | Add **`unmap_range`** (holes allowed), **`protect`** (a range, any W^X-legal perms up to the rights ceiling), a **conditional replace** in `map` (expect-old), and a bounded **`map_query`**. Each call covers at most 512 pages / one page table and does **one shootdown per call**. **No kernel frame copy**: `mm` keeps the memory it manages mapped in its own space (no execute) and copies with `memcpy`. |
| 3 | Revoke, batched copy | **No seL4 CDT in Phase 4.** Add **revoke-by-kill** for the objects that need it (the region capability in Q5), like an endpoint's death: O(1), every copy then fails `E_GONE`. Mach has no descendant revoke, and the Hurd does not need one. Add **`cap_copy_batch`** (task cap, first slot, count ≤ 32, mask), which copies to the same slot numbers, never allocates (derive doesn't), and returns a cursor. Keep a CDT (seL4 MDB, +8 B per slot) as a recorded option for when RS needs revoke-descendants. |
| 4 | Who pays | **Donated frames for page tables**: `space_donate(task, frame)` turns a frame into page-table and shadow pages for that space. The kernel holds a reference until the space dies. `mm` donates at task creation and when a space grows, so in memory terms **mm pays**. Quota units stay for objects. The boot pool shrinks to PML4s and the root. Fallback if this is too much for Phase 4: make the quota unit honest (1 unit = 1 page-table page) and size the pool at boot. |
| 5 | 0008 escape hatch | **A region capability** (Zircon VMAR-like): `{space, [lo,hi), backing frame, perms ceiling}`. Its holder may map, unmap and protect pages **of the backing frame** inside the range and nothing else. `mm` keeps the frame cap, and `region_kill` revokes in O(1). Close today's hole: `JOIST_SLOT_SELF` lets any task holding a frame cap map it anywhere in itself, and **unmap any mapping in itself, mm's included**. |
| 6 | Grants and 0006 | In Phase 4 a shared grant is **mm mapping the same pages twice**; there is no new kernel object. So that Phase 5's move-grant isn't undone: page identity sits in each PTE (shadow), COW sharers are mm's knowledge only, and the region capability is shaped so it can also be a **receive window**. The Phase 5 risk: a kernel move-grant of a page `mm` has COW-shared. Rule it out by allowing moves only within region ranges, or by moving through mm. |
| 7 | COW fault path | One fault IPC per write fault, plus one fused **"map-and-resume" reply** (0007's shape). Today it costs an extra `map` call, and an extra `cap_delete` for the thread cap that every fault puts in mm's slot. Add **fault-around** for reads and demand-zero (one `map` of up to 16 pages). Don't pre-break COW in the first version. A permission upgrade where the reference count is 1 needs no shootdown (x86 #PF drops the entry). A **frame replacement needs a shootdown** of the CPUs in that space. Make the handler idempotent: the evict/restart rule refaults. |
| 8 | Testing | Make the core property a **refinement check**: every read returns what an abstract model, in which `fork` deep-copies, says. Check it host-side (model + mm reference) and in-kernel (`cowstress`: fork trees, page tags, SMP writers). Use TLA+ for the stale-TLB and parent-downgrade-order race, CBMC on map/unmap/protect, mutations (drop the shootdown, child mapped before the parent downgrade, reference count off by one), and growroot-style counters. |

**Order of work suggested by dependencies:** (1) sub-range map + shadow +
map count; (2) protect, unmap_range, query, conditional map; (3) fault reply
with map; (4) cap_copy_batch; (5) mm with direct map, demand-zero, COW;
(6) region capability; (7) donation (could slide to 3 if the pool is made
honest first).

---

## 1. Page-granular COW when `map` maps a whole frame

**Problem.** COW needs per-page control: share page P read-only in two
spaces, then give one of them a private copy. Today the unit of mapping is
the whole frame (`joist_map`, I12 "every mapping maps a whole live frame").
`frame_split` refuses a mapped or shared frame, so `mm` would have to split
everything down to pages before the first map.

**Prior art (from memory).**

| System | Granule of kernel record | How COW works |
|---|---|---|
| seL4 | One frame cap per mapping. The cap holds the physical address and its mapped ASID/vaddr; there is **no separate object record** for a frame. Mapping in a second vspace needs `seL4_CNode_Copy` first. `seL4_X86_Page_Map/Unmap`. Page tables are objects the user retypes from untyped (`seL4_X86_PageTable_Map`). | Not in the kernel. User-level libraries (sel4utils `vspace`, a shadow table in user space **(unsure)**) remap read-only and copy on the fault. |
| Fiasco.OC / L4Re | Flexpages (2^n-aligned ranges) in map/grant items. The kernel **mapping database** (`src/kern/mapdb.cpp`, a tree per physical page or superpage) records who mapped what from whom, so unmap is recursive. | The region mapper (in each task) plus dataspaces (Moe). A COW dataspace exists in Moe? **(unsure)**. L4Linux does its own. |
| NOVA | Memory capability ranges delegated with typed items; a mapping database node per delegation (`mdb.cpp` **(unsure)**); revoke is recursive. | User level (the VMM or Genode core). |
| Zircon | The VMO's page list (`VmPageList`, nodes of 16 pages) on the kernel heap; `vm_page_t` per physical page, preallocated about 1.5 % **(unsure on the figure)**; mappings per `VmMapping` (one per region). | In the kernel: `zx_vmo_create_child(SNAPSHOT / SNAPSHOT_AT_LEAST_ON_WRITE)`. It was hidden VMO parents (`VmObjectPaged`), reworked into `VmCowPages` around 2020-23 **(unsure on dates)**. |
| Minix 3 | The VM server writes page tables itself, mapping them into its own space (`servers/vm/pagetable.c`). Physical blocks have reference counts (`phys_block`, `region.c`). | In VM: `fork.c`/`region.c` mark pages read-only and copy on `VM_PAGEFAULT`. |
| GNU Mach | `vm_map_entry` per region; `vm_object` per backing; `vm_page` per resident page; pmap per space (`i386/intel/pmap.c`). | Shadow chains (`vm_object_shadow`, `vm_fault_page` walking the chain, `vm_object_collapse`). Chains are known to grow without bound and to be hard to collapse. This is what 0011 rejects. |

**Options for Joist.** Cost for one fully touched 1 GiB space (262 144
4 KiB pages), using today's measured record sizes:

| | A: one frame per page (`frame_split` to pages) | B: map a sub-range of a frame (**recommended**) | C: kernel "page" object (cap per mapping, seL4-like) |
|---|---|---|---|
| Frame objects | 262 144 × 128 B = **32 MiB**; table today 1 024 | ~512 (2 MiB frames) or fewer: 64 KiB | 0 extra if the cap holds the address, but Joist caps hold an object index |
| Capability slots in mm | 262 144 (4 MiB). The space limit is 8 192, so **32× over**; the leaf pool is 64 | ~512 | 262 144 per space mapped, **×2 after fork** |
| Mapping records | 262 144 × 32 B = 8 MiB; table today 512 | none: the page tables are the record, plus a shadow page per page table (4-8 B per PTE) ≈ **2 MiB + 2 MiB per GiB per space** | none (in the cap) |
| Per-call cost | `new_frame`, `map` and `unmap` scan their tables, so 262 k iterations per call unless rebuilt | O(n pages) PTE writes and ≤ 3 table allocations | O(1) per page; no batching |
| Splits | `frame_split` must happen before any mapping (refs 1, unmapped) | none at run time | n/a |
| Frees | each page's last cap → `joist__frame_freed` scan | map count 0 → O(1); otherwise a preemptible sweep (below) | delete the cap → unmap that one mapping |

**B in detail.**
- `map(frame, off, n, task, va, perms)`, with `off + n ≤ frame.npages` and the run inside one page table (keep `JOIST_MAP_SPAN`). Rights are checked against the frame cap as today, W^X per call.
- The global `map[]` table is replaced by the **page tables themselves**, plus one **shadow page per page table**. A shadow entry holds the frame object index (32 bits) and the rights ceiling (WRITE allowed?) for its PTE. Overlap checking becomes "PTEs empty" (O(n)), and `unmap` finds the frame from the shadow.
- **Per-frame `mapcount`** = PTEs naming it. Deleting a frame's last cap with mapcount 0 is O(1).
- With mapcount > 0 (rare: mm died, or a bug) the frame becomes *dying*. It joins the destruction work list as a **sweep of every space's shadow pages**, one page table per unit of work, using the existing cursor, `continue` and idle machinery (0016). Then it is freed.
- Alternative if that sweep proves too slow: a reverse-map list threaded through the shadow (prev/next as 32-bit global PTE ids, 12-16 B per PTE). This makes the unmap O(mappings of that frame), at 2× the shadow memory.
- I12 becomes: every present user PTE names a page inside a live (or dying, while swept) frame; the shadow agrees with the PTE; Σ mapcount = present PTEs; W^X; user half only.
- `mm` then runs its own page allocator over its large frames, and keeps the per-page COW reference counts (Minix's `phys_block` shape).

**Why not A:** it multiplies three global tables by about 256 and needs
`frame_split`'s O(objects) scan 262 k times. It moves the problem rather
than solving it. **Why not C:** it requires a cap per mapping (seL4 pays
this, but its CNodes come from untyped and its frame caps need no object
record). Joist's slot limit and object record make it the most expensive.

## 2. The kernel calls Phase 4 needs, and their bounds

**Prior art (from memory).**
- **seL4:** `Page_Map` (remapping the same cap with fewer rights is its *protect*), `Page_Unmap`, `Page_GetAddress`, `PageTable_Map/Unmap`. No range calls (one call per page) and no copy call.
- **Fiasco:** `l4_task_map` / map items in IPC; `l4_task_unmap(fpage, L4_FP_ALL_SPACES | OTHER_SPACES)`, which is recursive through the mapdb and **reports accessed/dirty bits** back **(fairly sure)**. Protect is unmap of the W right with a flexpage. No copy call.
- **Zircon:** `zx_vmar_map/unmap/protect` over ranges, `zx_vmo_op_range` (commit, decommit, zero), `zx_vmo_read/write` (the kernel copies), `zx_object_get_info(ZX_INFO_VMAR_MAPS)` to read back.
- **Minix:** VM writes PTEs directly, so its kernel needs no map calls. The kernel's `SYS_VMCTL` and `VMCTL_MEMREQ` let the kernel ask VM to make memory present for a kernel copy **(unsure on names)**. `sys_vircopy`/`sys_physcopy` copy.
- **Mach:** `vm_protect`, `vm_copy`, `vm_read/write`, `vm_region`: all in the kernel and over ranges.

**Proposed set** (every call ≤ 512 pages = one page table, one shootdown):

| Call | Arguments | Effect | Bound | Notes |
|---|---|---|---|---|
| `map` (extended) | frame, page offset, n, task (or region), va, perms, **flags: REPLACE, expect** | install n PTEs. With REPLACE, overwrite present PTEs, but only if each matches `expect` (absent / RO-same-frame), else `E_STATE` and no effect | n PTE writes + ≤ 3 table allocations + one shootdown if anything was replaced | "Conditional replace" makes a multithreaded mm's fault handling race-free without mm locks over the kernel call (Dirty COW-class races) |
| `unmap_range` | task (or region), va, n | clear present PTEs; holes are fine; map counts drop | n + table frees + one shootdown | replaces `unmap(va)`'s "a mapping starts here" |
| `protect` | task (or region), va, n, perms | set perms on present PTEs. `E_RIGHTS` if any page's ceiling lacks WRITE and W is asked; W^X is checked per call | n + one shootdown (none for a pure upgrade) | fork downgrades the parent with 1 call per 2 MiB: 512 calls per GiB. Consider allowing up to k page tables per call once measured |
| `map_query` | task (or region), va, n ≤ 64, user buffer | per page: present, perms, frame id (only if the caller holds a CONTROL cap on that frame), page offset | n | needed by 0008's "mm's view matches the kernel" test, by fork of region ranges (Q5) and by mm's restart (class C). Doesn't reveal physical addresses |
| `space_donate` | task, frame | Q4 | O(1) | |

**Frame copy: not in the kernel.**

| | Kernel `frame_copy(src, off, dst, off, n)` | mm maps both into itself per copy | **mm keeps its managed memory mapped (recommended)** |
|---|---|---|---|
| Kernel work | n × 4 KiB through the kernel's direct map (HHDM exists): bounded but ~1 µs per page inside the one lock | 2 maps + 2 unmaps, **each unmap a shootdown** to mm's other CPUs | none |
| Per COW fault | 1 call | 4 calls + IPIs | `memcpy` |
| Cost | kernel code, and lock hold time grows with n | slow on SMP | mm's address space must cover managed memory: at most 47 bits, fine; RW, never X. §9.1 already says mm reads and writes all user memory |

The same applies to zeroing for demand-zero.

## 3. Revoke and the batched capability copy for `fork`

**What Joist records today** (`cspace.c`): `copy_cap` (derive, mint),
`cap_give` and `joist__cap_grant` each copy `{obj, type, rights, badge}`
and increment `obj.refs`. `joist__cap_delete_slot` clears the slot, runs
`cap_removed` (no-senders, endpoint death) and `obj_unref`, which frees the
object and refunds its donor at zero, possibly cascading. **There is no
parent pointer, sibling link or generation.** "Find every copy of X" is
therefore a scan of every slot of every space (16 × 8 192). Revocation today
exists only as object death: an endpoint dies with its last RECV, and tasks
and threads are destroyed. That is Mach's model (destroying a receive right
makes dead names).

**seL4, from memory.**
- Every slot (CTE, 32 B on 64-bit) carries an MDB node: `mdbPrev`, `mdbNext`, `mdbRevocable`, `mdbFirstBadged`. The CDT is a **doubly linked list in preorder**. Depth isn't stored: `isMDBParentOf` decides it from object identity, badge and the flags.
- **Revoke** (`cteRevoke`, `src/object/cnode.c`): while `next` is a child, `cteDelete(next)`, then `preemptionPoint()`. When preempted, the system call returns `seL4_Preempted`-style and is restarted. Progress is kept in the list itself (deleted children are gone), so no continuation state is needed.
- `isFinalCapability` is O(1) because copies are list neighbours. Deleting a CNode uses *Zombie* caps (`reduceZombie`), so the cascade is iterative and preemptible.
- Untyped revoke also resets the untyped's watermark, so memory is reusable.
- Bounded per step; the total is bounded by the number of descendants.

**Others, from memory.** Fiasco and NOVA revoke recursively through the
mapping database (per page or per capability node), preemptibly
**(unsure how NOVA preempts)**. Zircon has no revoke: handles are closed,
and `zx_vmar_destroy` or `zx_task_kill` stand in. Mach has none beyond
receive-right destruction.

**Options.**

| | A: seL4 MDB in every slot | B: revoke-by-kill per object (**recommended for Phase 4**) | C: revoke as a global slot sweep |
|---|---|---|---|
| Record | +8 B per slot (two 32-bit global slot ids), so the cap grows 16 → 24 B; every copy and delete keeps the list | a `dead` flag on the object (endpoints, tasks and threads have it already) | none |
| Revoke | descendants only, preemptible on the work list | **all** copies fail `E_GONE` at once, O(1). Slots stay occupied until deleted (the I1 shape, like dead endpoints) | delete every copy, preemptible, O(all slots) |
| Fits 0003 | yes | yes | yes, but slow |
| Needed by | RS revoking what it delegated through a server | Q5's region cap; frames for later Phase 5 move-grant windows | rare cleanup |
| Risk | touches every cap path; model, CBMC and TLA+ all need it | a dead object's unit stays charged to its donor until the holders delete their copies: the same DoS shape dead endpoints already accept | WCET of the whole sweep |

**Batched copy for `fork`** (0011: "bounded batches"). Hurd `fork`
recreates the same port names in the child (`client-recovery.md` §4, from
`_Fork.c`), so slot numbers are kept:

`cap_copy_batch(task cap (CONTROL), first, count ≤ 32, mask, flags)`
- Copies each non-empty slot `s` in `[first, first+count)` of the caller's space into slot `s` of the task, with rights ∩ mask. Badges are kept.
- **Skips** caps without GRANT, and RECV caps unless asked (Mach does not inherit receive rights; glibc makes new ones). It returns a bitmap of what it skipped.
- Stops at the first occupied target slot (`E_OCCUPIED` + the cursor) or absent leaf (`E_RANGE` + the cursor; the parent `cspace_grow`s the child's leaves first and pays for them).
- `E_DEAD` if the child is dying (TLA+'s rule, `IpcDelete.tla`).
- Never allocates: a derive only increments refs, and badge accounts already exist for any badge already held.
- Bound = 32 slots = `RUN_PREEMPT_SLOTS`, the same piece size as destruction. Atomic per batch only: the parent's other threads must be quiesced by glibc. Whether Hurd glibc suspends other threads in `fork` is **unsure**; check `sysdeps/mach/hurd/fork.c`.

## 4. Who pays for page tables and kernel objects

**Today.**
- `map` charges **the target space** one quota unit per mapping (`ts->quota--`), standing for page tables.
- The tables actually come from `pool[POOL_FRAMES]`, reserved at boot and sized to the worst case (3 tables per mapping).
- Under B, a 1 GiB space needs ~514 page-table pages + ~512 shadow pages ≈ 4 MiB. Sixteen such spaces ≈ 64 MiB, which can't be a compile-time reserve.

**Prior art (from memory).**
- seL4: the user retypes untyped into page tables, CNodes and TCBs. Whoever holds the untyped pays, and revoking the untyped reclaims everything.
- Fiasco: a kernel allocator plus per-task `Ram_quota` charged on kernel allocation **(fairly sure)**.
- NOVA: a kernel buddy allocator. Genode's NOVA added per-PD kernel memory quotas **(unsure)**.
- Zircon: the kernel heap, with no per-process kernel memory accounting (a known gap) **(fairly sure)**.
- Minix: VM allocates page tables from its own memory, so VM pays.
- Mach: the kernel zones, which is exactly the exhaustion §2 rejects.

| | A: honest quota (1 unit = 1 page-table page), pool sized at boot | **B: donated frames** (**recommended**) | C: mm's quota pays (caller-pays) |
|---|---|---|---|
| Change | count units at `walk_create`; size the pool from boot memory, not from `JOIST_MAX_MAPPINGS` | `space_donate(task, frame)`: the frame becomes kernel-use (not mappable or splittable; the kernel holds a ref) and feeds that space's page-table and shadow pages; it goes back to the donor's ordinary frame when the space dies | charge the caller of `map`; record the payer in the shadow for the refund |
| Who decides | the kernel's boot policy (a percentage of RAM) | mm, from the memory it already owns | mm |
| I3 | as today | memory conserved by construction: the pages never leave a frame object | needs a cross-space refund |
| Risk | the pool is still a global, kernel-chosen limit | a new frame state; a page table that runs out means `E_NOMEM` at `map`, so mm must donate ahead (it knows its own layout) | mm's quota becomes a global bottleneck |

Kernel objects (frames, regions) stay on unit quotas: under Q1-B mm needs
hundreds of frame objects, not hundreds of thousands, so the 1 024-entry
object table is enough for Phase 4. The plan's §5.1 already allows
"memory explicitly donated by a task", and `phase3-exit.md` lists donated
memory as reconsidered in Phase 4.

## 5. The 0008 escape hatch: a capability over an address range

**Prior art (from memory).**

| System | Shape | Revocation |
|---|---|---|
| Zircon | **VMAR handles**: `zx_vmar_allocate` (sub-region, with rights `CAN_MAP_READ/WRITE/EXECUTE`), `zx_vmar_map(vmar, vmo, off, len)`, `unmap`, `protect`. The kernel does placement, with ASLR. | `zx_vmar_destroy` unmaps everything and makes the handle useless |
| L4Re | The **region mapper lives in each task** (`l4re_kernel`, `region.cc` **(unsure on path)**) as that task's pager. `Rm::attach` is in-task, and pages come from dataspaces (`Dataspace::map` sends flexpages). | the dataspace provider unmaps (mapdb, recursive) |
| seL4 | Give the application untyped + its vspace caps: it self-pages (CAmkES, Genode on seL4) | revoke the untyped (CDT) |
| Genode (core) | Region maps over RPC; *managed dataspaces* nest them | core detaches |
| Minix, Hurd/Mach | none: an RPC to VM, or a kernel call | — |

**Joist today.** `joist_map`/`joist_unmap` with `task_slot ==
JOIST_SLOT_SELF` skip the task-capability check. Any task holding a frame
cap can map it anywhere in itself, **and can unmap any mapping in itself,
mm's included**. That is an unbounded escape hatch already, and it breaks
0008's "mm's view matches the kernel" unless mm treats the page tables as
the truth.

| | A: region capability (**recommended**) | B: give tasks frame caps + SELF map (today's behaviour) | C: no hatch; amortise mm RPCs |
|---|---|---|---|
| Object | `region {space, lo, hi, backing frame, perms ceiling}`, created by mm (CONTROL on the task, CONTROL on the frame) | none | none |
| Task may | map, unmap and protect pages **of the backing frame** inside `[lo,hi)`; frame offset chosen freely | anything with frames it holds | — |
| mm keeps | the frame cap; it knows every physical page in the range | nothing: the frames are gone, with no revoke | everything |
| Revoke | `region_kill`: dead, O(1); mm then `unmap_range`s in batches (or the kernel sweeps the range on the work list) | impossible (no revoke) | — |
| Fork of the range | mm reads it back with `map_query` (bounded batches) and COW-clones it like the rest | unknown pages | normal |
| Cost per malloc op | one kernel call | one kernel call | one IPC round trip, about 1.6 k cycles in two spaces on the P53, plus mm work |

Also: restrict `JOIST_SLOT_SELF` so that ordinary mm clients can't use it.
The simplest rule is that SELF needs a capability to one's own task with
CONTROL, which mm clients don't get. **Measure C first anyway**: glibc's
malloc already takes memory in large chunks. 0008 asks for the hatch to be
designed in Phase 4 and measured before Phase 7, so build A after the COW
exit, not before.

## 6. Shared memory grants vs 0006's typed bulk transfer

0006 picks three paths in Phase 5: inline copy, copy into a
receiver-provided buffer, and **move-grant** for the 54 `dealloc`
parameters (`io_read`). COW is explicitly **not** reproduced. Phase 4's
"shared memory grants" item is therefore separate, and should stay simple:
**mm maps the same pages into two tasks on request** (an `mm_share` RPC).
That works today, since a frame may be mapped many times, and needs no
kernel object.

**Prior art (from memory).**
- L4 *grant* = map + unmap from the sender, atomically. Receivers name a receive window (a flexpage).
- Zircon passes VMO handles (shared) or `zx_vmo_create_child` copies.
- seL4 shares by copying frame caps.
- Mach OOL = `vm_map_copy` COW.

**What Phase 4 must provide so Phase 5 need not undo it.**

| Phase 5 needs | Phase 4 provides |
|---|---|
| move the pages of `[va, n)` from server to client | page identity in each PTE (the Q1-B shadow), so a kernel can move a PTE run between spaces (`map_move`) without mm translating it |
| a place in the receiver to put them | the region capability (Q5) shaped so it can also act as a **receive window** |
| not break COW | COW sharers are known **only to mm**. A kernel move of a page that mm has COW-shared would hand the client a page the server's fork child still sees. Phase 4 must state which pages may be moved by the kernel. Proposal: only pages in region ranges, which mm doesn't COW-share (it copies them eagerly at fork, or blocks moves until they are broken). Otherwise the move goes through mm |
| a large in-buffer copy (`io_write`) | the kernel copies thread to thread (`hk_copy_msg` does at most one page today). Larger copies need a bounded, preemptible cross-space copy; not Phase 4's, but keep `map_query`/translate reusable for it |
| batched shootdown | one per call (Q2), already in place |

**Don't** add a kernel "shared grant" object in Phase 4. It would be a
fourth mechanism that 0006 doesn't have.

## 7. The fault path for COW

**Today** (`fault.c`, `abi.md`):
- A fault is a `Call` to the thread's handler (or the task's) with `r[0]` kind, `r[1]` address, `r[2]` error code, plus the faulting thread's control cap as capability 0. The handler endpoint's badge is delivered, so mm can mint a badge per task.
- Reply `r[0] = 0` resumes; any other value destroys the thread.
- `hk_off_cpu` (`run.c`): a fault interrupted by evict/restart "happens again when the thread next runs", so **duplicate faults are normal**.

**Prior art (from memory).**
- seL4 VM fault message: IP, address, prefetch flag, FSR. The reply resumes; mapping is separate.
- Fiasco page-fault IPC: pfa, pc. The reply carries the map item (flexpage) **in the reply itself**.
- Zircon: in the kernel (`VmAspace::PageFault`), with fault-around **(unsure)**.
- Minix: the kernel sends `VM_PAGEFAULT`; VM edits PTEs and resumes.
- Linux: `fault_around_bytes` (64 KiB) for read faults on file mappings; anonymous COW is per page; `do_wp_page` *reuses* the page if its map count is 1.

**Per write fault, Phase 4 as it stands:**
1. Exception entry.
2. Fault IPC. A two-space round trip is about 1 624 cycles fast path on the P53.
3. mm looks up its region and the page's reference count.
4. With refcount > 1: allocate, `memcpy` 4 KiB (direct map), then `map` REPLACE + shootdown if the space runs on other CPUs. With refcount 1: `protect` RW (no shootdown: an x86 #PF invalidates the faulting address's entries on that CPU; other CPUs may refault spuriously, and mm finds it already resolved).
5. **`cap_delete` of the thread cap** the fault delivered into mm's receive slot (or the next fault finds the slot occupied).
6. Reply.

So that is 3 kernel calls plus the IPC. My guess, not measured: about 4-6 k
cycles under KVM, about 1 µs or more per fault. Linux is roughly 1-2 µs per
COW fault **(from memory, unsure)**.

**Recommendations.**

| Item | Proposal |
|---|---|
| Fused reply | the fault reply may carry one **map action** (frame cap or region, offset, n, perms, REPLACE/expect): 0007's "reply carrying a frame capability". This saves a kernel entry per fault. Build it after measuring the separate version, as 0007 asks |
| Thread cap per fault | let the handler register "no thread cap for page faults" (the badge identifies the task, and a thread id word identifies the thread). mm needs the thread cap only to kill or inspect a thread. This saves a `cap_delete` per fault |
| Message fields | add an **arch-neutral access type** (read/write/exec, present/not-present) beside the raw error code, for aarch64 later; add the **faulting pc** (otherwise a `thread_read_regs` call is needed for diagnostics); keep the address and kind |
| Batching | read faults and demand-zero: **fault-around**, one `map` of up to 16 pages. Write faults: per page in the first version. Pre-breaking COW (stack top, the page next to a written one) can come later, from measurement |
| Ordering at fork | **downgrade the parent (protect RO + shootdown) before mapping the child.** Otherwise a parent thread with a stale RW TLB entry writes the page the child already sees |
| Idempotence | mm must treat "already resolved" (PTE present with the right perms and frame) as success: duplicates come from evict/restart, from two threads faulting the same page, and from spurious stale-RO refaults. The conditional `map` (expect) gives mm an atomic check |
| Dead pager | already specified: a dead handler means the thread is left suspended (or `SIGBUS` per 0007 once glibc exists) |

## 8. Testing COW under stress

**Prior art (from memory).**
- **seL4:** `sel4test` (`apps/sel4test-tests/src/tests/`: `frame*.c`, `vspace.c`, `faults.c` **(unsure on names)**), plus the functional-correctness proof. There is no kernel COW to test.
- **Zircon:** `zircon/system/utest/core/vmo/` (clone, snapshot and COW tests, `vmo-clone*.cc` **(unsure)**), kernel unit tests `kernel/vm/vm_unittest.cc` (`k ut vm`), and `kstress`'s VM stressor (`vmstress.cc` **(unsure)**).
- **Linux:**
  - `tools/testing/selftests/mm/cow.c` (D. Hildenbrand, 2022): COW across fork with GUP pins, THP, swap and KSM;
  - LTP fork, mmap and `madvise` tests;
  - stress-ng `--fork`, `--vm`, `--mmapfork`;
  - syzkaller;
  - the Dirty COW reproducer (CVE-2016-5195): a race between a write through `/proc/self/mem` and `madvise(DONTNEED)`, and the 2020 COW/GUP issue (CVE-2020-29374).
- **Minix:** `test/test*.c` fork and memory tests.

**What Joist should check.**

| Layer | Check |
|---|---|
| Host model (core) | I12′: shadow agrees with PTEs; Σ map counts = present PTEs; W^X; frames disjoint; a dying frame is swept exactly once; page-table pages conserved (Q4). Property tests compare whole states after every step, as today, including "a failed call changes nothing" for REPLACE/expect and for partial ranges. **CBMC** on `map`, `unmap_range` and `protect` with small sizes |
| Host model + an **mm reference model** | **Refinement**: an abstract machine where each task owns an array page→value and `fork` deep-copies it. The concrete one (kernel model + mm's COW logic + fault calls) runs random sequences of fork, read, write, unmap, protect, exit and kill-a-task. **Every read equals the abstract value**, and mm's view ⊇ the kernel's mappings (0008). Seeded and replayable, with mutation |
| TLA+ | per-CPU TLBs as stale caches; two threads of one task on two CPUs; mm replacing a frame. Properties: no thread reads a value written by another task after fork; the shootdown before reply is necessary (removing it must violate); parent-downgrade-before-child-map is necessary; duplicate faults are harmless |
| Kernel `cowstress` (user/) | fork trees to depth 4-6. Every page is tagged `(task, generation, va, checksum)`, written randomly, and verified after each step and at exit by parent and child. Multithreaded writers in parent and child on 2-4 CPUs (KVM). Kill children mid-fault and mid-fork (the `faultinj` pattern). growroot's `debug_counters` level after every round (frames, page-table pages, shadow pages, map counts). Run on the checking kernel |
| Mutations (kernel and model) | drop the shootdown on REPLACE; map the child before the parent's downgrade; reference count off by one (free a still-shared page); upgrade to W with refcount > 1; forget the map-count decrement; sweep skipping a page table; `cap_copy_batch` growing rights; REPLACE ignoring `expect` |
| Dirty-COW analogue | concurrent `unmap_range` (discard) and a write fault on the same page; mm must re-check with `expect`, and the test must catch a version that doesn't |

**Exit criterion wording suggestion:** "N rounds of `cowstress` on 1, 2 and
4 CPUs and on the checking kernel, every read verified, counters level,
every listed mutation caught; the refinement property test passes with M
seeds."
