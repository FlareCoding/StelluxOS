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

# Build all apps, then install to initrd
all: apps install

apps:
	@$(MAKE) -C apps ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V)

install: apps
	@mkdir -p $(INITRD_BIN)
	@echo "Installing userland binaries to initrd..."
	@if [ -d "$(BIN_DIR)" ] && [ -n "$$(ls -A $(BIN_DIR) 2>/dev/null)" ]; then \
		cp $(BIN_DIR)/* $(INITRD_BIN)/; \
		echo "Installed: $$(ls $(BIN_DIR)/)"; \
	else \
		echo "No binaries to install"; \
	fi

clean:
	@echo "Cleaning userland build artifacts..."
	@$(MAKE) -C apps clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT)
	@rm -rf build

.PHONY: all apps install clean
