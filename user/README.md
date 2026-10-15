# user/: user programs

Small freestanding programs that run in ring 3, linked with `user.ld`.
Phase 2's run alone as the kernel's root task (`../kernel`,
`ROOT_TASK=<name>`) and use the two debug system calls, `debug_write` and
`exit`, through `sys.h` (`docs/phase2-plan.md`, steps 6 and 7). Phase 3's
use the real system calls through `lib/joist.h` (`docs/abi.md`,
*Encoding*; `docs/phase3-plan.md`, step 4).

| Program | Purpose |
|---|---|
| `hello` | The root task: the first half of Phase 2's exit criterion |
| `sysfuzz` | The system-call fuzzer: the second half. Seed and call count via `UEXTRA="-DSYSFUZZ_SEED=n -DSYSFUZZ_CALLS=n"` |
| `badptr` | Self-test: `debug_write` with a kernel-half pointer must be refused |
| `ud` | Self-test: an invalid instruction is the task's fault, not the kernel's |
| `rowrite` | Self-test: a write to read-only data is a user page fault |
| `spin` | Self-test: the timer interrupts ring 3 many times and returns cleanly |
| `sysrethole` | Self-test: `syscall` in the last bytes of the user half (the `sysret` hazard) |
| `root` | Phase 3's root task (0013): loads each boot module as a task of its own (the kernel's ELF validator, built for user mode; memory a page at a time from its pool), hands out endpoints, starts the threads. `ROOT_TASK=root MODULES="pong ping"` |
| `pong`, `ping` | Phase 3 step 4: a server answering every message plus one, and a client making `PING_ROUNDS` calls (default 1000) and reporting to the root |
| `schedroot`, `worker` | Phase 3 step 5: a root task testing the scheduler from user mode (priorities and preemption, a budget per period in user mode and in system calls, equals taking turns, donation to a passive server), and the one program it loads as every worker. `ROOT_TASK=schedroot MODULES=worker` |
| `faultroot`, `faulter` | Phase 3 step 6: a root task that is the fault handler of the tasks it starts (resume past a bad instruction, a page fault resolved by mapping, terminate, no handler, an unanswered fault, thread control), and the one program it loads, which faults as asked. `ROOT_TASK=faultroot MODULES=faulter` |
| `cascaderoot` | Phase 3 step 7: destroys a chain of tasks with full capability tables while a higher-priority ticker (`worker`) measures how long it is kept waiting; and checks that everything comes back. For a kernel with larger tables (`../kernel/test/selftest.sh`). `ROOT_TASK=cascaderoot MODULES=worker` |
| `benchroot`, `ipcbench` | Phase 3 step 8: the IPC benchmark. `ipcbench` is both ends (a server loop; a client that times each `Call` with the time stamp counter, in CPU cycles against a ruler of its own, and prints least, median, 99th percentile); `benchroot` runs it in two tasks, in one, and with a server that has its own scheduling context. `ROOT_TASK=benchroot MODULES=ipcbench`, `UEXTRA=-DBENCH_CALLS=100000u` for the recorded runs |
| `smproot` | Phase 3 step 9: the multiprocessor test (CPUs run threads at once; priorities across CPUs; IPC between CPUs; a thread on another CPU stopped, read, resumed, killed; tasks destroyed while their threads run elsewhere). With `worker`. `ROOT_TASK=smproot MODULES=worker`, and more than one CPU |
| `growroot`, `fuzzchild` | Phase 3's exit: the memory-growth test. `growroot` makes and destroys tasks, threads, objects, capabilities and mappings round after round (cycles of tasks reaped, every fixed table filled to `E_NOMEM`), and checks the kernel's counters (`j_debug_counters`) don't move; `fuzzchild`, in each round, makes system calls of every kind at random. `ROOT_TASK=growroot MODULES=fuzzchild` |
| `faultinj`, `fiworker` | Phase 3's exit: the fault-injection test. Clients and servers killed at points chosen from a seed (in a Call, holding a reply capability or a saved reply, with a capability on its way, while signalling or waiting), in each of the ABI's ways; the survivors must get exactly the `E_PEER`, `E_GONE`, no-senders and dead-name notifications `docs/abi.md` says, and nothing may hang. `fiworker` is every peer: its threads carry out the root's commands one at a time (`lib/fi.h`). `ROOT_TASK=faultinj MODULES=fiworker`, seed and rounds via `UEXTRA="-DFAULTINJ_SEED=n -DFAULTINJ_ROUNDS=n"` (a failure prints how to replay its round) |
| `lib/fi.h` | The page `faultinj` shares with each peer: a record per thread, with its command, its arguments and what the call returned |
| `lib/testroot.h` | What the test root tasks share: workers from module 0 with a shared page each, sleeping, checks |
| `lib/loader.h` | For root tasks: a page allocator over the memory pool, and the loader that turns a boot module into a task |
| `lib/work.h` | The page `schedroot` shares with each worker: its role, and a count that moves only while it runs |
| `lib/joist.h` | The system calls from user mode: one raw call with every ABI register, and wrappers. Its numbers are checked against the kernel's at compile time (`../kernel/test/test_abi_user.c`) |

The self-tests are run by `make selftest` in `../kernel`.

```sh
make        # every program, into build/*.elf (clang, ld.lld)
make clean
```

These are test programs. Real user space (servers, the Hurd, glibc) starts
in later phases (§11).
