# Phase 5 step 9: copies through the caller's windows against the IPC buffer (P53, 2026-11-15)

Decision 0032 moves bulk data by a kernel copy into the caller's write
window (`reply_write`) or out of its read window (`reply_read`), at most
`JOIST_COPY_MAX` bytes a call (64 KiB, "set by measurement: the copy runs
under the kernel's lock"). This is that measurement: what a `reply_write`
of 4, 16 and 64 KiB adds to a call, against the plain call and the IPC
buffer's copy, and so what the copy costs a byte and how long a copy of
`JOIST_COPY_MAX` holds the kernel's lock.

- **The plain call:** two address spaces, a passive server (ipcbench's
  measurement 1), nothing in the IPC buffer.
- **The IPC buffer:** the same, with 4 KiB of IPC buffer each way
  (measurement 4, Phase 3's exit): the kernel copies 4 KiB to the server
  and 4 KiB back.
- **reply_write of n:** the same call; before each answer the server
  copies n bytes (4, 16 or 64 KiB) from a buffer of its own into the
  client's write window by one `reply_write` through its own reply, then
  answers with `ReplyRecv` (measurements 5-7). The window is a region
  benchroot gives the client (32 pages, mapped writable); the client sets
  it once with `thread_window` before timing. Nothing goes back in the
  IPC buffer.

The source and the window are the same 64 KiB every time: both stay in
the caches (the i7-9850H's L2 is 256 KiB a core), so these are the copy's
cost from cache, its best case (below).

20 × 5000 samples a line in a boot, the boot's median taken (ipcbench's
`p50`); 3 boots on each of 1 and 2 CPUs; the table gives the median of
the 3 boots' medians, in cycles (TSC).

## How

`user/ipcbench/ipcbench.c` (`copy_bytes`, `serve`) and
`user/benchroot/benchroot.c` (`give_window`, measurements 5-7), the
kernel's system calls 86-88 (`kernel/sys/abi.c`), and `kernel/lib/string.c`'s
`memcpy` by `rep movsb`: main 6eaecd7 with the step's uncommitted work,
the default build. Built on joist-dev2 (the P53 has no limine) and booted
on the P53's own CPU by `tools/p53/boot.sh` (KVM, the virtual CPUs pinned
to host CPUs 2-5, 256 MB, the same host pages each boot), from the P53
itself (`P53_HOST=aditya@localhost`), the build VM idle and no other QEMU
running: `bench.sh` here (`build`, then `boot`). Logs:
`p53-kvm-smp*-*.log`, and `boots.txt` (what `bench.sh boot` printed). ISO SHA-256
`f3f2f1a9623e67377a2f830c3ed675a872435957d8a58a585201d1e151dde659`.

## Results (median of 3 boots' medians, cycles)

| | 1 CPU | 2 CPUs |
|---|---:|---:|
| a system call that does nothing | 448 | 500 |
| **the plain call** (two spaces, passive server) | **1 912** | **2 012** |
| 4 KiB of IPC buffer each way | 3 264 | 3 368 |
| reply_write of 4 KiB | 2 688 | 2 840 |
| reply_write of 16 KiB | 3 768 | 3 904 |
| reply_write of 64 KiB | 8 640 | 8 832 |
| *less the plain call:* | | |
| 4 KiB of IPC buffer each way (8 KiB copied) | 1 352 | 1 356 |
| reply_write of 4 KiB | 776 | 828 |
| reply_write of 16 KiB | 1 856 | 1 892 |
| reply_write of 64 KiB | **6 728** | **6 820** |

The spread of a line's three boots (largest less smallest, over the
median) was at most 0.37 % on 1 CPU (reply_write of 64 KiB) and 0.82 % on
2 CPUs (reply_write of 16 KiB); the do-nothing call, the plain call and
the IPC buffer's line were the same to the cycle in all three boots on
1 CPU. For reference, on 2 CPUs the plain call while the other CPU makes
system calls was 3 736 (spread 1.07 %).

## What it says

- **The copy costs about 0.1 cycle a byte** from cache: 0.099 between 16
  and 64 KiB (1 CPU: 4 872 cycles for 48 KiB), 0.088 between 4 and 16 KiB;
  0.100 and 0.087 on 2 CPUs. That is `rep movsb`'s rate, about 10 bytes a
  cycle (26 GB/s at the TSC's 2.6 GHz). The laptop's figure, 0.1 under
  KVM, holds here; the byte loop it replaced was 1.2.
- **A `reply_write` costs about 230 to 420 cycles besides its copy** (the
  two straight-line fits' intercepts): a system call and its checks. At
  4 KiB, one way, it is 776 cycles against the IPC buffer's 1 352 for
  4 KiB each way (about 676 a direction): the window's extra system call
  costs about what the IPC buffer's second copy saves, so a server
  answering with up to 4 KiB loses little either way, and past the
  buffer's 4 KiB the window is the only way.
- **The kernel lock's hold at `JOIST_COPY_MAX`:** the 64 KiB line less
  the plain call is **6 728 cycles on 1 CPU, 6 820 on 2** (about 2.6 µs at
  2.6 GHz): the whole of the `reply_write` call, entry to exit, with the
  lock held for nearly all of it. For scale (decision 0017): a passive
  call's mean hold was 865 to 1 293 cycles, one timer write (a VM exit)
  6 383, and the own-context hold before the timer moved out of the lock
  (`RUN_LOCK_TIMER_OUT`) 8 891. A copy of the full 64 KiB holds the lock
  about as long as one timer write did, and about five passive calls.
- **Not measured: a cold copy.** Here both ends are in L2. A copy from or
  to memory not in the caches (a server's freshly read file data, a
  client's new buffer) runs at memory's rate, likely 3 to 5 times slower,
  so the worst hold at 64 KiB may be 20 000 to 35 000 cycles (8 to 13 µs)
  on this machine. The contended case, a copy on one CPU while another
  waits for the lock, wasn't measured either.

## Whether 64 KiB should stay the limit (the owner decides)

**Recommended: keep 64 KiB.** A smaller limit splits a large transfer into
more calls, each paying the 230 to 420 cycles: at 16 KiB, a 64 KiB transfer
takes four calls, about 700 to 1 250 cycles more (+10 to 19 %), to bound
the hold at about 1 850 cycles. At 32 KiB, one call more (+3 to 6 %) for a
hold of about 3 600. A larger limit gains little: at 128 KiB, a call fewer
in each 128 KiB (about 3 %) for a hold near 13 000 cycles warm, and perhaps
50 000 cold. At 64 KiB the per-call cost is about 5 % of the copy, and the
warm hold is within what one timer write cost under the lock before
0017's change. If the cold case, measured at step 10 with `io_read`
(0032's 4 KiB to 1 MiB), turns out much worse than estimated above, 32 KiB
is the fallback: it halves the worst hold for a few per cent.
