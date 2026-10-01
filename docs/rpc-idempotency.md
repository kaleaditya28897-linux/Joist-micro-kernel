# RPC idempotency classification

Phase 0 deliverable required by `docs/client-recovery.md` §5. Every Hurd RPC
that a client may replay after reconnecting to a restarted server gets a class.
**Unclassified is a build error** once the MIG backend exists (Phase 5); until
then this file is the table.

**Status:** seeded 2026-09-23 from the rpctrace data behind
`docs/mach-inventory.md` (the 60 Hurd RPCs that fired in the three workloads, plus 2 seen in an earlier discarded trace). The full list
comes from the static `.defs` pass. Rows here are ordered by observed frequency
in the package build, per client-recovery §8 Q3.

## Classes

- **idempotent**: replaying it after reconnect is safe.
- **conditional**: idempotent for some argument values only. The condition is
  stated, and the client must check it before replaying.
- **non-idempotent**: replay risks double execution. A lost reply surfaces as
  `EIO` (client-recovery §5).
- **n/a (not restartable)**: the server is not in a restartable class (proc,
  auth, exec) or the call is part of a handshake that must be re-run whole.
  These still need a class for the build rule; the class only records that
  replay is not the recovery mechanism.

## The finding that changes §4

`io_read` and `io_write` carry an offset argument, and **offset `-1` means "use
and advance the server's file pointer"**. In the build trace:

| Call | offset `-1` | explicit offset |
|---|---:|---:|
| `io_read` | 65 694 (99.2%) | 508 |
| `io_write` | 27 187 (99.9%) | 24 |

`io_seek` whence: `SEEK_SET` 50 331, `SEEK_CUR` 6 886, `SEEK_END` 4. All counts are build-workload values from `phase0-data/io-offsets.csv`, which also has the boot and shell columns.

So glibc's `read`/`write` do **not** track the offset client-side. It lives in
the server's per-open state (libdiskfs `struct peropen`). client-recovery §4
assumes the recovery record can restore "the offset" after reconnect. With
today's protocol, the client does not know it.

**Decided in `client-recovery.md` §4.1 (2026-10-01, proposed):** the server
stays the authority. `io_read`/`io_write`/`io_seek` replies report the
resulting offset, the client caches it in the per-description recovery
record, and the reconnect restores it with one `io_seek (SEEK_SET, …)` before
replaying anything. Client-side offsets were rejected: they would move
offset atomicity into memory shared between processes.

**What that does to replay of offset `-1` calls:**

- `io_read`, offset `-1`: **idempotent after restore.** If the old server
  performed the read but died before replying, the offset advance died with
  it. The restored offset is the one before the read, and replaying reads the
  same bytes.
- `io_write`, offset `-1`: **idempotent after restore, except with
  `O_APPEND`.** If the write reached the disk, replaying writes the same bytes
  at the same restored position, which has the same result. With `O_APPEND`
  the position is end-of-file, which the lost write may have moved, so replay
  could append twice. That stays **non-idempotent**: fail with `EIO`
  (client-recovery §5).
- For descriptions shared by concurrent processes, the restored offset is
  best-effort (§4.1 *Known weak spot*), and so is this replay.

## Table

Counts are boot / shell / build from the rpctrace workloads, generated from
`phase0-data/hurd-rpc-counts.csv`. `*_request` names in the raw trace are the same
RPC (same msgid) and are merged there.

| RPC | Server | Count | Class | Condition / note |
|---|---|---|---|---|
| `dir_lookup` | fs | 41 / 883 / 292 908 | conditional | idempotent unless flags include `O_CREAT` (with `O_EXCL`, a replay returns `EEXIST`) or `O_TRUNC` |
| `io_stat` | io | 36 / 853 / 210 966 | idempotent | |
| `io_read` | io | 30 / 155 / 66 202 | conditional | explicit offset: idempotent. Offset `-1`: idempotent once the offset is restored (client-recovery §4.1); see above |
| `io_seek` | io | 0 / 35 / 57 221 | conditional | `SEEK_SET` idempotent; `SEEK_CUR`/`SEEK_END` not |
| `io_write` | io | 4 / 32 / 27 211 | conditional | idempotent with an explicit offset, or with offset `-1` once the offset is restored (§4.1); **never with `O_APPEND`** |
| `io_map` | io | 62 / 169 / 24 214 | idempotent | returns memory objects; mmap recovery itself is deferred to Phase 4 (client-recovery §3) |
| `io_identity` | io | 0 / 0 / 9 690 | idempotent | also the natural carrier for identity validation (client-recovery §3.1) |
| `file_check_access` | fs | 0 / 2 / 6 448 | idempotent | |
| `auth_user_authenticate` | auth | 0 / 0 / 5 789 | n/a (not restartable) | handshake half; see `io_reauthenticate` |
| `io_reauthenticate` | io | 0 / 0 / 5 789 | non-idempotent | consumes a rendezvous port; on reconnect, re-run the whole reauthentication rather than replaying |
| `proc_getpids` | proc | 4 / 38 / 4 783 | idempotent | |
| `proc_set_arg_locations` | proc | 4 / 38 / 4 783 | idempotent | sets absolute values |
| `proc_setmsgport` | proc | 4 / 38 / 4 783 | non-idempotent | returns the *old* port; a replay returns the new one |
| `proc_handle_exceptions` | proc | 4 / 38 / 4 783 | n/a (not restartable) | |
| `proc_wait` | proc | 1 / 24 / 4 722 | non-idempotent | reaps a child |
| `dir_readdir` | fs | 0 / 8 / 4 218 | conditional | takes an explicit entry index, so replay is safe *if* the directory's index order survived the restart; otherwise best-effort, as client-recovery §3 says |
| `proc_task2proc` | proc | 2 / 15 / 3 064 | idempotent | |
| `proc_child` | proc | 2 / 15 / 3 064 | non-idempotent | |
| `proc_task2pid` | proc | 1 / 15 / 3 064 | idempotent | |
| `proc_mark_exit` | proc | 0 / 17 / 3 056 | non-idempotent | |
| `term_getctty` | term | 0 / 8 / 2 769 | idempotent | |
| `proc_set_code`, `proc_get_code`, `proc_get_arg_locations`, `proc_set_entry`, `proc_get_entry` | proc | 1/15/2481; 1/15/2481; 1/15/2481; 1/15/2481; 1/15/2481 (in the order listed) | idempotent | getters, or setters of absolute values |
| `proc_dostop` | proc | 1 / 15 / 2 464 | n/a (not restartable) | |
| `dir_unlink` | fs | 0 / 0 / 2 417 | non-idempotent | a replay returns `ENOENT` for a succeeded unlink |
| `exec_startup_get_info` | exec | 3 / 23 / 2 302 | n/a (not restartable) | |
| `proc_getpgrp` | proc | 3 / 23 / 2 302 | idempotent | |
| `file_exec_paths` | fs | 2 / 14 / 2 301 | non-idempotent | replaces the calling task's image |
| `msg_sig_post` | msg (per-process) | 0 / 14 / 2 268 | non-idempotent | delivers a signal |
| `socket_create` | pflocal/pfinet | 0 / 10 / 1 605 | non-idempotent | allocates |
| `tioctl_tiocgwinsz` | term | 0 / 0 / 1 386 | idempotent | |
| `file_utimens` | fs | 0 / 3 / 1 157 | idempotent | absolute times; `UTIME_NOW` replays with a later time, which is acceptable |
| `file_set_size` | fs | 0 / 3 / 1 151 | idempotent | absolute size |
| `socket_connect2` | pflocal | 0 / 5 / 827 | non-idempotent | socket recovery is best-effort anyway (client-recovery §3) |
| `auth_getids` | auth | 2 / 13 / 648 | idempotent | |
| `auth_makeauth` | auth | 0 / 0 / 555 | non-idempotent | allocates a handle |
| `tioctl_tiocgpgrp` | term | 0 / 2 / 551 | idempotent | |
| `file_chmod` | fs | 0 / 0 / 96 | idempotent | absolute mode |
| `dir_rename` | fs | 0 / 0 / 48 | non-idempotent | |
| `io_readable` | io | 0 / 1 / 21 | idempotent | |
| `dir_mkdir` | fs | 0 / 0 / 18 | non-idempotent | |
| `io_get_openmodes` | io | 0 / 3 / 18 | idempotent | |
| `dir_mkfile` | fs | 0 / 0 / 11 | non-idempotent | creates an **unlinked** file; see client-recovery §8 Q2 |
| `dir_link` | fs | 0 / 0 / 11 | non-idempotent | |
| `file_set_translator` | fs | 0 / 0 / 11 | non-idempotent | |
| `io_set_all_openmodes` | io | 0 / 0 / 10 | idempotent | absolute modes; note it is description state held by the server, like the offset |
| `dir_rmdir` | fs | 0 / 0 / 10 | non-idempotent | |
| `term_get_nodename` | term | 0 / 1 / 10 | idempotent | |
| `proc_uname` | proc | 0 / 0 / 8 | idempotent | |
| `io_select_timeout` | io | 0 / 0 / 7 | idempotent | |
| `file_chauthor` | fs | 0 / 1 / 6 | idempotent | |
| `interrupt_operation` | any | 0 / 4 / 5 | idempotent | cancel; a replay is harmless |
| `msg_get_init_port` | msg | 0 / 1 / 5 | idempotent | |
| `proc_getprivports` | proc | 3 / 0 / 0 | idempotent | |
| `io_pathconf` | io | 0 / 0 / 2 | idempotent | |
| `file_get_storage_info` | fs | 1 / 0 / 0 | idempotent | |
| `msg_proc_newids` | msg | 0 / 0 / 0 | idempotent | notification of absolute values; seen only in the discarded hung `bash -i` trace, kept because the RPC is real |
| `proc_setpgrp` | proc | 0 / 0 / 0 | idempotent | seen only in the discarded hung `bash -i` trace, kept because the RPC is real |
| `proc_setowner` | proc | 1 / 0 / 0 | idempotent | |

The traffic is dominated by `dir_lookup`, `io_stat`, `io_read` and `io_seek`.
By call volume, then, the recovery path depends most on (a) the `O_CREAT`
condition on `dir_lookup`, and (b) the offset question above, not on the long
tail of mutations.
