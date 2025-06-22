# =========================
# Userland Master Makefile
# =========================

# Toolchain Configuration
TOOLCHAIN_DIR := $(realpath ../toolchain/dist)
TOOLCHAIN_PREFIX := x86_64-linux-musl

# =========================
# Exported Toolchain Variables
# =========================
export CC := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-gcc
export CXX := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-g++
export LD := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ld
export AR := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ar
export RANLIB := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ranlib
export STRIP := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-strip
export OBJCOPY := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-objcopy
export OBJDUMP := $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-objdump

# =========================
# Exported Directory Variables
# =========================
export USERLAND_ROOT := $(CURDIR)
export USERLAND_BUILD_DIR := $(USERLAND_ROOT)/build
export USERLAND_LIB_DIR := $(USERLAND_BUILD_DIR)/lib
export USERLAND_BIN_DIR := $(USERLAND_BUILD_DIR)/bin
export USERLAND_INCLUDE_DIR := $(USERLAND_ROOT)/lib/stlibc/include
export USERLAND_SYSROOT := $(TOOLCHAIN_DIR)/$(TOOLCHAIN_PREFIX)
export INITRD_BIN_DIR := $(USERLAND_ROOT)/../initrd/bin

# =========================
# Exported Compiler Flags
# =========================
# Architecture and basic flags
export ARCH_FLAGS := -m64 -march=x86-64
export DEBUG_FLAGS := -g -O2
export WARNING_FLAGS := -Wall -Wextra -Werror

# Include paths
export LIBSTLXGFX_INCLUDE_DIR := $(USERLAND_ROOT)/lib/libstlxgfx/include
export INCLUDE_FLAGS := -I$(USERLAND_INCLUDE_DIR) -I$(USERLAND_SYSROOT)/include

# Standard C flags
export CFLAGS_COMMON := $(ARCH_FLAGS) $(DEBUG_FLAGS) $(WARNING_FLAGS) $(INCLUDE_FLAGS) \
                       -std=c11 -fno-stack-protector -nostartfiles

# Standard C++ flags  
export CXXFLAGS_COMMON := $(ARCH_FLAGS) $(DEBUG_FLAGS) $(WARNING_FLAGS) $(INCLUDE_FLAGS) \
                         -std=c++17 -fno-stack-protector -nostartfiles -fno-exceptions -fno-rtti

# =========================
# Exported Linker Flags
# =========================
export LDFLAGS_COMMON := -L$(USERLAND_LIB_DIR) -L$(USERLAND_SYSROOT)/lib
export LIBS_COMMON := -lstlibc -lc
export LIBS_STLXGFX := -lstlxgfx
export LINKER_SCRIPT := $(USERLAND_ROOT)/userland.ld
export LDFLAGS_EXE := -T$(LINKER_SCRIPT) $(LDFLAGS_COMMON)

# =========================
# Exported Build Configuration
# =========================
export USERLAND_CRT_START := $(USERLAND_LIB_DIR)/crt0.o
export USERLAND_CRT_END := $(USERLAND_LIB_DIR)/crtn.o

# =========================
# Local Directories
# =========================
LIBS_DIR := lib
APPS_DIR := apps

# =========================
# Helper Functions
# =========================

# Function to check if toolchain exists (only for build targets)
define check_toolchain
	@if [ ! -f "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-gcc" ]; then \
		echo "Error: Toolchain not found at $(TOOLCHAIN_DIR)"; \
		echo "Please run: cd ../toolchain/scripts && source env.sh && ./build-toolchain.sh"; \
		exit 1; \
	fi
endef

# =========================
# Build Targets
# =========================

# Default target - build everything and install to initrd
all: create_dirs libs apps install_to_initrd

# Create necessary directories
create_dirs:
	$(call check_toolchain)
	@mkdir -p $(USERLAND_BUILD_DIR) $(USERLAND_LIB_DIR) $(USERLAND_BIN_DIR) $(INITRD_BIN_DIR)

# Build libraries first (apps depend on them)
libs: create_dirs
	@echo "Building userland libraries..."
	@$(MAKE) -C $(LIBS_DIR)

# Build applications (depends on libraries)
apps: libs
	@echo "Building userland applications..."
	@$(MAKE) -C $(APPS_DIR)

# Install binaries to initrd
install_to_initrd: apps
	@echo "Installing binaries to initrd..."
	@if [ -n "$$(ls -A $(USERLAND_BIN_DIR)/ 2>/dev/null)" ]; then \
		cp $(USERLAND_BIN_DIR)/* $(INITRD_BIN_DIR)/; \
		echo "Binaries installed to initrd"; \
	else \
		echo "No binaries to install"; \
	fi

# Clean all build artifacts (no toolchain check needed)
clean:
	@echo "Cleaning userland build artifacts..."
	@$(MAKE) -C $(LIBS_DIR) clean || true
	@$(MAKE) -C $(APPS_DIR) clean || true
	@rm -rf $(USERLAND_BUILD_DIR)

# Debug target to show exported variables (requires toolchain)
debug-vars:
	$(call check_toolchain)
	@echo "=== Exported Build Variables ==="
	@echo "CC: $(CC)"
	@echo "CXX: $(CXX)"
	@echo "CFLAGS_COMMON: $(CFLAGS_COMMON)"
	@echo "CXXFLAGS_COMMON: $(CXXFLAGS_COMMON)"
	@echo "LDFLAGS_COMMON: $(LDFLAGS_COMMON)"
	@echo "LIBS_COMMON: $(LIBS_COMMON)"
	@echo "USERLAND_INCLUDE_DIR: $(USERLAND_INCLUDE_DIR)"
	@echo "USERLAND_LIB_DIR: $(USERLAND_LIB_DIR)"
	@echo "USERLAND_BIN_DIR: $(USERLAND_BIN_DIR)"

.PHONY: all clean create_dirs libs apps install_to_initrd debug-vars
