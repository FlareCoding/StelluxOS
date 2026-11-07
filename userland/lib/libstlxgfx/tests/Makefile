#
# Stellux Userland libstlxgfx Host Tests
#
# Builds each *.test.c with the sources in `SOURCES` by the build machine's own
# compiler and runs it. The top-level `make host-test` runs this.
#

HOST_CC ?= clang

REPO_ROOT := $(abspath ../../../..)
SRC_DIR   := ../src
BUILD_DIR := ../build/host
SOURCES   := $(SRC_DIR)/surface.c $(SRC_DIR)/text.c
HEADERS   := $(wildcard ../include/stlxgfx/*.h ../include/stlxgfx/internal/*.h)
TESTS     := $(patsubst %.test.c,$(BUILD_DIR)/%,$(wildcard *.test.c))

CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -I../include \
	-DINITRD_DIR='"$(REPO_ROOT)/initrd"'

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(BUILD_DIR)/%: %.test.c $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(CFLAGS) $< $(SOURCES) -o $@ -lm

.PHONY: run
