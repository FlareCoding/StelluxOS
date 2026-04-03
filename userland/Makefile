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

# Output directories
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin
INITRD_BIN := $(USERLAND_ROOT)/../initrd/bin

# Verbosity
ifeq ($(V),1)
  SUBMAKE_FLAGS :=
  export STLX_QUIET :=
else
  SUBMAKE_FLAGS := --no-print-directory
  export STLX_QUIET := 1
endif

all: build install-summary

build:
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

install-summary: build
	@mkdir -p $(INITRD_BIN)
ifeq ($(V),1)
	@echo "Installing userland binaries to initrd..."
	@if [ -d "$(BIN_DIR)" ] && [ -n "$$(ls -A $(BIN_DIR) 2>/dev/null)" ]; then \
		cp $(BIN_DIR)/* $(INITRD_BIN)/; \
		echo "Installed: $$(ls $(BIN_DIR)/)"; \
	else \
		echo "No binaries to install"; \
	fi
else
	@if [ -d "$(BIN_DIR)" ] && [ -n "$$(ls -A $(BIN_DIR) 2>/dev/null)" ]; then \
		cp $(BIN_DIR)/* $(INITRD_BIN)/; \
		bincount=$$(ls -1 $(BIN_DIR)/ | wc -l); \
		libcount=$$(ls -1 $(SYSROOT)/lib/libstlx.a 2>/dev/null | wc -l); \
		if [ $$libcount -eq 1 ]; then libword="library"; else libword="libraries"; fi; \
		printf "  $$libcount $$libword, $$bincount binaries installed to initrd/bin/\n"; \
	else \
		echo "  No binaries to install"; \
	fi
endif

clean:
	@echo "Cleaning userland build artifacts..."
	@$(MAKE) -C lib clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@$(MAKE) -C apps clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@rm -rf build

include mk/toolchain.mk

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

.PHONY: all build install-summary clean compile-commands
