# Phase 5 step 11: a MIG round trip against the raw IPC one

The plan's last measurement (*Measured, recorded*), on the P53's own CPU,
2026-11-18.

## What is timed

`user/glibcroot` built with `GT_HELLO` (the `hello` selftests' image): after
its checks, the root's client thread calls `io_seek(file, 0, SEEK_CUR)` on
`hello`'s root node 2 200 times, the last 2 000 each timed with the TSC
(`lfence; rdtsc`) around the call; the median, least and greatest are
printed ("hello: a MIG round trip (io_seek)"). `io_seek` with `SEEK_CUR` and
0 changes nothing, so this is the round trip itself:
- the client side: joistmig's user stub and `joist_mig_call` (the root's
  raw runtime);
- the Call;
- the server side, `hello`, unchanged: glibc's server side (`server_ipc`,
  the names table), libports' one-thread loop (the port by its badge,
  `ports_begin_rpc` and `ports_end_rpc`), libtrivfs's demuxer and its
  protid lookup, and `hello`'s `trivfs_S_io_seek`;
- and back, by `ReplyRecv`.

The raw one is step 9's `ipcbench`, "two address spaces, passive server",
on the same machine (`docs/phase5-data/step9-copy/`): 1 912 cycles by its
ruler. Its ruler there took 6 772 to 7 106 TSC counts for 12 000 cycles,
about 0.58 counts a cycle. The P53's TSC runs slower than its cores at
turbo.

How: the image built on the laptop (`make BUILD=build/hello ROOT_TASK=glibcroot
MODULES="mm glibctest" UEXTRA=-DGT_HELLO GLIBCTEST=hello.elf GLIBC_CONSOLE=...
GLIBC_HELLO=hello.elf.helloroot.o`, from `tools/phase5/hello.sh`), booted on
the P53 by `tools/p53/boot.sh` (KVM, the virtual CPUs pinned, 256 MB), 3
boots each on 1 and 2 CPUs. Logs: `p53-kvm-smp*-*.log`.

## Results (TSC counts; the median of 3 boots' medians)

| | 1 CPU | 2 CPUs |
|---|---:|---:|
| MIG round trip (`io_seek` to `hello`) | 3 198 | 3 526 |
| raw round trip (`ipcbench`, 1 912 cycles) | about 1 110 | |

The boots' medians: 3 198, 3 128, 3 198 (1 CPU); 3 536, 3 446, 3 526 (2 CPUs).

## What it says

- **A MIG round trip through a real Hurd server costs about 2.9 times the
  raw one:** about 2 100 TSC counts, or 3 600 cycles, on top of the IPC.
  That is the stubs' marshalling, glibc's names table on the server side,
  libports' badge lookup and RPC accounting, and libtrivfs's demuxing to
  `hello`'s routine. None of it is the kernel's.
- **On 2 CPUs it costs about 10 % more** (the client and server threads on
  different CPUs).
- Where the time goes inside those 3 600 cycles isn't measured here. A
  profile belongs to Phase 11's server hardening, as the plan has
  performance last.
