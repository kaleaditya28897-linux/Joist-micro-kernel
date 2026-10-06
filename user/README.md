# user/: Phase 2's user programs

Small freestanding programs that run in ring 3 as the kernel's root task
(`../kernel`, `ROOT_TASK=<name>`). They use the two Phase 2 debug system
calls, `debug_write` and `exit`, through `sys.h`, and are linked with
`user.ld`. Context: `docs/phase2-plan.md`, steps 6 and 7.

| Program | Purpose |
|---|---|
| `hello` | The root task: the first half of Phase 2's exit criterion |
| `sysfuzz` | The system-call fuzzer: the second half. Seed and call count via `UEXTRA="-DSYSFUZZ_SEED=n -DSYSFUZZ_CALLS=n"` |
| `badptr` | Self-test: `debug_write` with a kernel-half pointer must be refused |
| `ud` | Self-test: an invalid instruction is the task's fault, not the kernel's |
| `rowrite` | Self-test: a write to read-only data is a user page fault |
| `spin` | Self-test: the timer interrupts ring 3 many times and returns cleanly |
| `sysrethole` | Self-test: `syscall` in the last bytes of the user half (the `sysret` hazard) |

The self-tests are run by `make selftest` in `../kernel`.

```sh
make        # every program, into build/*.elf (clang, ld.lld)
make clean
```

These are throwaway test programs. Real user space (servers, the Hurd,
glibc) starts in later phases (§11).
