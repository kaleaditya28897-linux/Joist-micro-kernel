# SPDX-License-Identifier: GPL-3.0-only
# Makefile for joist-mcp. Plain Make, KISS (matches the plan's tooling policy).
#
# Build with either compiler:
#   make            # uses clang (project default)
#   make CC=gcc     # glibc-side fallback compiler is allowed
#
# The strict warning set applies to Joist's own code; the vendored cJSON is
# built with warnings relaxed since it is upstream code we do not own.

# CC is a built-in Make variable (origin 'default', value 'cc'), so `?=` would
# never fire and the build would silently use cc -- which is gcc on most
# distros. Guard on origin so the project default is clang, while an
# exported CC or `make CC=gcc` still wins.
ifeq ($(origin CC),default)
CC      := clang
endif
CSTD    := -std=c17
OPT     ?= -O2 -g

WARN    := -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
           -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -Isrc -Ivendor/cjson -D_POSIX_C_SOURCE=200809L
CFLAGS  ?= $(CSTD) $(OPT)

BIN     := joist-mcp
OWN_SRC := src/main.c src/plan.c
VEND_SRC:= vendor/cjson/cJSON.c
OWN_OBJ := $(OWN_SRC:.c=.o)
VEND_OBJ:= $(VEND_SRC:.c=.o)

.PHONY: all clean test format

all: $(BIN)

$(BIN): $(OWN_OBJ) $(VEND_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

# Our own code: full strict warning set, warnings are errors.
$(OWN_OBJ): %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c $< -o $@

# Vendored cJSON: build without -Werror so upstream stays buildable.
$(VEND_OBJ): %.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -c $< -o $@

test: $(BIN)
	./test/run.sh ./$(BIN)

format:
	clang-format -i $(OWN_SRC) src/plan.h

clean:
	rm -f $(BIN) $(OWN_OBJ) $(VEND_OBJ)
