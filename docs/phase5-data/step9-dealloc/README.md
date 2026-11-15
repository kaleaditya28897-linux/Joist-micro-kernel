# Phase 5 step 9: the review of the `dealloc` out-of-line parameters

Date: 2026-11-14. For each out-of-line (OOL) parameter of the Hurd's
interfaces that carries a `dealloc` flag: which way it goes, how the
receiver can size its window before the call (decision 0032's write
window), whether its sender uses the memory after sending, and who serves
and calls it. 0006 left this review to Phase 5 ("the 54 `dealloc` sites need
individual review"); 0032 (accepted 2026-11-14) asks it for each
parameter's bound.

## Method

All by reading source; nothing downloaded was run.

- **The Hurd:** Debian's `hurd_0.9.git20261028.orig.tar.bz2` (SHA-256
  `2a17feadadf1d1cd632c446bac259acedd1c1e1bcea786c0d2118bf9d008bd27`, as
  `tools/phase5/hurd-libs.sh` checks it). The `.defs` in its `hurd/` are
  the same as Debian's installed ones.
- **glibc 2.43,** the source the cross-build uses.
- **The list:** `docs/phase0-data/ool-params.csv`'s rows with a `dealloc`
  flag, checked again on this Hurd with `tools/phase0/ool.py` corrected
  (below). Each routine's declaration and comment in its `.defs`.
- **Servers and callers:** for each routine, every definition of
  `*S_<routine>` in the Hurd and glibc, and every call of
  `<routine>`/`__<routine>` in both; the servers read through to where the
  returned memory comes from (the stub's buffer, a fresh `mmap`, or memory
  the server already held) and whether anything touches it after the
  routine returns. Every file and function read is named in the table's
  `senders_read`.
- **Reach:** the step 5 inventory (`docs/phase5-data/step5-glibc-inventory/`:
  which members `hello`, the test client and the raw libports server link),
  Joist's `sysdeps/joist`, and the main plan's Phases 6-8.
- **joistmig:** built from a copy of `third_party/mig` (as
  `tools/mig-test/run.sh` builds it) to generate `fs` and `pfinet`'s
  server stubs and see what they pass for a `dealloc[]` argument.

## Correction to the list: 56 parameters, not 54

`ool.py` takes a parameter's type as OOL only if its name is an unbounded
array directly or through a `type` alias. A type defined in the
parameter itself (`x: name_t = array[] of y`) whose name has no `type`
alias is missed. `default_pager_storage_info`'s `out size` and `out free`
(each `vm_size_array_t = array[] of vm_size_t, dealloc`) are missed this
way, and so are two `in runs` without `dealloc`. With the fix (if the type
has an `=`, judge its right-hand side), this Hurd has **144** OOL
parameters, **56** with a dealloc flag (51 `dealloc`, 3 `CountInOut
Dealloc`, 2 `dealloc[]`). Apart from those four, the CSV matches this
Hurd. The table covers all 56 and marks the two new ones.

## Files

- `ool-params-hurd-20261028.csv`: every OOL parameter of this Hurd, as
  `tools/phase0/ool.py` (corrected) lists them, in `docs/phase0-data/ool-params.csv`'s
  format: 144 rows, the Phase 0 list's 140 and the four the old script
  missed.
- `dealloc-review.tsv`: one line a parameter, tab-separated. Columns:
  `defs`, `routine`, `parameter`, `direction` (in: client to server; out:
  server to client), `mig_type`, `flag`, `dealloc_choice` (fixed, or run
  time for `dealloc[]`), `bound_class` and `bound` (the receiver's bound
  before the call), `bound_evidence`, `sender_use_after_send`,
  `senders_read`, `servers`, `callers`, `reach_phase5_7`, `unusual`,
  `notes`.

`bound_class`:

| Class | Meaning |
|---|---|
| `argument` | the caller passes the size (`amount`, `bufsiz`), and the servers keep to it |
| `fixed` | a fixed size, from the routine's meaning or its servers |
| `stub buffer` | every server writes into the stub's own buffer and never allocates: the data always fits the message |
| `small` | small by the routine's meaning (a few ints, one sockaddr, a translator's argz), but the protocol sets no limit: a stub default covers it |
| `none` | no bound known before the call: the size depends on the server's state |
| `n/a (in)` | `in`: the server reads the caller's read window |

## Results

**Direction:** 48 `out`, 8 `in`. **Flags:** 51 `dealloc`, 3 `CountInOut
Dealloc` (`default_pager_objects` ×2, `default_pager_object_pages`), 2
`dealloc[]` with a run-time choice (`dir_readdir`, `pfinet_getroutes`).

**The receiver's bound, for the 48 `out`:**

| Class | Count | Parameters |
|---|---:|---|
| `argument` | 6 | `io_read` data, `socket_recv` data (`amount`); `dir_readdir` data (`bufsiz`); `pci_conf_read` data; `pfinet_siocgifconf` buf (`amount`, but -1 means all); `pfinet_getroutes` routes (at most 255) |
| `fixed` | 3 | `file_getfh`, `pci_get_dev_regions`, `pci_get_dev_rom` |
| `stub buffer` | 1 | `socket_getopt` |
| `small` | 14 | `exec_startup_get_info` portarray, intarray; `file_get_translator`; `file_get_fs_options`; `fsys_get_options`; `login_get_location`, `login_get_input_devices`; `msg_get_init_ports`, `msg_get_init_ints`; `proc_getexecdata` ports, ints; `socket_whatis_address`; `default_pager_storage_info` size, free |
| `none` | 24 | see below |

The **24 with no bound known**, which 0032 left open:

- **5 port arrays** (rights, not bytes; joistmig refuses arrays of rights
  today): `default_pager_objects` ports, `exec_startup_get_info` dtable,
  `file_get_storage_info` ports, `msg_get_dtable`, `socket_recv` ports.
- **19 data arrays:** `default_pager_objects` objects,
  `default_pager_object_pages`; `exec_startup_get_info` argv, envp;
  `file_get_storage_info` ints, offsets, data; `msg_get_environment`,
  `msg_get_env_variable`; `proc_getallpids`, `proc_getprocinfo` procinfo
  and threadwaits, `proc_getprocargs`, `proc_getprocenv`,
  `proc_getloginpids`, `proc_getsessionpgids`, `proc_getsessionpids`,
  `proc_getpgrppids`; `socket_recv` control (pflocal).

None of the 24 is reached in Phase 5. `exec_startup_get_info` is replaced
by Joist's start-up. The proc routines come with proc in Phase 8, and the
default pager's with Phase 6. Their sizes are small in ordinary use, but some
can be large: argv and environments, a fragmented file's runs, a big
object's pages. **Two `argument` routines are unbounded as glibc calls
them:** `dir_readdir`, which readdir and getcwd call with `bufsiz` 0 (the
whole directory), and `pfinet_siocgifconf`, which `if_nameindex` and
`__ifreq` call with -1 (all interfaces). Their bound is in the protocol, but
glibc's calls would have to pass the window's size.

**The sender's use after sending:**

- **The 8 `in`** (all in `process_reply.defs`) have no sender. Nothing in
  the Hurd or glibc sends them. They are simpleroutines (a deferred reply
  sent as a message), so 0032's windows couldn't serve them anyway: no
  caller waits. Joist's equivalent is a saved reply.
- **The 48 `out`:** no server read touches the memory after its routine
  returns. Each returns either the stub's own buffer or memory it allocated
  for the reply or already held, and none keeps a pointer to it. **0032's
  copy during the call keeps every one right.** Three kinds lean on
  `dealloc` more than usual:
  - **libpipe returns its own pages** (`pq.c` `packet_fetch`, for `io_read`
    in pflocal, `fifo` and `new-fifo`, and for `socket_recv`'s data and
    control in pflocal). For a read of a page or more it returns a pointer
    into the packet's own buffer, ends the read on a page boundary, and
    moves past those pages, so the send's `dealloc` is what unmaps them.
    The stub copies and then frees. glibc's `__joist_mig_deallocate` rounds
    to pages as Mach does, so the same pages go. The free must happen even
    if the reply's copy fails, because the pipe has already dropped the data.
  - **exec gives away its startup arrays** (`exec_startup_get_info`'s
    five). It returns memory it kept from `exec_exec` and zeroes the lengths,
    so that its cleanup (`deadboot`) frees nothing. The same holds with
    copy-then-free.
  - **`trans/magic` keeps its memory** (`dir_readdir` with a `dealloc[]`
    false). It returns its own directory buffer, which is built once and
    never changed. Under Mach the client got a COW copy. With 0032 it gets a
    copy made during the call, which is right as long as the stub doesn't
    free the buffer.

**Found in joistmig (not yet fixed):** for a `dealloc[]` `out` argument,
a server routine gets a `boolean_t *` to say whether its memory is to be
freed. joistmig passes `(void *)0` there and frees any pointer that isn't
its own buffer, whatever the routine says. libdiskfs's and libnetfs's
`dir_readdir` write `*data_dealloc = 1` unguarded, a write through NULL, and
magic's write (`= 0`) does the same; magic would also have its buffer freed.
pfinet's and lwip's `getroutes` test the pointer first, and their choice
agrees with joistmig's, so they are unaffected. libtrivfs's `dir_readdir`
(`hello`) never touches the flag. `tools/mig-test` doesn't catch it because
its test servers never write the flag.

**Unusual:** 8 port arrays (the 5 above, `exec_startup_get_info`
portarray, `msg_get_init_ports`, `proc_getexecdata` ports). Their rights
move, and this is not a bulk-data question. **Obsolete:** `login_get_*`
(its server, `login/utmp.c`, is not built) and the 8 `process_reply` (not
used). The **very large** ones: `io_read` and `socket_recv` data (the
reader's `amount`), `dir_readdir` with `bufsiz` 0, `file_get_storage_info`
offsets, `proc_getprocargs`/`getprocenv`, `msg_get_environment`,
`exec_startup_get_info` argv and envp, `default_pager_object_pages`.

**Reached in Phase 5:** `io_read` (hello's server; glibc's client). The
client stubs of `dir_readdir`, `file_get_translator`, `pfinet_siocgifconf`
and `proc_getpgrppids` are linked into every inventory program. libtrivfs's
servers of `dir_readdir`, `file_get_storage_info`, `file_getfh`,
`file_get_translator` (all refusals), `file_get_fs_options` and
`fsys_get_options` are linked into `hello`. glibc's `msg_get_*` servers are
in every program. By the plan's Phase 7 exit (files, against a stub
filesystem), glibc's `io_read`, `dir_readdir` and `file_get_translator`
are called in Phase 7.

## Not settled from the source

- `pci_*`'s client is libpciaccess's Hurd backend, which was not read; no
  caller is in the Hurd or glibc.
- Whether every netfs translator's `netfs_get_dirents` keeps to `bufsiz`
  was not read; ext2fs's `diskfs_get_directs` does.
- GNU Mach's own `.defs` (`device_read`, `memory_object_data_supply`,
  mach_debug's arrays) carry `dealloc` too. They are outside the Hurd's
  interfaces and Joist doesn't serve them, so they were not reviewed.
