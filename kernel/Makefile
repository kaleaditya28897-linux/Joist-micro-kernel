# Makefile for the Joist kernel (Phase 2, docs/phase2-plan.md). Plain Make (§13).
#
#   make            the host tests (kernel data structures built for the host, §8.1)
#   make check      the host tests under plain, ASan, UBSan, MSan, TSan, and the
#                   freestanding compile of every kernel source
#   make freestanding  compile every kernel source for x86_64-unknown-none-elf
#                   with the §13 flags (objects only: linking needs ld.lld)
#   make kernel     link the kernel image, build/joist.elf (LD, default ld.lld)
#   make iso        a bootable ISO with Limine, build/joist.iso (BIOS and UEFI)
#   make qemu-test  boot it headless in QEMU and check the serial log
#                   (FIRMWARE=bios, the default, or uefi)
#   make boot-test  qemu-test under both
#   make mutate     injected bugs in test/mutations.txt must fail the host tests
#   make fuzz-elf   libFuzzer on the ELF validator (FUZZ_TIME seconds, default 60)
#   make format     clang-format in place
#   make clean

ifeq ($(origin CC),default)
CC := clang
endif

WARN := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wvla
INC  := -Iinclude -Imem -Ielf

# Kernel sources that also build for the host. Grows as Phase 2 adds them.
KSRC  := mem/frame.c elf/elf.c
TESTS := $(wildcard test/test_*.c)
RUNNER := test/harness.c
BUILD := build

# §13's kernel flags.
KFLAGS := --target=x86_64-unknown-none-elf -std=c17 -ffreestanding -nostdlib -fno-builtin \
          -fno-stack-protector -fno-pic -mcmodel=kernel -mno-red-zone -mgeneral-regs-only \
          -O2 -g $(WARN)

SANITIZERS := asan ubsan msan tsan
SAN_asan  := -fsanitize=address
SAN_ubsan := -fsanitize=undefined -fsanitize-trap=undefined
SAN_msan  := -fsanitize=memory -fsanitize-memory-track-origins
SAN_tsan  := -fsanitize=thread

STAMP := $(BUILD)/sources.list
$(shell mkdir -p $(BUILD); echo '$(KSRC) $(TESTS)' | cmp -s - $(STAMP) || echo '$(KSRC) $(TESTS)' > $(STAMP))

.PHONY: all test check freestanding kernel iso qemu-test boot-test mutate fuzz-elf format clean $(SANITIZERS)
all: test

define variant
$(BUILD)/$(1)/run-tests: $(KSRC) $(TESTS) $(RUNNER) $(STAMP) $(wildcard include/joist/*.h mem/*.h elf/*.h test/*.h)
	@mkdir -p $$(@D)
	$$(CC) -std=c17 -O1 -g -fno-omit-frame-pointer $$(WARN) $$(INC) -Itest $(2) \
	    $$(KSRC) $$(TESTS) $$(RUNNER) -o $$@
endef
$(eval $(call variant,plain,))
$(foreach s,$(SANITIZERS),$(eval $(call variant,$(s),$(SAN_$(s)))))

test: $(BUILD)/plain/run-tests
	./$<

$(SANITIZERS): %: $(BUILD)/%/run-tests
	./$<

freestanding:
	@mkdir -p $(BUILD)/freestanding
	@for f in $(KSRC); do \
	    $(CC) $(KFLAGS) $(INC) -c $$f -o $(BUILD)/freestanding/$$(basename $$f .c).o || exit 1; \
	done
	@echo "freestanding: $(words $(KSRC)) kernel source(s) compile for x86_64-unknown-none-elf"

# ---- the kernel image (Phase 2 step 1) ----
# Everything that goes into the image. KSRC (host-testable) is a subset.
IMG_SRC := arch/x86_64/boot.c arch/x86_64/serial.c lib/kprint.c lib/string.c $(KSRC)
IMG_INC := $(INC) -Iarch/x86_64
LD ?= ld.lld
ifeq ($(origin LD),default)
LD := ld.lld
endif
IMG_OBJ := $(patsubst %.c,$(BUILD)/kernel/%.o,$(IMG_SRC))

$(BUILD)/kernel/%.o: %.c $(wildcard include/joist/*.h include/limine/*.h arch/x86_64/*.h mem/*.h elf/*.h)
	@mkdir -p $(@D)
	$(CC) $(KFLAGS) $(IMG_INC) -c $< -o $@

$(BUILD)/joist.elf: $(IMG_OBJ) arch/x86_64/linker.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T arch/x86_64/linker.ld \
	    $(IMG_OBJ) -o $@

kernel: $(BUILD)/joist.elf

# Limine's files, from the distribution package (Void: /usr/share/limine).
LIMINE_DIR ?= /usr/share/limine
$(BUILD)/joist.iso: $(BUILD)/joist.elf ../boot/limine.conf
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/limine $(BUILD)/iso/EFI/BOOT
	cp $(BUILD)/joist.elf $(BUILD)/iso/boot/joist.elf
	cp ../boot/limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	    $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(BUILD)/iso/EFI/BOOT/
	xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot \
	    -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(BUILD)/iso -o $@ 2>/dev/null
	limine bios-install $@ 2>/dev/null

iso: $(BUILD)/joist.iso

# Headless boot: serial to a file, QEMU's debug-exit device turns the
# kernel's verdict into QEMU's exit status ((code << 1) | 1, so 1 = success).
QEMU ?= qemu-system-x86_64
QEMU_TIMEOUT ?= 30
# FIRMWARE=bios (default) or uefi (QEMU's EDK2 build).
FIRMWARE ?= bios
UEFI_CODE ?= /usr/share/qemu/edk2-x86_64-code.fd
FW_bios :=
FW_uefi := -drive if=pflash,format=raw,readonly=on,file=$(UEFI_CODE)
qemu-test: $(BUILD)/joist.iso
	@rm -f $(BUILD)/serial.log
	@timeout $(QEMU_TIMEOUT) $(QEMU) -M q35 -m 256M $(FW_$(FIRMWARE)) -cdrom $< -display none -no-reboot \
	    -serial file:$(BUILD)/serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04; \
	    st=$$?; cat $(BUILD)/serial.log; \
	    if [ $$st -eq 1 ] && grep -q "joist: ok" $(BUILD)/serial.log; then \
	        echo "qemu-test: booted ($(FIRMWARE)), kernel reported ok"; \
	    else echo "qemu-test: FAILED ($(FIRMWARE), QEMU status $$st)"; exit 1; fi

# Boot under both firmwares.
boot-test: $(BUILD)/joist.iso
	@$(MAKE) -s qemu-test FIRMWARE=bios
	@$(MAKE) -s qemu-test FIRMWARE=uefi

check: test $(SANITIZERS) freestanding boot-test
	@echo "check: host tests pass under plain, $(SANITIZERS); kernel sources compile freestanding; the kernel boots under BIOS and UEFI"

mutate:
	@sh test/mutate.sh "$(CC)"

FUZZ_TIME ?= 60
$(BUILD)/fuzz/fuzz-elf: elf/elf.c fuzz/fuzz_elf.c $(wildcard include/joist/*.h elf/*.h test/elf_promises.h)
	@mkdir -p $(@D) $(BUILD)/fuzz-elf-corpus
	$(CC) -std=c17 -O1 -g $(WARN) $(INC) -Itest -fsanitize=fuzzer,address,undefined \
	    -fsanitize-trap=undefined elf/elf.c fuzz/fuzz_elf.c -o $@

fuzz-elf: $(BUILD)/fuzz/fuzz-elf
	./$< -max_total_time=$(FUZZ_TIME) -max_len=8192 -artifact_prefix=$(BUILD)/ \
	    -print_final_stats=1 $(BUILD)/fuzz-elf-corpus fuzz/elf-seeds

format:
	clang-format --style=file:../.clang-format -i $(IMG_SRC) $(TESTS) $(RUNNER) fuzz/*.c \
	    $(wildcard include/joist/*.h arch/x86_64/*.h mem/*.h elf/*.h test/*.h)

clean:
	rm -rf $(BUILD)
