# The toolchain record (decision 0026, layers 2 and 3)

What was built, from what, with what, and what it hashed to. Anyone with
the same seed and sources can rerun `tools/ddc/bootstrap.sh` and the L3
steps below and compare.

**State (2026-10-30):**
- **Layer 2 done:** the bootstrap ran to GCC 15.2.0.
- **Layer 3:** 2T and 2D are identical (270 of 270 files), after one
  explained mismatch. nasm, checked through Limine, is too (2026-10-30;
  *The DDC of nasm*, below).

The seed's reading is **done**: the owner read docs/toolchain/seed-review.md
and accepted the seed (2026-10-30). The bootstrapped GCC is **trusted**, as
0026 says, and so is what is verified against it.

## Layer 2: the trusted compiler

### live-bootstrap
- **Repository:** https://github.com/fosslinux/live-bootstrap
- **Pinned:** commit `b1ceced7ea8a819a26f23796f6dd7ae8496a33a4` (master,
  2026-09-17, "Merge pull request #587"). It has **no tagged release**
  (no tags, no GitHub releases, 2026-10-29). Pinning by commit is
  accepted by the owner (2026-10-29).
- **Submodules:**

| Path | Commit | Tag |
|---|---|---|
| builder-hex0 | `a2781242d19e6be891b453d8fa827137ab5db31a` | |
| seed/stage0-posix | `45d90f5955b6907dc6cdea9ebafce558359edcd3` | Release_1.9.1 |
| seed/stage0-posix/x86 | `3b9c2bb6d4155e4f2e5f642b5e0f59255dfc5934` | |
| seed/stage0-posix/M2libc | `68a23cfd05d5a355ba7a30c770d684cbe86fcc4e` | |
| seed/stage0-posix/M2-Planet | `bd2fe4b0659fd0ad3f476a5ad0ef801bd134665d` | Release_1.13.1 |
| seed/stage0-posix/M2-Mesoplanet | `4b011a85da73a7c97212468d41f17e806ba99547` | Release_1.13.0 |
| seed/stage0-posix/mescc-tools | `5adfbf3364261a77109878a56b100aeeb6ef9ac4` | Release_1.7.0 |
| seed/stage0-posix/mescc-tools-extra | `a151c245e512076971a3c85bb1502cf92cfa83b6` | Release_1.4.0 |
| seed/stage0-posix/bootstrap-seeds | `cedec6b8066d1db229b6c77d42d120a23c6980ed` | (not used in QEMU mode) |

(The other architectures' submodules, AArch64, AMD64, armv7l, riscv32 and
riscv64, are fetched but not used.)

### The seed
In QEMU mode the only binary is builder-hex0's boot sector, which the host
makes from its hex0 source (seed-review.md, section 1).

| File | sha256 | Bytes |
|---|---|---|
| boot sector, from `builder-hex0/builder-hex0-x86-stage1.hex0` | `87c3d44555bd294dbc7be12dd06697235a354247c155eb0eb5a8834a3aad1b59` | 512 |
| `builder-hex0/builder-hex0-x86-stage1.hex0` (source) | `b137141c916c5eb745a67bdfd314af33a445972db9c77f2b06c8b48f39a447e8` | |
| `builder-hex0/builder-hex0-x86-stage2.hex0` (source) | `37b765a6aecd007e4afec714211aa4d8f39515ebc3e277fc1251d4b54fd31c66` | |
| `seed/stage0-posix/x86/hex0_x86.hex0` (source) | `0727c60b5573c0cbdbcf908cf54ebeb861b64cdfc1bdc818995a53bfac02f07a` | |
| `seed/stage0-posix/x86/kaem-minimal.hex0` (source) | `a2d4036b17f42a2081f892038b3fd12f7e886732c8da757bdbdd5fb23e00c1ae` | |

The binary seeds committed in stage0-posix are not used in this mode;
builder-hex0 compiles them from the two sources above. The sources give
exactly the committed bytes (checked 2026-10-29):

| Binary in `bootstrap-seeds/POSIX/x86/` | sha256 | Bytes |
|---|---|---|
| `hex0-seed` | `f74dcf9cef2aee9eb7723eca755849d474fccd89662947021d1aa08429d23c13` | 181 |
| `kaem-optional-seed` | `545f89537f070d7e62b05243be34cbcfdb1e92dbf9d82e2cd15792d880436ac2` | 632 |

**The owner's reading of the seed: done.** The owner read seed-review.md
and accepted the seed (2026-10-30).

### The sources
- **Fetched** with live-bootstrap's own `mirror.sh` into `~/ddc/distfiles`
  on the build VM, 2026-10-29. It downloads each file, or makes it with
  `git archive` where live-bootstrap gives only a git commit, and checks
  each against the sha256 in live-bootstrap's `steps/*/sources`. The guest
  checks each again before using it.
- **314 files, 1.2 GB.** Their sha256 are in
  `docs/toolchain/live-bootstrap-distfiles.SHA256SUMS`, whose own sha256 is
  `674b39a20b2c0d558e8d9c5a7794a04ee985518b13fe6edaa583763e2073542e`.
- `download-distfiles.sh` was tried first. It cannot fetch the sources
  that exist only as git commits (URL `_`), so `bootstrap.sh` uses
  `mirror.sh`.

### The run
- **Command,** on the build VM: `tools/ddc/bootstrap.sh ~/ddc`, which runs

  ```
  python3 rootfs.py --qemu --external-sources --cores 6 --qemu-ram 6144 \
      --mirrors file:///home/user/ddc/distfiles --target /home/user/ddc/target
  ```

  `rootfs.py` then runs:

  ```
  qemu-system-x86_64 -enable-kvm -m 6144M -smp 6 \
      -drive file=/home/user/ddc/target/init.img,format=raw \
      -drive file=/home/user/ddc/target/external.img,format=raw \
      -machine kernel-irqchip=split -nic user,ipv6=off,model=e1000 \
      -no-reboot -nographic
  ```

  (The QEMU network is there because `rootfs.py` always adds it. With
  `--external-sources` every source is on the second disk, and the guest
  fetches nothing.)
- **When:** 2026-10-29, 13:11:38 to 20:31:57 UTC: **7 h 20 min**
  (26 423 s). Most steps use one CPU. The guest's Linux (built in the chain
  by GCC 4.0.4) was up for 17 975 s of it; the time before that is
  builder-hex0 and Fiwix.
- **Log:** `~/vlog-ddc/l2-boot.log` on the VM (443 000 lines, ending in
  `reboot: Power down`, `exit 0`).
- **The disks it left:** `target/init.img` (builder-hex0's disk, sha256
  `b8b9a6e84caf46befc251c44e8835ddb6b98230a8a55970466a5916496dcdac5`), and
  the final system, `target/external.img`. That one is kept read-only as
  `~/ddc/lb-final.img`, sha256
  `3bfdca288648879d4c539ab1f4ec0b949c72bca1a92b0141b41542f0dfe1994b`. Layer
  3 runs in it. It is an ext3 file system 1 MiB into a 16 GiB disk,
  3.7 GiB used.
- **Is it reproducible?** Not checked: one run so far. A second run, to
  compare the GCC's hashes above, is 7 hours of VM time.

### The GCC
- **Version:** `gcc (GCC) 15.2.0`, `i686-unknown-linux-musl`, C and C++,
  no multilib. It defaults to `-std=gnu++17` (`__cplusplus` 201703L). A
  C++17 program with threads builds statically and runs (the probe job,
  2026-10-29). LLVM 21 asks for GCC 7.4 or newer and C++17, so this GCC is
  enough.
- **The system around it:** Linux 4.14.341-openela (i686, PAE), musl,
  binutils 2.41, GNU Make 4.2.1, Python 3.11.1, Perl, tar 1.34, xz 5.6.4.
  No cmake or ninja (L3 builds them).
- **sha256,** from `tools/ddc/gcc-hashes.sh ~/ddc/lb-final.img` (read
  with `debugfs`, without booting it):

| File | sha256 |
|---|---|
| `/usr/bin/gcc` | `923f5e46a599751e8b1152591539f2102ff86c7ee9ea69d199e00296d5bec473` |
| `/usr/bin/g++` | `9342a03cd54e24d23164bef4196b49552660ea8b117bab0c259e82f7acaa23d8` |
| `/usr/bin/cpp` | `6540de9a085392b73fbb9d46e9c65379d58e2d0d19b82e3fa4a5f794fc410872` |
| `.../libexec/gcc/i686-unknown-linux-musl/15.2.0/cc1` | `6b1e9e51fb29e96f6f76d6a2602af4333e5822d4a902c76c91506966431c5119` |
| `.../15.2.0/cc1plus` | `08ee803366234590fbb10db72e8c92eeb61215ad757e1384aef52eef1da66137` |
| `.../15.2.0/collect2` | `2a3e743a98d9c8c7a1fd93da20547a5cc530a561e3f0dca2b6938bf9632a2d18` |
| `.../15.2.0/lto-wrapper` | `0c4a720043c91c212a747d19fe8a66b6e4cef1dd3f7bf2b50599993242815be2` |
| `/usr/bin/as` (binutils 2.41) | `c10604b6c880ab0fce67634f48ccbd67f7d83d840f52f45a25cf9ecc92ed0c50` |
| `/usr/bin/ld` and `ld.bfd` | `47dbaff4df771bd02d8bf3da6ff219c53a3978b22a3aa4ca417e3201cea63466` |
| `/usr/bin/ar` | `9d7ce00fc66cdfef20b8b581467172df8f29cf49b6d800b77847d1a3fdfa8142` |
| `/usr/bin/ranlib` | `7c52bc69fb740e9e254986630ca812ff741e9db0196ae97e77abfaa885db36bd` |
| `/usr/lib/i686-unknown-linux-musl/libstdc++.a` | `79188d44a28321783f7bdd2cdca755f915b8f7fef13ce0d06fe92b41e6812db0` |
| `.../libstdc++.so.6.0.34` | `9aeeb171ceeee4b00a076a6b5a91ada92a0e3c8381ef026f3be08e917b1b7b17` |
| `.../libgcc_s.so.1` | `bf5786f09044633a9c42ca7beb03e73af6215605e98f06007d047ae740307236` |
| `.../gcc/i686-unknown-linux-musl/15.2.0/libgcc.a` | `d3d2fcbae5c1e088cefdf30658709e343b8a976daad14f6c6818b5f6dbd2a10e` |
| `.../15.2.0/libgcc_eh.a` | `f981cb8cfb6a2f5073a5709d0680ce6659d6cb02aadd4eb8e3fcdc381c331421` |
| `.../15.2.0/crtbegin.o` | `44937d7a81723eb73ffd3f31ef9d5744bbd7668855133442af8aca3357c51392` |
| `.../15.2.0/crtend.o` | `936915df06a446788baee9dbb92b5291e5c3e0fcd5059688b51099e15a22cfe2` |
| `/usr/lib/i686-unknown-linux-musl/libc.a` (musl) | `68c1a705a4fc42e29b5e47ddec0a6800677d9cbc44c41f9e2cb07789677deded` |
| `.../libc.so` (musl, also its loader) | `d899a7e49a066dbcc7eb3d81cdd2f8f592525493f931871ed49bb45d78a39703` |
| `.../crt1.o` | `45fc5ebefb55d74ad053f4e94fe8fc4ef7dee91108d18c0e2e9623b369562497` |
| `.../crti.o` | `8459e59936ab4e327632147a97bd906fcf6794682de262c58c727d2a696a04cc` |
| `.../crtn.o` | `6f16041ae59c0ac6fe0d60580e3e22cf1d8d21be4dc88609a0366eb10fae6edf` |
| `/boot/vmlinuz` (the guest's Linux, used for layer 3) | `0716c71eb0641ca04f986327e4dc05b54a0f51d21c3bf3eb57ac2f2063cd61c1` |

**Trusted: yes,** since the owner read the seed and accepted it
(2026-10-30). Everything verified against this GCC is trusted with it.

## Layer 3: DDC of Clang and lld

### Source A
- `llvm-project-21.1.8.src.tar.xz`, from
  https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8
- **sha256:** `4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142`.
  The same as GitHub's own record of the asset (its API's `digest`).
- **Signature:** `llvm-project-21.1.8.src.tar.xz.sig` (sha256
  `ee44b67f2260fd87d40377560672ac0562bf9dc335b89d3c35edc41faa350424`),
  made 2025-12-16 11:09:52 UTC. `gpg --verify`: **good signature** from
  Douglas Yung <douglas.yung@sony.com>, key
  `FFB3 3689 80F3 E6BB 5737  145A 316C 56D0 64CA CBA5` (ed25519, 2025-09-15).
- **The key:** from https://releases.llvm.org/release-keys.asc (sha256
  `f38bd19b7f2d22cafa755841e6ce9375b9a2e6d41f7b974e4953d6b084839001`,
  2026-10-29). The same fingerprint is on the signer's GitHub account
  (https://github.com/dyung.gpg). It is not on keys.openpgp.org.
  The other keys in that file:
  - `D574 BD5D 1D0E 9889 5E3B  F900 44F2 485E 45D5 9042` Tobias Hieta
  - `474E 2231 6ABF 4785 A88C  6E8E A2C7 94A9 8641 9D8A` Tom Stellard
    (expired 2025-05-02)
  - `B6C8 F982 82B9 44E3 B0D5  C253 0FC3 042E 345A D05D` Hans Wennborg
  - `0FB9 6824 606D 15FD D54C  3D09 2FFC 629D 3F56 3BDC` Muhammad Omair
    Javaid
  - `7104 6D1E 9C66 56BD D611  7187 3E83 BABF 4A4F 9E85` Cullen Rhodes

### The tools the guest builds itself
- **cmake 3.31.6:** `cmake-3.31.6.tar.gz`, sha256
  `653427f0f5014750aafff22727fb2aa60c6c732ca91808cfb78ce22ddd9e55f0`. It is
  in `cmake-3.31.6-SHA-256.txt` (the same from GitHub and cmake.org), whose
  signature is good from Brad King, primary key
  `CBA2 3971 357C 2E65 90D9  EFD3 EC8F EF3A 7BFB 4EDA` (signing subkey
  `C6C2 6532 4BBE BDC3 50B5  13D0 2D2C EF10 3492 1684`). The same key is on
  keys.openpgp.org and his GitHub account.
- **ninja 1.12.1:** GitHub's archive of tag v1.12.1, sha256
  `821bdff48a3f683bc4bb3b6f0b5fe7b2d647cf65d52aeb63328c91a6c6df285a`. It
  has no signature. That hash is Debian's `ninja-build_1.12.1.orig.tar.gz`
  (its `ninja-build_1.12.1-1.dsc`).

### Limine's source (for L3b)
- **Version:** 12.9.2, the release whose binaries Joist uses now
  (docs/testbed.md; `limine --version` on the VM).
- `limine-12.9.2.tar.xz` from
  https://github.com/Limine-Bootloader/Limine/releases/tag/v12.9.2, sha256
  `9d104aa70b7cf017899b1f79426f55f78206372daccb059a93732febba59ad37`
  (the same as GitHub's digest).
- **Signature:** `limine-12.9.2.tar.xz.sig`, made 2026-10-04 15:32:43 UTC,
  good from Mintsuki <mintsuki@protonmail.com>, key
  `05D2 9860 D0A0 668A AEFB  9D69 1F3C 021B ECA2 3821`. That is the key
  testbed.md records for the binaries; it is the same on keys.openpgp.org
  and on https://github.com/mintsuki.gpg.

### Where the stages run (accepted by the owner, 2026-10-29)
- **All in live-bootstrap's final system** (0026's option 1), except path
  D's stage 1', which must be built by Debian's Clang under Debian. The
  system is `lb-final.img`, booted by `tools/ddc/guest.sh` under QEMU/KVM:
  - 6 vCPUs, 6 GiB, no network;
  - a throwaway copy-on-write layer over the image, which is never written;
  - its own kernel (`/boot/vmlinuz`, above);
  - the inputs on a fresh ext3 disk, and a work disk kept from job to job.
- **live-bootstrap is i686 only, so every stage is a static i686 program:**
  - stage 1T and both stage 2s link the guest's musl and libstdc++;
  - stage 1' links Debian's i686 glibc and libstdc++ (`-m32 -static`),
    deliberately not the guest's: those were compiled by the bootstrapped
    GCC, and path D would then not be independent of path T.
- **cmake 3.31.6 and ninja 1.12.1 in the guest** are built there from the
  sources above by the bootstrapped GCC (`tools/ddc/stage.sh tools`).
  sha256 of the results: cmake
  `842c00e6ec533c064139e0ed68f0318cc159ff19e6b491ffdde9db884984b7bc`, ninja
  `77bcc88e35bca6ee54b5541069839a7423d133d9aa718b14f300f8fa6dc8ebc8`.
- **The stage 2 builds** are identical but for the stage 1:
  - the stage 1 is copied to `/work/stage1`; the build is in `/work/build`
    and installs into `/work/stage2`;
  - each uses its own stage 1's `lld` (`-fuse-ld=lld`);
  - both get the same fixed flags that point Clang at the system's
    libraries (`stage.sh`; live-bootstrap's layout is not one Clang finds
    by itself);
  - `SOURCE_DATE_EPOCH=1765883392` (the signature's time), `LC_ALL=C`,
    `TZ=UTC`, umask 022.

### The options
`tools/ddc/llvm.cmake` (Release, no assertions, no PGO, no LTO; clang and
lld; X86 only; static; no optional libraries; `LLVM_APPEND_VC_REV=OFF`;
only the tools 0026 names, `llvm-readelf` and Clang's headers installed),
plus, from `tools/ddc/build-llvm.sh`, for every stage:
`-ffile-prefix-map=SRC=/llvm-src -ffile-prefix-map=BUILD=/llvm-build
-march=pentium4 -mfpmath=sse`.

**`-march=pentium4 -mfpmath=sse` was added after the first comparison
failed** (below). Every stage now does its floating point in SSE2.

### The first comparison: a mismatch, explained (2026-10-29/30)
The first 2T and 2D (without `-mfpmath=sse`) matched in 268 of 270 files.
**`clang-21` and `lld` differed.**
- **Each path was deterministic:** 2D built twice was identical, and so
  was 2T.
- **9 of 2529 objects differed:**
  - in LLVM: TailDuplicator, FunctionSpecialization, LoopDistribute,
    DemoteRegToStack, LoopVectorize and SLPVectorizer;
  - in clang: ASTContext, Targets/Hexagon and SemaOpenMP.
- **Where it starts:** for `DemoteRegToStack.cpp`, both stage 1s gave the
  same IR. The first difference was in the Greedy Register Allocator:
  same input, more spill slots from stage 1'.
- **The cause:** the stage 1s did floating point differently.
  - The bootstrapped GCC's i686 default is `-march=pentiumpro
    -mfpmath=387` (x87, 80-bit excess precision, `FLT_EVAL_METHOD` 2), so
    stage 1T computed its floats that way.
  - Debian's Clang (`-m32`) built stage 1' for `pentium4` with SSE2.
  - The register allocator's spill weights and the optimizers' cost
    models are floats, so the two compilers chose differently.
- **The fix:** remove the difference the same way on both paths (SSE2 for
  every stage). No byte is ignored.
- `findings.md` (the hand-off) holds the investigation.

### The stages (with the fix), 2026-10-30

| Stage | Built by | Where | Time | sha256 of `bin/clang-21` | of `bin/lld` |
|---|---|---|---|---|---|
| 1' (D) | Debian's Clang 21.1.8 (apt.llvm.org) | Debian | 37 min | `1ae1e35184c818695e5e8f33594231c24e57621b6254a5fcfa4715bf1798c48f` | `c862274fc1aa9d79b0c51eb3a9882bcc14e6ad1b91e3425c27021b74acd834f7` |
| 1T | the bootstrapped GCC 15.2.0, GNU ld | guest | 50 min | `6dc4cd132b5ccef6dae8157d8a132baa92e6507230c6226776a09d9404a04850` | `0e19337cdd717bf852a31bb196c3185eccf9b89d02e9f03c0f4de9a2dab3ce80` |
| 2T | stage 1T, its lld | guest | 44 min | `cb2d968a…` | `c10f4e44…` |
| 2D | stage 1', its lld | guest | 41 min | `cb2d968a…` | `c10f4e44…` |

The commands (on the build VM, from `~/ddc`):
- **Stage 1':** `EXTRA_FLAGS=-m32 LINKER=lld tools/ddc/build-llvm.sh
  clang-21 clang++-21 llvm-src/llvm-project-21.1.8.src d/build1 d/stage1`
- **Stage 1T:** `tools/ddc/ddc.sh stage1T`
- **Stage 2T:** `tools/ddc/ddc.sh stage2 t`
- **Stage 2D:** `tools/ddc/ddc.sh stage2 d`

**Determinism, with the fix:** 2D built a second time (`ddc.sh stage2 d`,
42 min) is identical to the first, 270 of 270 files. Without the fix,
both 2D and 2T had each been built twice, each pair identical.

### The result: 2T and 2D are identical
`tools/ddc/compare.sh` (every file and symlink, byte for byte; nothing
ignored): **270 of 270 files identical**, the same file list and the same
symlinks. That is every installed binary (6 files, 9 symlinks), and Clang's 264
headers.

**This is the verified toolchain**, kept on the build VM as
`~/ddc/toolchain/llvm-21.1.8-verified`. Every file's sha256 is in
`docs/toolchain/llvm-21.1.8-verified.SHA256SUMS` (whose own sha256 is
`6b2fa759ddcee2372c38dae2056261511389089c662ba3e945e8e31a4eed7ecf`). The
binaries:

| File | sha256 |
|---|---|
| `bin/clang-21` (and `clang`, `clang++`, `clang-cpp`, `clang-cl`) | `cb2d968a4e375611cb3e7dc83007cdcb80e54f68f4a271217eb514bfcf48a88e` |
| `bin/lld` (and `ld.lld`, `ld64.lld`, `lld-link`, `wasm-ld`) | `c10f4e444f2d217891c5ff85854e5cd21b5f2bbd430a44f288abcd06cc13099b` |
| `bin/llvm-ar` | `7dd1e183e32ff37763fdd87fbe4a815d3aa765fdbd56698929cdde88f02621e9` |
| `bin/llvm-objcopy` | `d970ff1c798abfbc349f00f588c5bceaed7c382218bff43ce03a76dd733e2fe5` |
| `bin/llvm-objdump` | `8e0e99b5d10d8a43768a9646ef2d95f75b9fc272d383454fd9c508ba92b63d06` |
| `bin/llvm-readobj` (and `llvm-readelf`) | `d896303521599e442084bcc3e56341623c3c48e0f9d4b9a6751ea55b6c0bb827` |

They are static i686 programs: `clang version 21.1.8`, default target
`i686-unknown-linux-musl`. They run on the build VM's Debian x86_64 as
they are.

**It is verified as far as the GCC is trusted,** and the GCC is trusted
since the owner accepted the seed (2026-10-30). 0026's "does not cover"
applies as written.
Also not covered: the musl, libstdc++ and libgcc code linked into the
stage 2 binaries was compiled by the bootstrapped GCC, and DDC does not
check it.

## Packages installed on the build VM
`sudo apt-get install --no-install-recommends`, 2026-10-29 (Debian 13):

| Package | Version | For |
|---|---|---|
| parted | 3.6-5 | live-bootstrap's disk images |
| cmake | 3.31.6-2 | path D's stage 1' |
| ninja-build | 1.12.1-1 | path D's stage 1' |
| diffoscope, diffoscope-minimal | 297+deb13u1 | comparisons |
| gpg | 2.4.7-21+deb13u1+b5 | signatures |
| libc6-dev-i386 | 2.41-12+deb13u4 | path D's stage 1' (static i686) |
| lib32stdc++-14-dev | 14.2.0-19 | path D's stage 1' |
| lib32gcc-14-dev | 14.2.0-19 | path D's stage 1' |

Already there and used:

| Package | Version |
|---|---|
| clang-21, lld-21, llvm-21 (apt.llvm.org) | 1:21.1.8~++20251221033036+2078da43e25a-1~exp1~20251221153213.50 |
| qemu-system-x86 | 1:10.0.13+ds-0+deb13u1 |
| python3 | 3.13.5-1 |
| python3-requests | 2.32.3+dfsg-5+deb13u1 |
| e2fsprogs | 1.47.2-3+b12 |
| git | 1:2.47.3-0+deb13u1 |
| curl | 8.14.1-2+deb13u7 |
| xorriso | 1.5.6-1.2+b1 |

## Accepted by the owner
1. **live-bootstrap pinned by commit**, since it has no tagged release
   (2026-10-29).
2. **The sources fetched on the host,** checked there and again in the
   guest (2026-10-29).
3. **All stage 2 builds in the bootstrapped system** (option 1), with
   static i686 stages (2026-10-29).
4. **No network for the L3 guests** (2026-10-29).
5. **Every stage built with `-march=pentium4 -mfpmath=sse`**, the fix
   for the first mismatch (2026-10-30).
6. **The fixed library-path flags** for every stage 2 (`-stdlib++-isystem`,
   `-B`, `-L`), the same for 2T and 2D (2026-10-30).
7. **nasm is diverse-double-compiled** through Limine (below), not only
   built with the verified toolchain (2026-10-30).
8. **mtools stays outside DDC,** like `xorriso`; the ISO's bytes are
   covered by `repro-check` (2026-10-30).

## Proposed, awaiting the owner
1. **Limine's other tools:** the host programs (nasm, the `limine` tool)
   link the build VM's glibc, and Limine's build directory is fixed.
2. **The verified toolchain as Joist's default** (`LLVM_BIN` set by
   default in both Makefiles, the toolchain kept where `check-toolchain.sh`
   passes), and Limine from source as the ISO's default. Not done: the
   owner's call.
3. **Map Clang's resource directory** in the REPRO flags
   (`-ffile-prefix-map=$(shell $(KCC) -print-resource-dir)=clang`), so
   where the toolchain is installed never reaches Joist's bytes. Not done:
   it changes every build's debug information.

## Layer 3, afterwards: Joist built with the verified toolchain (2026-10-30)

### The setting
- **`LLVM_BIN`** names the verified toolchain's `bin` directory. The
  kernel's Makefile uses it for everything that goes into the image
  (`KCC`, `LD`); the user programs' Makefile for `CC` and `LD`.
- **Unset, nothing changes:** `clang` and `ld.lld` from PATH.
- **The host builds** (the tests, the sanitizers, the fuzzers, clang-tidy,
  scan-build, the model) keep the host's Clang. They need the host's C
  library and runtimes (compiler-rt), which the verified toolchain
  doesn't have.
- **Making the verified toolchain the default** is the owner's call
  (proposed, below).
- **`tools/ddc/check-toolchain.sh DIR`** checks every file of a toolchain
  against `llvm-21.1.8-verified.SHA256SUMS`, and that it has no other
  file. It passes on `~/ddc/toolchain/llvm-21.1.8-verified`, and fails on
  a changed byte or an extra file (both tried).

### Results (the build VM, from `~/ddc/joist`, the tree at 042d1f7 with the L3b changes)
- **The kernel ELF against the Debian-Clang build: not byte-identical,
  and explained.** Every loaded section is identical: `.text`, `.rodata`,
  `.data` and `.bss` of the kernel, and of every user program. Only the
  DWARF differs, for two reasons:
  - **The producer string:** "Debian clang version 21.1.8
    (++20251221033036+2078da43e25a-1~exp1~20251221153213.50)" against
    "clang version 21.1.8".
  - **The absolute path of Clang's own headers** (stdint.h and the like):
    `/usr/lib/llvm-21/lib/clang/21/include` against
    `/home/user/ddc/toolchain/llvm-21.1.8-verified/lib/clang/21/include`.

  Seven user programs are identical outright. mm, mmclient and mmroot
  differ in their DWARF only, for the same reasons.

  | Build | sha256 of `joist.elf` |
  |---|---|
  | Debian's Clang | `90fa3cfe577c367c28a7dd45236e1ef67f456a7e93aecc7a18702fd46850b6ea` |
  | the verified toolchain | `9f4e9f04519ed5bc88a2cdb876115cda7cf33d161ad076e35d6f9a78817b8f88` |
- **The kernel selftests** (all of `kernel/test/selftest.sh`, run as
  `~/vlog-ddc/ddcsel.sh` with `LLVM_BIN`): **81 runs ok** and the bench
  gate's three medians ok, `exit 0`.
- **`tools/repro-check.sh`** with `LLVM_BIN`: **all 32 artifacts
  identical.** The same again with `LIMINE_DIR` set to the Limine below.
- **`make -C model check`** (the VM idle, 2026-10-30 05:49-06:05 UTC):
  237 tests, 0 failed, under plain, ASan, UBSan, MSan and TSan. The model
  builds for the host, so it uses the host's Clang whatever `LLVM_BIN`
  says. It checks the tree, not the toolchain.

### Limine 12.9.2 from source
`tools/ddc/build-limine.sh` (on the build VM, with the verified toolchain):
- **nasm 2.16.03** first, from source, built by the verified Clang.
  Limine's BIOS and UEFI x86-64 parts need it, and a binary nasm would be
  one more compiler nobody verified.
- **Then Limine:** `--enable-bios --enable-bios-cd --enable-bios-pxe --enable-uefi-x86-64
  --enable-uefi-cd`, with the verified Clang, lld, `llvm-objcopy`,
  `llvm-objdump` and `llvm-readelf`, and that nasm. The host programs
  (nasm and the `limine` tool) link the build VM's glibc.
- **mtools** (Debian's) packs the UEFI CD image's FAT file system.
- **Reproducible:** two builds, from different source directories, with
  different locales, time zones and umasks, are identical. It took two
  fixes. Both ensure the build directory's name never reaches the output:
  - `-ffile-prefix-map` for the C;
  - a fixed build directory for nasm, which Limine always asks for DWARF
    and which has no prefix map. That DWARF fed the BIOS stage's build ID.

| File | sha256 |
|---|---|
| `limine-bios.sys` | `a2e34b480f86a76a1c9d58a686d7b91c70582e52b33fe3d663dc6d6f8d58d916` |
| `limine-bios-cd.bin` | `e0f7a86e3519eab5df251c5dcfe056b11a5b2846cf04af8a9f854f1a956c79f6` |
| `limine-bios-pxe.bin` (built since 2026-10-30; not in the ISO) | `2d3bf404cc15163bd7f72e2e211ed88de25390b3f8ce75527c2fdd66798ef6bd` |
| `limine-uefi-cd.bin` | `f88e004258ee148f3960bbd8e7ff6ebc4f10fe88883b967b3b19119ea0b2bbc2` |
| `BOOTX64.EFI` | `0a20986be932af86c93b6b8de74921e8de8ba2fc5c9ed69e9b10564ebf1def01` |
| `limine` (the host tool) | `9d7d9752efccd1cf8058cede7e596ff976434f63692295e7c1afa2b507f0efb2` |
| `nasm` | `f91002c77956a63b6985de0cfb4b106ca70c7c3877db8eee77cbe3e401dd39c9` |

- **The ISO** uses them when `LIMINE_DIR` points at them, and the
  `limine` tool there writes the BIOS stage. Built with `LLVM_BIN` and
  that `LIMINE_DIR`, the `mmroot` ISO (sha256
  `23bdc468744632bad2042a1364191b281ce2703aeb6ae47a62a7cb50dd818898`)
  boots under BIOS and UEFI: `qemu-test`, the root task exited 0,
  `mm: all ok`.
- **Not the same bytes as the release binaries Joist uses today**
  (docs/testbed.md). Those were built by Limine's maintainer with another
  toolchain, so no match is expected.

### Packages added for this
| Package | Version | For |
|---|---|---|
| mtools | 4.0.48-1 | Limine's UEFI CD image |
| nasm | 2.16.03-1 | the DDC of nasm: the third witness (2026-10-30) |

nasm's source: `nasm-2.16.03.tar.xz` from nasm.us, sha256
`1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148`. nasm
publishes no signature; the hash is Debian's `nasm_2.16.03.orig.tar.xz`
(its `nasm_2.16.03-1.dsc`), checked through Debian's signed archive
below.

## The DDC of nasm (2026-10-30)
Accepted by the owner (2026-10-30; 0026, Amendment 1). nasm doesn't
compile itself, so it is checked through what it makes:
`tools/ddc/nasm-ddc.sh`, on the build VM.

### The source
`nasm-2.16.03.tar.xz`, the same bytes as above. **nasm signs nothing:**
nasm.us has no signature or checksum file beside its tarballs
(2026-10-30). The signed link is Debian's archive, checked on the build VM
(`apt-get source --download-only nasm`, then by hand with `gpgv`):
- trixie's `InRelease` (2026-09-12): good signatures from the Debian
  archive keys (12/bookworm) `4CB5 0190 207B 4758 A3F7  3A79 6ED0 E7B8
  2643 E131` and (13/trixie) `B8E5 F131 76D2 A7A7 5220  0280 78DB A3BC
  47EF 2265`, and the stable release key (13/trixie) `4158 7F7D B8C7 74BC
  CF13  1416 762F 67A0 B2C3 9DE4`, from `debian-archive-keyring` 2025.1;
- it lists `main/source/Sources` with sha256 `ae99131b…b65606`;
- that lists `nasm_2.16.03-1.dsc` (`8ba32e63…a1d7`) and
  `nasm_2.16.03.orig.tar.xz` with sha256
  `1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148`:
  the tarball used.

So the source is the one Debian ships, not one nasm's authors signed.

### The method
1. **nasm-V:** built by the verified Clang on the build VM, as
   `build-limine.sh` builds it: `./configure CC="clang
   --target=x86_64-linux-gnu" CFLAGS="-g -O2 -pipe
   -ffile-prefix-map=/tmp/joist-build-limine=/build"
   LDFLAGS=-fuse-ld=lld AR=llvm-ar RANLIB="llvm-ar s"`, `make nasm`. It
   links the build VM's glibc.
2. **nasm-T:** built by the bootstrapped GCC 15.2.0 **in live-bootstrap's
   final system** (`guest.sh`: 4 vCPUs, 2 GiB, no network, a new work
   disk), with that system's make, shell, binutils and musl. The same
   configure line but for the compiler and its linker: `CC=gcc`, the
   same `CFLAGS`, `LDFLAGS=-static` (the guest's musl isn't on the build
   VM), and the guest's own `ar` and `ranlib`. A static i686 program,
   which runs on the build VM as it is.
3. **Three Limines:** `build-limine.sh` three times, with `NASM` set to
   nasm-V, nasm-T and Debian's `/usr/bin/nasm`. Each copies its nasm to
   the same place in the same fixed build directory; the C is the
   verified Clang's, and everything else is the same.
4. **Compare** every installed Limine file byte for byte (`compare.sh`;
   nothing ignored): `limine-bios.sys`, `limine-bios-cd.bin`,
   `limine-bios-pxe.bin`, `limine-uefi-cd.bin`, `BOOTX64.EFI` and the
   `limine` tool.

What it covers: up to 27 of Limine's nasm sources (the BIOS boot
sectors and stage 2, the PXE and CD sectors, and the UEFI x86-64 parts).
It shows the nasms agree on what Joist uses, not on every input.

### The nasms
| nasm | Built by | sha256 |
|---|---|---|
| nasm-V | the verified Clang (x86_64, glibc) | `f91002c77956a63b6985de0cfb4b106ca70c7c3877db8eee77cbe3e401dd39c9` |
| nasm-T | the bootstrapped GCC, in the guest (static i686, musl) | `ccd8eba5da66a70fff60a69b4215077942a9afe6dcc7f28d354ba9deb6d426d9` |
| Debian's | Debian (2.16.03-1, amd64) | `b0e9b463a6f800117de60c0a55507b7880dfa0d4dbf15f47e1350e23c54c0128` |

nasm-V is the same bytes as the nasm of L3b (above), rebuilt.

### The result: identical
**Limine with nasm-V and with nasm-T: 6 of 6 files identical.** Limine
with Debian's nasm: 6 of 6 identical as well. No difference to explain.
The hashes are the table in *Limine 12.9.2 from source*, above.
- The run: 2026-10-30, finished 06:17:52 UTC, about 1 min in all; the
  guest booted at 06:17:06 and built nasm-T in 14 s. Log
  `~/vlog-ddc/nasm-ddc.log`, guest console
  `~/ddc/logs/nasm-t-20261030T061706.log`, outputs `~/ddc/nasm-ddc/out`.
- **Run twice:** the second run (log `~/vlog-ddc/nasm-ddc-rerun.log`)
  gave the same nasm-T, nasm-V and Limine bytes.
- The command: `tools/ddc/nasm-ddc.sh
  ~/ddc/toolchain/llvm-21.1.8-verified/bin ~/ddc/nasm-ddc/src
  ~/ddc/nasm-ddc/out` (`JOBS=4`, `CORES=4` by default).
