# Makefile for the Phase 1 host-side semantic model (plan §11 Phase 1, §8.1).
# Plain Make, KISS (§13). The model's code is the kernel's object layer,
# ../kernel/core (moved there in Phase 3, step 2); this directory holds its
# host test runner, fuzzer, CBMC harness and TLA+ model.
#
#   make            build and run the tests (plain build)
#   make asan       ... under AddressSanitizer
#   make ubsan      ... under UndefinedBehaviorSanitizer (traps, no recovery)
#   make msan       ... under MemorySanitizer
#   make tsan       ... under ThreadSanitizer
#   make small      ... with tiny capacities (exhaustion paths), under ASan + UBSan
#   make check      all of the above: the Phase 1 bar is "every test, every sanitizer"
#   make sanity     prove each sanitizer is live: a known-bad program must be caught
#   make mutate     prove the property tests catch each bug in test/mutations.txt
#   make fuzz       libFuzzer over the whole model (FUZZ_TIME seconds, default 60)
#   make fuzz-sanity prove the fuzzer finds injected bugs (FUZZ_SANITY list)
#   make tla        TLC on tla/IpcDelete.tla: IPC racing preemptible destruction
#   make tla-mutate prove the TLA+ properties catch each bug in tla/mutate_tla.py
#   make cbmc       CBMC preservation proofs for the capability, task and mapping operations
#   make format     clang-format the model's sources in place
#   make clean

# CC is a built-in Make variable (origin 'default', value 'cc'); `?=` would
# never fire and the build would silently use cc (GCC on most distros). Guard
# on origin, as mcp-server/Makefile does and decision 0002 requires.
ifeq ($(origin CC),default)
CC := clang
endif

CSTD     := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
            -Wstrict-prototypes -Wmissing-prototypes -Wvla
CORE     := ../kernel/core
# Table sizes for the host tests: two leaves of 64 slots, so leaf 0 (built
# into each space) holds every slot the older tests use, and growing,
# shrinking and absent leaves are exercised too. The kernel's are in core.h.
CNODE    := -DJOIST_CNODE_ROOT=2u -DJOIST_CNODE_LEAF=64u -DJOIST_MAX_LEAVES=8u
CPPFLAGS := -I../kernel/include -I$(CORE) $(CNODE) -D_POSIX_C_SOURCE=200809L
OPT      ?= -O1 -g -fno-omit-frame-pointer

SRC      := $(wildcard $(CORE)/*.c)
TESTS    := $(wildcard test/test_*.c)
RUNNER   := test/harness.c
EXEC     := test/exec.c

BUILD    := build
# Make only rebuilds when a prerequisite is newer, and a deleted file never
# is, so a removed test would linger in a stale binary. The stamp holds the
# current source list and is rewritten only when that list changes.
STAMP    := $(BUILD)/sources.list
$(shell mkdir -p $(BUILD); echo '$(SRC) $(TESTS)' | cmp -s - $(STAMP) || echo '$(SRC) $(TESTS)' > $(STAMP))

SANITIZERS := asan ubsan msan tsan small
SAN_asan  := -fsanitize=address
SAN_ubsan := -fsanitize=undefined -fsanitize-trap=undefined
SAN_msan  := -fsanitize=memory -fsanitize-memory-track-origins
SAN_tsan  := -fsanitize=thread
# Tiny capacities, so every table-exhaustion path (objects, threads, no-senders
# accounts, dead-name registrations) is hit constantly; with ASan and UBSan.
SAN_small := -DJOIST_MAX_OBJECTS=32u -DJOIST_MAX_THREADS=8u -DJOIST_MAX_BADGE_ACCTS=4u \
             -DJOIST_MAX_DEADNAMES=4u -UJOIST_MAX_LEAVES -DJOIST_MAX_LEAVES=2u -fsanitize=address,undefined -fsanitize-trap=undefined

.PHONY: all test check sanity mutate fuzz fuzz-sanity tla tla-mutate cbmc format clean $(SANITIZERS)

all: test

# $(1): variant name, $(2): extra flags
define variant
$(BUILD)/$(1)/run-tests: $(SRC) $(TESTS) $(RUNNER) $(EXEC) $(STAMP) $(wildcard ../kernel/include/joist/core.h $(CORE)/*.h test/*.h)
	@mkdir -p $$(@D)
	$$(CC) $$(CSTD) $$(OPT) $$(WARN) $$(CPPFLAGS) $(2) $$(SRC) $$(TESTS) $$(RUNNER) $$(EXEC) -o $$@ -lpthread
endef

$(eval $(call variant,plain,))
$(foreach s,$(SANITIZERS),$(eval $(call variant,$(s),$(SAN_$(s)))))

test: $(BUILD)/plain/run-tests
	./$<

$(SANITIZERS): %: $(BUILD)/%/run-tests
	./$<

check: test $(SANITIZERS)
	@echo "check: all tests passed under plain, $(SANITIZERS)"

# Each sanitizer must actually catch a seeded bug, so a silently disabled
# sanitizer can never make `make check` pass vacuously.
sanity:
	@sh test/sanitizer-sanity.sh "$(CC)"

# Each listed bug, injected into a scratch copy, must fail the property tests.
mutate:
	@sh test/mutate.sh "$(CC)"

# The fuzzer drives the same executor as test_props_all.c from fuzz input,
# under ASan and UBSan. A crash leaves its input in build/ as crash-*; replay
# it with ./build/fuzz/fuzz-model build/crash-<hash>.
FUZZ_TIME ?= 60
$(BUILD)/fuzz/fuzz-model: $(SRC) $(EXEC) fuzz/fuzz_model.c $(wildcard ../kernel/include/joist/core.h $(CORE)/*.h test/*.h)
	@mkdir -p $(@D) $(BUILD)/fuzz-corpus
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) -Itest \
	    -fsanitize=fuzzer,address,undefined -fsanitize-trap=undefined \
	    $(SRC) $(EXEC) fuzz/fuzz_model.c -o $@

# Seed corpus: the property test's first FUZZ_SEEDS sequences, recorded in the
# fuzz input encoding (fuzz/make_seeds.c), FUZZ_SEED_STEPS steps each.
FUZZ_SEEDS      ?= 32
FUZZ_SEED_STEPS ?= 1000
$(BUILD)/fuzz/make-seeds: $(SRC) $(EXEC) fuzz/make_seeds.c $(wildcard ../kernel/include/joist/core.h $(CORE)/*.h test/*.h)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) -Itest $(SRC) $(EXEC) fuzz/make_seeds.c -o $@

$(BUILD)/fuzz-seeds/stamp: $(BUILD)/fuzz/make-seeds
	@rm -rf $(@D); mkdir -p $(@D)
	./$< $(@D) $(FUZZ_SEEDS) $(FUZZ_SEED_STEPS)
	@touch $@

fuzz: $(BUILD)/fuzz/fuzz-model $(BUILD)/fuzz-seeds/stamp
	@mkdir -p $(BUILD)/fuzz-corpus
	./$< -max_total_time=$(FUZZ_TIME) -max_len=8192 -len_control=0 -artifact_prefix=$(BUILD)/ \
	    -print_final_stats=1 $(BUILD)/fuzz-corpus $(BUILD)/fuzz-seeds

# A few mutations from test/mutations.txt, one per area; each must crash the
# fuzzer started from the seed corpus within FUZZ_SANITY_TIME seconds. Each is
# also tried from an empty corpus for FUZZ_BLIND_TIME seconds, reported only.
FUZZ_SANITY      ?= I1 refcount|I3 refund|I6 reply reusable|I2 bits lost|I5 never dies|task-quota-lost|task-no-cascade
FUZZ_SANITY_TIME ?= 300
FUZZ_BLIND_TIME  ?= 120
fuzz-sanity: $(BUILD)/fuzz-seeds/stamp
	@sh test/fuzz-sanity.sh "$(CC)" $(FUZZ_SANITY_TIME) '$(FUZZ_SANITY)' $(FUZZ_BLIND_TIME)

# Work item 8: the TLA+ model of IPC racing a preemptible task destruction.
# The two safe configurations must pass; the unsafe one (capabilities may be
# passed into a dying space) must fail on SpaceEmptied, the leak it shows.
# TLC is tla2tools.jar's tlc2.TLC; TLC names the command that runs it.
TLC ?= tlc
TLA_META = $(BUILD)/tla
tla:
	@mkdir -p $(TLA_META)
	cd tla && $(TLC) -workers auto -metadir ../$(TLA_META)/safe -config IpcDelete.cfg IpcDelete.tla \
	    > ../$(TLA_META)/safe.log 2>&1 && grep -q "No error has been found" ../$(TLA_META)/safe.log
	cd tla && $(TLC) -workers auto -metadir ../$(TLA_META)/large -config IpcDelete_large.cfg IpcDelete.tla \
	    > ../$(TLA_META)/large.log 2>&1 && grep -q "No error has been found" ../$(TLA_META)/large.log
	cd tla && ! $(TLC) -workers auto -metadir ../$(TLA_META)/unsafe -config IpcDelete_unsafe.cfg IpcDelete.tla \
	    > ../$(TLA_META)/unsafe.log 2>&1; grep -qE "Invariant (SpaceEmptied|TablesEmptied) is violated" ../$(TLA_META)/unsafe.log
	@for c in safe large; do printf "tla: %-5s " $$c; grep "distinct states found" $(TLA_META)/$$c.log | tail -1; done
	@echo "tla: safe and large configurations hold; the unsafe one leaks, as expected"

tla-mutate:
	python3 tla/mutate_tla.py "$(TLC)"

# CBMC (§8.1): for each capability and task operation, from any state the
# invariant check accepts, the operation keeps every invariant (and every
# array access, pointer and shift is checked on the way). Operation numbers
# are those of cbmc/harness_cspace.c (7, 8: grow, shrink; 9, 10: obj_kill,
# cap_copy_batch, 0022); CBMC_JOBS run at once (each needs 3.5 to 6.5 GB, so
# one at a time on the 8 GB build VM, with nothing else on it; all fifteen
# take about 6 hours). Operations 3 (delete) and 6 (task destroy) can cascade; their
# formulas exhaust 15 GB even at smaller sizes, split by slot type, with
# CaDiCaL, or path by path, so they are left to the property tests, the
# fuzzer and the TLA+ model (docs/phase1-plan.md, work item 8). The page
# tables are their smallest (nodes of 2 entries, one top-level entry;
# cbmc/run.sh). Operations 11-16 are the mapping calls (map, map_replace,
# unmap_range, protect, space_donate, map_query; 0020, 0021), from states with page
# tables in simulated memory; the others from states without (a condemned
# frame's sweep, which kill and delete may start, is left to the property
# tests and the mutations).
CBMC_OPS  ?= 0 1 2 4 5 7 8 9 10 11 12 13 14 15 16
CBMC_JOBS ?= 1
cbmc:
	@mkdir -p $(BUILD)/cbmc
	@rm -f $(BUILD)/cbmc/summary.txt
	@printf "%s\n" $(CBMC_OPS) | xargs -P$(CBMC_JOBS) -I{} sh cbmc/run.sh $(BUILD)/cbmc {} \
	    | sort | tee $(BUILD)/cbmc/summary.txt
	@# Each operation by its own line: a run killed (out of memory: it says
	@# nothing) is not verified, whatever else the file holds (2026-10-19, a
	@# count of the file's lines passed one with op0 killed).
	@n=0; for op in $(CBMC_OPS); do \
	    grep -q "^op$$op: .*VERIFICATION SUCCESSFUL" $(BUILD)/cbmc/summary.txt && n=$$((n + 1)) || \
	        echo "cbmc: op$$op not verified ($(BUILD)/cbmc/op$$op.log)"; \
	done; \
	    test $$n -eq $(words $(CBMC_OPS)) && echo "cbmc: all $$n operations verified" || \
	    { echo "cbmc: $$n of $(words $(CBMC_OPS)) verified"; exit 1; }

format:
	clang-format --style=file:../.clang-format -i $(SRC) $(TESTS) $(RUNNER) $(EXEC) \
	    fuzz/*.c cbmc/*.c $(wildcard ../kernel/include/joist/core.h $(CORE)/*.h test/*.h)

clean:
	rm -rf $(BUILD)
