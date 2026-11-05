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

- `sysdeps/joist/joist-gsync.{c,h}`: the futex queues (0014 B): a
  notification per thread and a fixed table of FIFO waiter queues, under
  Mach's `gsync_wait`, `gsync_wake` and `gsync_requeue`.
- `test/`: host tests (`make check`: real threads on an exact emulation of
  Joist's notifications, under ASan, UBSan, MSan and TSan), mutations
  (`make mutate`) and CBMC proofs (`make cbmc`).
