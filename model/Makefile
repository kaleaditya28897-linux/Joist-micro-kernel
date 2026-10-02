# Makefile for the Phase 1 host-side semantic model (plan §11 Phase 1, §8.1).
# Plain Make, KISS (§13). The model is a C17 library plus a test runner.
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
CPPFLAGS := -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L
OPT      ?= -O1 -g -fno-omit-frame-pointer

SRC      := $(wildcard src/*.c)
TESTS    := $(wildcard test/test_*.c)
RUNNER   := test/harness.c

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
             -DJOIST_MAX_DEADNAMES=4u -fsanitize=address,undefined -fsanitize-trap=undefined

.PHONY: all test check sanity mutate format clean $(SANITIZERS)

all: test

# $(1): variant name, $(2): extra flags
define variant
$(BUILD)/$(1)/run-tests: $(SRC) $(TESTS) $(RUNNER) $(STAMP) $(wildcard include/joist/*.h src/*.h test/*.h)
	@mkdir -p $$(@D)
	$$(CC) $$(CSTD) $$(OPT) $$(WARN) $$(CPPFLAGS) $(2) $$(SRC) $$(TESTS) $$(RUNNER) -o $$@ -lpthread
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

format:
	clang-format --style=file:../.clang-format -i $(SRC) $(TESTS) $(RUNNER) \
	    $(wildcard include/joist/*.h src/*.h test/*.h)

clean:
	rm -rf $(BUILD)
