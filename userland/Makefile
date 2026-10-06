#
# Stellux 3.0 - Userland Master Makefile
#
# Builds userland applications and installs them to the initrd.
# Requires musl sysroot to be set up first (make musl from top-level).
#
# Usage:
#   make ARCH=x86_64          # Build all userland apps
#   make clean ARCH=x86_64    # Clean build artifacts
#

export USERLAND_ROOT := $(CURDIR)

SUPPORTED_ARCHS := x86_64 aarch64

# ARCH validation
ifndef ARCH
  $(error ARCH is required. Use: make ARCH=x86_64 or ARCH=aarch64)
endif
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),)
  $(error Invalid ARCH=$(ARCH). Supported: $(SUPPORTED_ARCHS))
endif

# Sysroot check
SYSROOT := $(USERLAND_ROOT)/sysroot/$(ARCH)
ifeq ($(wildcard $(SYSROOT)/lib/libc.a),)
  $(error musl sysroot not found at $(SYSROOT). Run 'make musl' from the top-level directory first)
endif

# Output directories, the image build assembles the initrd from both
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin
ROOTFS_DIR := $(USERLAND_ROOT)/build/$(ARCH)/rootfs

# Verbosity
ifeq ($(V),1)
  SUBMAKE_FLAGS :=
  export STLX_QUIET :=
else
  SUBMAKE_FLAGS := --no-print-directory
  export STLX_QUIET := 1
endif

# Steps run in order, only the compiles inside each library and app run in parallel
.NOTPARALLEL:

all: summary

build: sdk
ifeq ($(V),1)
	@$(MAKE) -C lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V)
	@$(MAKE) -C apps ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V)
else
	@echo ""
	@echo "  Userland ($(ARCH))"
	@echo ""
	@$(MAKE) $(SUBMAKE_FLAGS) -C lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@echo ""
	@$(MAKE) $(SUBMAKE_FLAGS) -C apps ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@echo ""
endif

summary: build packages
ifeq ($(V),1)
	@echo "Built binaries: $$(ls $(BIN_DIR)/ 2>/dev/null | tr '\n' ' ')"
	@if [ -d "$(ROOTFS_DIR)" ] && [ -n "$$(ls -A $(ROOTFS_DIR) 2>/dev/null)" ]; then \
		echo "Package overlay staged in $(ROOTFS_DIR)"; \
	else \
		echo "No package overlay"; \
	fi
else
	@bincount=$$(ls -1 $(BIN_DIR)/ 2>/dev/null | wc -l); \
	if [ -d "$(ROOTFS_DIR)" ] && [ -n "$$(ls -A $(ROOTFS_DIR) 2>/dev/null)" ]; then \
		rootfsword=", package overlay staged"; \
	else \
		rootfsword=""; \
	fi; \
	libcount=$$(ls -1 $(SYSROOT)/lib/libstlx.a 2>/dev/null | wc -l); \
	if [ $$libcount -eq 1 ]; then libword="library"; else libword="libraries"; fi; \
	printf "  $$libcount $$libword, $$bincount binaries built$$rootfsword\n"
endif

clean:
	@echo "Cleaning userland build artifacts..."
	@$(MAKE) -C lib clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@$(MAKE) -C apps clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@rm -rf build

include mk/toolchain.mk
include mk/packages.mk

CC_SOURCES := $(shell find apps lib -name '*.c' 2>/dev/null | sort)
CC_FLAGS := $(CFLAGS_COMMON)

compile-commands:
	@echo "Generating userland compile_commands.json..."
	@echo "[" > compile_commands.json
	@first=1; \
	for src in $(CC_SOURCES); do \
		if [ $$first -eq 1 ]; then first=0; else echo "," >> compile_commands.json; fi; \
		extra_flags=""; \
		case "$$src" in \
			lib/libstlxgfx/*) extra_flags="-Ilib/libstlxgfx/include" ;; \
			lib/libstlx/*) extra_flags="-Ilib/libstlx/include" ;; \
		esac; \
		args="\"$(CC)\""; \
		for flag in $(CC_FLAGS) $$extra_flags; do args="$$args, \"$$flag\""; done; \
		args="$$args, \"-c\", \"$$src\""; \
		echo "  {" >> compile_commands.json; \
		echo "    \"directory\": \"$$(pwd)\"," >> compile_commands.json; \
		echo "    \"file\": \"$$src\"," >> compile_commands.json; \
		echo "    \"arguments\": [$$args]" >> compile_commands.json; \
		echo "  }" >> compile_commands.json; \
	done
	@echo "]" >> compile_commands.json
	@echo "Created userland/compile_commands.json"

.PHONY: all build summary clean compile-commands
