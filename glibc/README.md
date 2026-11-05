# Joist's files for glibc

What Joist adds to glibc 2.43 to run the Hurd's programs on Joist
(`docs/phase5-plan.md`, *Step 7: the design of `sysdeps/joist`*, accepted
2026-11-05). glibc's own files that Joist changes are patches in
`tools/phase5/glibc-patches/`; the files here are new ones, added to
glibc's source tree as they are.

## License

**Everything under `sysdeps/` is LGPL-2.1-or-later**, as the rest of glibc
(the owner's decision, 2026-11-05): it is linked into every program, which
must stay linkable as with any glibc, and it is written to go upstream. The
tests under `test/` are not part of glibc and are Joist's own, GPL-3.0-only,
as the rest of this repository.

## What is here

`tools/phase5/glibc-cross.sh` copies `sysdeps/joist` into glibc's source
and configures it with `--enable-joist` (patch 0011), which puts
`sysdeps/joist` before the first of the Mach layer's directories (for
`x86_64-gnu`, the front of the list): a file here replaces the Mach
layer's file of the same name, among them the stubs glibc generates from
Mach's `.defs` (`mach_port_allocate.c`, `RPC_mach_port_mod_refs.c`, ...).

- `sysdeps/joist/joist-gsync.{c,h}`: the futex queues (0014 B): a
  notification per thread and a fixed table of FIFO waiter queues, under
  Mach's `gsync_wait`, `gsync_wake` and `gsync_requeue`.
- `sysdeps/joist/joist-names.{c,h}`: Mach port names on capability slots
  (the design's items 2 and 3), tested against the kernel model
  (`model/test/test_names.c`).
- `sysdeps/joist/joist-ports.{c,h}`, `joist-sys.h`: the program's one name
  table under a lock (in ld.so for a dynamic program, which libc.so and
  libmachuser use through its GLIBC_PRIVATE exports in `Versions`; in
  libc.a for a static one), its kernel calls, and the `mach_port_*` calls
  on it (`mach_port_allocate.c`, `RPC_mach_port_mod_refs.c`, ...: each
  replaces the generated stub of that name). A call naming another task
  is refused (`MACH_SEND_INVALID_DEST`).
- `sysdeps/joist/mach_reply_port.c`, `mig-reply.c`: reply names, which
  stand for a call's own reply capability; `mig_get_reply_port` gives one
  for the task, the reply sentinel. `msg.c`, `mach_msg_trap.c`:
  `mach_msg` is not provided (`MACH_SEND_INVALID_HEADER`).
- `sysdeps/joist/joist-vm.{c,h}`: the task's address reservations (the
  design's item 6): libc chooses every address `mm` maps, first fit in a
  window (4 GiB to 64 TiB) or where it is told, and answers `vm_region`;
  checked before any change, so a refusal changes nothing. Host-tested
  against a page-level model (`test/test_vm.c`). The `vm_*` calls on it
  are not here yet.
- `test/`: host tests (`make check`: the futex queues with real threads on
  an exact emulation of Joist's notifications, and the reservations
  against their model, under ASan, UBSan, MSan and TSan), mutations
  (`make mutate`) and CBMC proofs (`make cbmc`).
