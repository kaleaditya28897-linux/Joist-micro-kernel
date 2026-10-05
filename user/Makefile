# Makefile for Phase 2's user programs (docs/phase2-plan.md). Plain Make.
#   make        build/hello.elf
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
PROGS := hello badptr ud rowrite spin

.PHONY: all clean
all: $(patsubst %,$(BUILD)/%.elf,$(PROGS))

# One rule per program: a pattern can't use % twice (prog/prog.c).
define prog
$(BUILD)/$(1).o: $(1)/$(1).c sys.h
	@mkdir -p $$(@D)
	$$(CC) $$(UFLAGS) -c $$< -o $$@
endef
$(foreach p,$(PROGS),$(eval $(call prog,$(p))))

$(BUILD)/%.elf: $(BUILD)/%.o user.ld
	$(LD) -static -nostdlib -z max-page-size=0x1000 -z noexecstack -T user.ld $< -o $@

clean:
	rm -rf $(BUILD)
