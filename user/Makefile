# Makefile for the user programs: Phase 2's single-task tests
# (docs/phase2-plan.md) and Phase 3's (the root task, ping, pong: step 4;
# schedroot and worker, the scheduler test: step 5; faultroot and faulter,
# the fault test: step 6; cascaderoot, the destruction test: step 7;
# benchroot and ipcbench, the IPC benchmark: step 8; smproot, the
# multiprocessor test: step 9; faultinj and fiworker, the fault-injection
# test: the exit; docs/phase3-plan.md; scaleroot, the lock study's scaling
# test; revokeroot, decision 0022's calls; mm, the memory manager, with
# mmroot and mmclient, its test: Phase 4 step 6),
# which use lib/joist.h. Plain Make.
#   make        every program, into build/*.elf
ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin LD),default)
LD := ld.lld
endif
UFLAGS := --target=x86_64-unknown-none-elf -std=c17 -ffreestanding -nostdlib -fno-builtin \
          -fno-pic -fno-stack-protector -mgeneral-regs-only -O2 -g \
          -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes \
          -Wmissing-prototypes -Wvla
BUILD := build
# Extra flags, e.g. UEXTRA="-DSYSFUZZ_SEED=7 -DSYSFUZZ_CALLS=10000000".
UEXTRA ?=
UFLAGS += $(UEXTRA)
# The memory manager's table sizes (servers/mm/mmlib.h), for it and for the
# programs that talk to it.
MMFLAGS := -DMM_MAX_TASKS=16u -DMM_TASK_PAGES=512u -DMM_PAGES=1024u
UFLAGS += $(MMFLAGS)
MM := ../servers/mm
PROGS := hello badptr ud rowrite spin
# Programs with an assembly part: prog/prog.c plus prog/*.S.
ASMPROGS := sysfuzz sysrethole

.PHONY: all clean
# Phase 3 programs: lib/joist.h, plus the kernel's string functions (a
# freestanding build may call memset and memcpy) and, for the root task, its
# ELF validator.
LIBPROGS := ping pong root worker schedroot faulter faultroot cascaderoot ipcbench benchroot smproot scaleroot growroot fuzzchild \
            fiworker faultinj revokeroot mmroot mmclient mm

all: $(patsubst %,$(BUILD)/%.elf,$(PROGS) $(ASMPROGS) $(LIBPROGS))

$(BUILD)/string.o: ../kernel/lib/string.c
	@mkdir -p $(@D)
	$(CC) $(UFLAGS) -c $< -o $@

$(BUILD)/elf.o: ../kernel/elf/elf.c ../kernel/elf/elf.h
	@mkdir -p $(@D)
	$(CC) $(UFLAGS) -I../kernel/include -I../kernel/elf -c $< -o $@

define libprog
$(BUILD)/$(1).o: $(1)/$(1).c lib/joist.h lib/loader.h lib/work.h lib/testroot.h lib/fi.h ../kernel/elf/elf.h \
                 $(MM)/mm_proto.h $(MM)/mmlib.h
	@mkdir -p $$(@D)
	$$(CC) $$(UFLAGS) -I../kernel/include -I../kernel/elf -I$(MM) -c $$< -o $$@
$(BUILD)/$(1).elf: $(BUILD)/$(1).o $(BUILD)/string.o $(2) user.ld
	$$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T user.ld \
	    $(BUILD)/$(1).o $(BUILD)/string.o $(2) -o $$@
endef
$(eval $(call libprog,ping,))
$(eval $(call libprog,pong,))
$(eval $(call libprog,root,$(BUILD)/elf.o))
$(eval $(call libprog,worker,))
$(eval $(call libprog,schedroot,$(BUILD)/elf.o))
$(eval $(call libprog,faulter,))
$(eval $(call libprog,faultroot,$(BUILD)/elf.o))
$(eval $(call libprog,cascaderoot,$(BUILD)/elf.o))
$(eval $(call libprog,ipcbench,))
$(eval $(call libprog,benchroot,$(BUILD)/elf.o))
$(eval $(call libprog,smproot,$(BUILD)/elf.o))
$(eval $(call libprog,scaleroot,$(BUILD)/elf.o))
$(eval $(call libprog,growroot,$(BUILD)/elf.o))
$(eval $(call libprog,fuzzchild,))
$(eval $(call libprog,fiworker,))
$(eval $(call libprog,faultinj,$(BUILD)/elf.o))
$(eval $(call libprog,revokeroot,$(BUILD)/elf.o))
$(eval $(call libprog,mmroot,$(BUILD)/elf.o))
$(eval $(call libprog,mmclient,))

# The memory manager: its loop and its library, from servers/mm.
$(BUILD)/mmlib.o: $(MM)/mmlib.c $(MM)/mmlib.h
	@mkdir -p $(@D)
	$(CC) $(UFLAGS) -c $< -o $@
$(BUILD)/mm.o: $(MM)/mm.c $(MM)/mm_proto.h $(MM)/mmlib.h lib/joist.h
	@mkdir -p $(@D)
	$(CC) $(UFLAGS) -c $< -o $@
$(BUILD)/mm.elf: $(BUILD)/mm.o $(BUILD)/mmlib.o $(BUILD)/string.o user.ld
	$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T user.ld \
	    $(BUILD)/mm.o $(BUILD)/mmlib.o $(BUILD)/string.o -o $@

# One rule per program: a pattern can't use % twice (prog/prog.c).
define prog
$(BUILD)/$(1).o: $(1)/$(1).c sys.h
	@mkdir -p $$(@D)
	$$(CC) $$(UFLAGS) -c $$< -o $$@
endef
$(foreach p,$(PROGS),$(eval $(call prog,$(p))))

$(BUILD)/sysfuzz.elf: sysfuzz/sysfuzz.c sysfuzz/fuzz_call.S sys.h user.ld
	@mkdir -p $(@D)
	$(CC) $(UFLAGS) -c sysfuzz/sysfuzz.c -o $(BUILD)/sysfuzz.o
	$(CC) --target=x86_64-unknown-none-elf -c sysfuzz/fuzz_call.S -o $(BUILD)/fuzz_call.o
	$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T user.ld \
	    $(BUILD)/sysfuzz.o $(BUILD)/fuzz_call.o -o $@

$(BUILD)/%.elf: $(BUILD)/%.o user.ld
	$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T user.ld $< -o $@

clean:
	rm -rf $(BUILD)

$(BUILD)/sysrethole.elf: sysrethole/sysrethole.S sysrethole/sysrethole.ld
	@mkdir -p $(@D)
	$(CC) --target=x86_64-unknown-none-elf -c sysrethole/sysrethole.S -o $(BUILD)/sysrethole.o
	$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T sysrethole/sysrethole.ld \
	    $(BUILD)/sysrethole.o -o $@
