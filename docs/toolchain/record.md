# The toolchain record (decision 0026, layers 2 and 3)

What was built, from what, with what, and what it hashed to. Anyone with
the same seed and sources can rerun `tools/ddc/bootstrap.sh` and the L3
steps below and compare.

**State (2026-10-29):** layer 2 done: the bootstrap ran to GCC 15.2.0.
The seed's reading is **pending**: until the owner has read it (docs/toolchain/seed-review.md),
the bootstrapped GCC is **not trusted**, and neither is anything verified
against it.

## Layer 2: the trusted compiler

### live-bootstrap
- **Repository:** https://github.com/fosslinux/live-bootstrap
- **Pinned:** commit `b1ceced7ea8a819a26f23796f6dd7ae8496a33a4` (master,
  2026-09-17, "Merge pull request #587"). It has **no tagged release**
  (no tags, no GitHub releases, 2026-10-29). See *Proposed*, below.
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

**The owner's reading of the seed: pending.**

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

**Trusted: no, not yet.** This GCC is trusted only once the owner has
read the seed (seed-review.md), and with it everything that is verified
against it.

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

(to fill in: the stages, the comparison)

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

## Proposed, awaiting the owner
1. **live-bootstrap pinned by commit**, since it has no tagged release.
2. **All stage 2 builds in the bootstrapped system** (option 1), with
   static i686 stages.
