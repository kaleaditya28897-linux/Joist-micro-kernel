# The seed: a guided reading (decision 0026, layer 2)

Decision 0026 says someone reads the seed before its first use. This is a
guide for that reading. It is not the reading itself. The owner's reading
is **pending** (docs/toolchain/record.md), and until it is done the
bootstrapped GCC is not trusted.

Everything here is at live-bootstrap b1ceced7 with its submodules at the
commits in the record. Line numbers are those files' lines.

## 1. What the seed is, in QEMU mode

`tools/ddc/bootstrap.sh` runs `rootfs.py --qemu` with no `--kernel`, which
live-bootstrap calls the *kernel bootstrap*. There is no Linux and no
prebuilt program at the start. The guest's first disk is made on the host
by `lib/generator.py` (`create_builder_hex0_disk_image`):

1. **Sector 0, the boot sector:** the bytes of
   `builder-hex0/builder-hex0-x86-stage1.hex0`. For each line the host drops
   everything after `#` or `;`, strips it, and turns the hex left into
   bytes. That gives 512 bytes, sha256
   `87c3d44555bd294dbc7be12dd06697235a354247c155eb0eb5a8834a3aad1b59`.
   **This is the only binary in the chain.**
2. **From sector 1:** the *text* of `builder-hex0-x86-stage2.hex0`,
   unconverted, padded with zeros to a sector.
3. **Then a "src" file system:** a list of `src N PATH` commands, each
   followed by N bytes of a file. It holds stage0-posix's sources, the
   `steps` directory and the early source tarballs. At its end come
   commands to build the first two programs from source (below).

The committed binaries `seed/stage0-posix/bootstrap-seeds/POSIX/x86/hex0-seed`
(181 bytes) and `kaem-optional-seed` (632 bytes) are **not** copied in this
mode (`stage0_posix` skips `bootstrap-seeds`). Builder-hex0 compiles them
from `x86/hex0_x86.hex0` and `x86/kaem-minimal.hex0` instead. The bytes
those sources give are the same as the committed binaries (checked
2026-10-29: sha256 `f74dcf9c…` and `545f8953…`).

So the reading is:
- **the boot sector** (stage 1, 244 lines, about 190 bytes of code), in
  full;
- **builder-hex0 stage 2** (2682 lines), a small kernel in hex0;
- **`hex0_x86.hex0`** (155 lines) and **`kaem-minimal.hex0`** (422 lines),
  the first two programs.

All four are hex with a comment on every instruction. The work is to check
that each instruction's bytes are what its comment says, and that the
comments add up to the behaviour described.

Also trusted, and not part of the reading: the host's Python that
converts stage 1 and writes the disk (about 40 lines of `generator.py`),
QEMU, its SeaBIOS, KVM and the VM's kernel. QEMU could change anything in
the guest. That is the "machine the builds run on" of 0026's "does not
cover".

## 2. Stage 1: the boot sector

The BIOS loads sector 0 at 0x7C00 and jumps to it in 16-bit real mode,
with the boot drive in `dl`. The memory map is at lines 19-24.

| Lines | What it does |
|---|---|
| 32-43 | **Entry.** Zeroes `ax`, `ds`, `es`, `ss`; stack at 0x7700; `cld`; a far jump to 0x7C11 to set `cs` to 0. |
| 47-57 | **compile:** `bx` is the "have one digit" state; `di` = 0x7E00, where stage 2's bytes go. |
| 59-100 | **read_loop:** gets a byte from `read`. A NUL ends the input (`jz finish`). `#` and `;` start a comment. `0-9`, `a-f` and `A-F` become 0-15; any other byte is skipped. |
| 102-106 | **skip_comment:** reads up to `\n`. |
| 108-127 | **maybe_store:** the first digit is shifted into `bh`; the second is or-ed in and the byte stored with `stosb` at `es:di`. |
| 129-130 | **finish:** `jmp` to 0x7E00, stage 2's first byte. |
| 134-159 | **read_sector:** BIOS `int 0x13`, `ah=0x42` (extended read, LBA), one sector, using the disk address packet at 0x7CB0. On error (carry set, or 0 sectors read) it resets the disk (`ah=0`) and tries again, forever. |
| 163-193 | **read:** returns the next byte of the input. Keeps the offset in the sector at 0x7CB2 and the LBA at 0x7CB8. At offset 0x1FF it reads the next sector into the buffer at 0x7800 first. |
| 197-209 | **Data:** the disk address packet (size 0x10; count, also the offset; buffer 0000:7800; LBA starting at 1). |
| 211-243 | **Padding**, a disk ID, a partition table entry of type B5 (some BIOSes want one) and the `55 AA` signature at 510. |

Things to check here:
- **The relative jumps.** For example, `74 4B` at line 62 is at 0x7C1B, so
  `jz` goes to 0x7C1D + 0x4B = 0x7C68, `finish`. And `E9 95 01` at line 130
  is at 0x7C68, so it goes to 0x7C6B + 0x195 = 0x7E00, stage 2. The
  disassembly in section 5 shows every target; the `[7C11]`, `[7C6B]` and
  `[7C8C]` comments are the addresses to hold them against.
- **A stale comment:** the memory map (line 22) says the sector buffer is at
  0x7A00, but the code uses 0x7800 (`dest_offset`, lines 189 and 205). It
  is harmless: 0x7800-0x79FF is clear of the stack (below 0x7700) and of
  the code (0x7C00).
- **The input has no length.** The first NUL ends it. The zero padding
  after stage 2's text supplies that NUL.
- **Nothing is verified.** The boot sector checks nothing about stage 2. Its
  whole job is to turn hex text into bytes and jump to them, so what
  stage 2 does is what its source says, which is why stage 2 is read too.

## 3. Stage 2: builder-hex0, a kernel in hex0

Stage 2 is a minimal 32-bit kernel. It runs programs as i386 Linux ELF
binaries with a handful of system calls, all held in memory, and reads its
input from the "src" file system on the disk. Its header (lines 11-58)
lists every function, and its memory map is at lines 61-110.

| Lines | Part |
|---|---|
| 111-230 | **16-bit start:** `kernel_entry`, console output through the BIOS (`int 0x10`), and the disk address packet. |
| 233-371 | **16-bit disk I/O:** `read_sectors_16` and `write_sectors_16` (BIOS `int 0x13` 0x42 and 0x43, with retry). |
| 456-596 | **kernel_main:** enables the A20 line, takes the memory map from BIOS `e820` (kept as the file `/e820`), and sets up a GDT. |
| 597-679 | **Into 32-bit protected mode:** the interrupt table, a stub handler for everything, and `int 0x80` for system calls. |
| 680-797 | **Back to real mode and out again** (`enter_16bit_real`, `resume_32bit_mode`), so the 32-bit kernel can use the BIOS for disk and console. |
| 798-873 | **32-bit console and disk** wrappers, and `reboot`. |
| 874-973 | **The system call dispatcher.** fork, read, write, open, close, waitpid, execve, chdir, lseek, brk, access, mkdir, wait4 and getcwd; others fail. |
| 974-2037 | **The system calls.** Files live in memory at 0x54000000 and up, each found by its absolute path (`absolute_path`, `find_file`). `fork` saves the parent's image (at 0x30000000 and up) and runs the child to completion first. `execve` loads an ELF's program headers at its addresses and builds argv and envp. |
| 2038-2123 | **strcmp, and read and write** of the input stream. |
| 2124-2230 | **The `src` command:** `src N PATH` makes a file of the next N bytes. |
| 2231-2397 | **The `hex0` command:** `hex0 IN OUT` compiles a hex0 file into a file. The same parser as stage 1. |
| 2398-2477 | **Any other line** runs a program: its path, then arguments. |
| 2478-2573 | **flush_disk:** writes the in-memory files back to the disk. live-bootstrap uses this to hand its files to the next kernel (Fiwix). |
| 2574-2670 | **internalshell:** reads commands from the disk and dispatches them (`src`, `hex0`, `flush`, else run). |

Things to check here:
- **execve trusts the ELF** it loads, which is fine: everything it runs was
  compiled in the guest from source.
- **No more binaries arrive this way.** Everything that reaches a program
  comes from the `src` file system, whose content is source
  (`append_srcfs`).

## 4. hex0_x86.hex0 and kaem-minimal.hex0

**`hex0_x86.hex0`** (the first program, `hex0`, 181 bytes):

| Lines | Part |
|---|---|
| 7-56 | **The ELF and program headers.** One PT_LOAD at 0x08048000, 0xB5 bytes, executable. The program header overlaps the ELF header's last 8 bytes (line 34). |
| 63-81 | **_start:** `open(argv[1], O_RDONLY)`, `open(argv[2], O_WRONLY\|O_CREAT\|O_TRUNC, 0700)`. Leaves `edx` = 1 and the output fd in `edi`. |
| 83-110 | **The loop:** `read` one byte onto the stack; at end of file `exit(0)`. |
| 112-138 | **The parser:** a newline ends a comment, `#` or `;` starts one, and hex digits become 0-15; anything else is skipped. |
| 140-154 | **write:** two digits make a byte. `edi` changes sign on each digit, so it tracks first versus second. Then `write(out, &byte, 1)`. |

**`kaem-minimal.hex0`** (632 bytes): a minimal script runner. It reads a file
one line at a time, splits the line into arguments at spaces, and forks and
execs each line, stopping on the first failure. Read it the same way: the
ELF header first, then `_start`, the line reader, the tokenizer and
fork/execve/waitpid.

## 5. Checking each against its source, by hand

On any Linux with Python 3 and binutils (none of it heavy):

```sh
cd live-bootstrap
# The boot sector, exactly as generator.py makes it:
python3 -I -c '
import sys
out = open(sys.argv[2], "wb")
for line in open(sys.argv[1]):
    out.write(bytes.fromhex(line.split("#")[0].split(";")[0].strip()))
' builder-hex0/builder-hex0-x86-stage1.hex0 /tmp/stage1.bin
sha256sum /tmp/stage1.bin       # 87c3d445... (record.md)
# Disassemble it as 16-bit code loaded at 0x7C00 and compare each line
# with the .hex0 comments:
objdump -D -b binary -m i8086 --adjust-vma=0x7c00 /tmp/stage1.bin | less

# hex0 and kaem-minimal: convert the same way (their hex is spread over
# lines with ';' comments), then disassemble as i386 at 0x08048000:
python3 -I -c '
import re, sys
s = "".join(l.split("#")[0].split(";")[0] for l in open(sys.argv[1]))
open(sys.argv[2], "wb").write(bytes.fromhex(re.sub(r"\s", "", s)))
' seed/stage0-posix/x86/hex0_x86.hex0 /tmp/hex0
cmp /tmp/hex0 seed/stage0-posix/bootstrap-seeds/POSIX/x86/hex0-seed
objdump -D -b binary -m i386 --adjust-vma=0x08048000 /tmp/hex0 | less
```

Stage 2 is too long to disassemble at once. Read it a function at a time:
convert it the same way (the second form) and disassemble with
`--start-address` at each function's address. Its comments give the
addresses (`[7C6B]` style), and 16-bit code (lines 111-455 and 680-797)
needs `-m i8086`.

`ndisasm -b 16` / `-b 32` (from `nasm`) gives the same views.

## 6. What is built from it

In order (`seed/stage0-posix/kaem.x86`, `x86/mescc-tools-seed-kaem.kaem`,
`x86/mescc-tools-mini-kaem.kaem`, then live-bootstrap's `steps/manifest`):

1. **builder-hex0 stage 2** compiles `hex0` and `kaem-optional-seed`, then
   runs `kaem-optional-seed kaem.x86`.
2. **hex0** compiles `hex0` again and `kaem-0`. Then **hex1** (labels, one
   byte offsets), **hex2** (labels of every size), **catm** (cat) and
   **M0** (a macro assembler).
3. **cc_x86**, a C compiler written in assembly. It compiles **M2-Planet**
   (a subset-of-C compiler), which compiles **blood-elf**, **M1**, **hex2**,
   **kaem** and the rest of mescc-tools and mescc-tools-extra.
4. live-bootstrap's steps: **GNU Mes** and **MesCC** (a Scheme interpreter
   and the C compiler in it), then **TinyCC** (rebuilt several times),
   **Fiwix** (a small kernel that replaces builder-hex0), musl, make,
   patch, bash and coreutils.
5. **GCC 4.0.4**, then **Linux 4.14** from source (kexec'd into), then
   binutils, **GCC 4.7.4**, **GCC 10.5.0** and **GCC 15.2.0**, with Perl
   and Python on the way. `steps/manifest` lists every step, and
   `parts.rst` describes each one.

None of these steps adds a binary: each one is built from source by the
step before it. `steps/*/sources` lists each tarball's sha256, and those
are checked on the host and again in the guest.
