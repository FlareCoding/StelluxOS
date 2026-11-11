#
# Stellux stlxterm Host Tests
#
# Builds each *.test.c with `SOURCES` by the build machine's own compiler and runs it, for `make host-test`.
#

HOST_CC ?= clang

SRC_DIR   := ../src
BUILD_DIR := ../build/host
SOURCES   := $(addprefix $(SRC_DIR)/,term.c select.c paste.c)
HEADERS   := $(addprefix $(SRC_DIR)/,term.h select.h paste.h)
TESTS     := $(patsubst %.test.c,$(BUILD_DIR)/%,$(wildcard *.test.c))

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR)

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(BUILD_DIR)/%: %.test.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(CFLAGS) $< $(SOURCES) -o $@

.PHONY: run
