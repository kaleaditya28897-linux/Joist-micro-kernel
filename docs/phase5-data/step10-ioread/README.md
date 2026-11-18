# Phase 5 step 10: io_read through decision 0032's windows, end to end

The plan's measurement (*Measured, recorded*: `io_read`, 4 KiB to 1 MiB),
on the P53's own CPU, 2026-11-18. Move-grant was replaced by a copy
(decision 0032), so this measures the copy path as glibc uses it.

## What is timed

`user/glibctest`'s `iobench` mode: `pread(0, buf, n, 0)` of its stdin, 200
times for each size after 20 to warm, each timed with the TSC around the
call (`lfence; rdtsc`); the median, least and greatest are printed. The
server is glibcroot's console (`user/glibcroot`), which serves stdin from
module 3 (1 MiB here). With an explicit offset, its `io_read` reads there
and leaves the file pointer alone, as the Hurd's does. So each call is
glibc's whole path:
- glibc's `pread` and `__io_read`, joistmig's user stub;
- for 16 KiB and more, a slice of glibc's window region, with the write
  window set before the call and cleared after;
- the Call;
- the root's server stub, with `reply_write` copies of 64 KiB or less
  through the reply, then the answer;
- for 4 KiB and less, the data comes back in line, through the IPC buffer.

How: `bench.sh` here (`build` on the laptop, with glibctest from
`tools/phase5/glibc-test.sh`; `boot` on the P53 by `tools/p53/boot.sh`, KVM,
the virtual CPUs pinned, 256 MB), at the commit that adds this directory.
The pool is as large as the glibc test runs' (a 1 MiB buffer and its window
region need it). Logs: `p53-kvm-smp*-*.log`.

## Results (median of 3 boots' medians, TSC counts)

| bytes | 1 CPU | 2 CPUs |
|---:|---:|---:|
| 2 048 (in line) | 22 014 | 36 586 |
| 4 096 (in line) | 23 234 | 37 760 |
| 16 384 (window) | 24 832 | 43 824 |
| 65 536 | 32 842 | 54 820 |
| 262 144 | 64 898 | 98 604 |
| 1 048 576 | 188 506 | 263 682 |

A line's three boots were within 4 % of each other (2 CPUs, 64 KiB).

## What it says

- **Each read costs a constant plus about 0.16 counts a byte (1 CPU).** The
  constant is about 22 000 counts. The per-byte part, from 256 KiB to 1 MiB,
  is about 0.157 counts a byte on 1 CPU and 0.21 on 2. That matches step
  9's kernel copy (about 0.1 cycle a byte, from cache) plus glibc's copy
  out of the window into the caller's buffer, which MIG's ownership rule
  asks for (step 9's design, item 4).
- **The constant is the test's server and glibc's path, not the copy.**
  glibcroot's console polls: each message also costs it a clock read and a
  counters read, and wakes it from a timed receive. glibc adds its
  descriptor lookup, the window's two system calls and the slice's lock.
  Step 9's `ipcbench` puts a plain round trip at about 1 900 cycles and a
  64 KiB `reply_write` at about 8 600. A real filesystem's loop would come
  closer to those.
- **The window costs little at the switch:** 16 KiB through the window
  (24 832) is about 1 600 counts above 4 KiB in line (23 234).
- **2 CPUs cost more:** client and server sit on different CPUs, so the
  copies cross caches and every call is an interrupt to the other CPU.
- **Warm, not cold:** the same bytes are read each time, so the copy is
  from cache. 0032 asks for cold copies too. A cold measurement needs a
  source larger than the cache to cycle through, and is left to the first
  real file server (Phase 6's pager), with `JOIST_COPY_MAX` re-checked then.
