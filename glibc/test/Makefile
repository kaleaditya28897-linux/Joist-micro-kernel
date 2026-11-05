# SPDX-License-Identifier: GPL-3.0-only
# Host tests of Joist's files for glibc (../sysdeps/joist), Phase 5 step 7:
# the futex queues (test_gsync.c) and the address reservations (test_vm.c).
#
#   make           the tests, plain build
#   make check     ... and under ASan, UBSan, MSan and TSan
#   make mutate    every mutation in mutations.txt must be caught
#   make cbmc      CBMC on the queue operations (cbmc_gsync.c)
#   make clean
ifeq ($(origin CC),default)
CC := clang
endif
SRC_DIR  := ../sysdeps/joist
CSTD     := -std=c17
WARN     := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wvla
CPPFLAGS := -I$(SRC_DIR) -I. -D_GNU_SOURCE
OPT      ?= -O1 -g -fno-omit-frame-pointer
BUILD    := build
SEEDS    ?= 20
THREADS  ?= 8

GSYNC    := $(SRC_DIR)/joist-gsync.c test_gsync.c notif_host.c
VM       := $(SRC_DIR)/joist-vm.c test_vm.c
VM_SMALL := -DJV_MAX=6u -DJV_USER_PAGES=64ull -DJV_LO=8ull -DJV_HI=48ull

VARIANTS := plain asan ubsan msan tsan
SAN_plain :=
SAN_asan  := -fsanitize=address
SAN_ubsan := -fsanitize=undefined -fsanitize-trap=undefined
SAN_msan  := -fsanitize=memory -fsanitize-memory-track-origins
SAN_tsan  := -fsanitize=thread

.PHONY: all test check mutate cbmc clean $(VARIANTS)

all: test

define variant
$(BUILD)/$(1)/test-gsync: $(GSYNC) $(SRC_DIR)/joist-gsync.h notif_host.h
	@mkdir -p $$(@D)
	$$(CC) $$(CSTD) $$(OPT) $$(WARN) $$(CPPFLAGS) $(SAN_$(1)) $(GSYNC) -o $$@ -lpthread
$(BUILD)/$(1)/test-vm: $(VM) $(SRC_DIR)/joist-vm.h
	@mkdir -p $$(@D)
	$$(CC) $$(CSTD) $$(OPT) $$(WARN) $$(CPPFLAGS) $(VM_SMALL) $(SAN_$(1)) $(VM) -o $$@
$(1): $(BUILD)/$(1)/test-gsync $(BUILD)/$(1)/test-vm
	./$(BUILD)/$(1)/test-gsync $$(SEEDS) $$(THREADS)
	./$(BUILD)/$(1)/test-vm
endef
$(foreach v,$(VARIANTS),$(eval $(call variant,$(v))))

test: plain
check: $(VARIANTS)

mutate:
	@sh mutate.sh "$(CC)"

cbmc:
	@sh cbmc.sh

clean:
	rm -rf $(BUILD)
