# Makefile for the Joist kernel (Phase 2, docs/phase2-plan.md). Plain Make (§13).
#
#   make            the host tests (kernel data structures built for the host, §8.1)
#   make check      the host tests under plain, ASan, UBSan, MSan, TSan, and the
#                   freestanding compile of every kernel source
#   make freestanding  compile every kernel source for x86_64-unknown-none-elf
#                   with the §13 flags (objects only: linking needs ld.lld)
#   make mutate     injected bugs in test/mutations.txt must fail the host tests
#   make format     clang-format in place
#   make clean

ifeq ($(origin CC),default)
CC := clang
endif

WARN := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -Wstrict-prototypes \
        -Wmissing-prototypes -Wvla
INC  := -Iinclude -Imem

# Kernel sources that also build for the host. Grows as Phase 2 adds them.
KSRC  := mem/frame.c
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

.PHONY: all test check freestanding mutate format clean $(SANITIZERS)
all: test

define variant
$(BUILD)/$(1)/run-tests: $(KSRC) $(TESTS) $(RUNNER) $(STAMP) $(wildcard include/joist/*.h mem/*.h test/*.h)
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

check: test $(SANITIZERS) freestanding
	@echo "check: host tests pass under plain, $(SANITIZERS); kernel sources compile freestanding"

mutate:
	@sh test/mutate.sh "$(CC)"

format:
	clang-format --style=file:../.clang-format -i $(KSRC) $(TESTS) $(RUNNER) \
	    $(wildcard include/joist/*.h mem/*.h test/*.h)

clean:
	rm -rf $(BUILD)
