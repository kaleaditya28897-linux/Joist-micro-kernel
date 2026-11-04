# GNU MIG in Joist

GNU MIG, the Mach Interface Generator, kept so that the Hurd's `.defs` are
read exactly as the Hurd reads them (Phase 5 plan, choice 3; step 6).

## Where it comes from

Debian's `mig_1.8+git20231217.orig.tar.xz`, SHA-256
`684c897b62fdeaa84608738f76facb11d41335312c3488d27c339a51393376e9` (as in
Debian's `mig_1.8+git20231217-11.dsc`), unpacked here unchanged. It ships
its generated parser (`parser.c`, `lexxer.c`), so it builds with clang
alone: no bison or flex.

Licences: MIG's own sources are Carnegie Mellon's (the Mach licence, a
permissive one); the generated parser is the GNU GPL with Bison's
exception; `COPYING` is the GPL, version 2. All of it may be combined with
Joist's GPL, version 3.

## What Joist added

Two files; every other file is as Debian ships it.

- `cpu.h`: MIG's build makes it from `cpu.sym` and Mach's headers (its
  `Makefile.am`). Generated once, for x86-64, from Debian's hurd-amd64
  `gnumach-dev` 1.8+git20261028-1 headers:

  ```
  awk -f gensym.awk cpu.sym > cpu.symc
  clang --target=x86_64-unknown-linux-gnu -fno-integrated-as -nostdinc \
    -isystem $(clang -print-resource-dir)/include \
    -isystem SYSROOT/usr/include/x86_64-gnu -isystem SYSROOT/usr/include \
    -D__x86_64__ -S -ffreestanding -x c -o cpu.symo cpu.symc
  sed -e '/^[^*].*$/d' -e 's/^[*]/#define/' -e 's/mAgIc[^-0-9]*//' cpu.symo > cpu.h
  ```

- `joistmig.c`: MIG's Joist mode, a program of its own (`joistmig`) built
  from MIG's other files and this one in place of `migcom.c`. The same
  parser and types, the same user and server headers; new user stubs and
  server demuxers that speak Joist's IPC through `user/lib/joist_mig.h`.
  The design is in `docs/phase5-plan.md`, "Step 6: the design".

Build: `clang -O2 -I. -o joistmig joistmig.c error.c global.c header.c
lexxer.c parser.c routine.c server.c statement.c string.c type.c user.c
utils.c`. Tests: `tools/mig-test/run.sh`.
