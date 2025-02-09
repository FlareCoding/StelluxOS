# =========================
# Userland Settings
# =========================

# Compiler settings
CXX       := g++
LD        := ld

# Paths
ROOT_DIR         := $(shell realpath ..)
KERNEL_ELF       := $(ROOT_DIR)/build/stellux
KERNEL_INCLUDES  := -I$(ROOT_DIR)/kernel/include -I$(ROOT_DIR)/kernel/include/core
INITRD_BIN_DIR   := $(ROOT_DIR)/initrd/bin
USERLAND_DIR     := $(shell realpath .)
APPS_DIR       	 := $(USERLAND_DIR)/apps
USERLAND_LINKER_SCRIPT := $(USERLAND_DIR)/userland.ld

# Architecture
TARGET_ARCH := X86_64

# Preprocessor Defines
PREPROCESSOR_DEFINES := -DARCH_$(TARGET_ARCH)

# Common flags for all userland applications
COMMON_CXXFLAGS := -m64 -g -O0 $(PREPROCESSOR_DEFINES) \
				   -ffreestanding -Wall -Werror -nostdlib -fno-rtti -fno-exceptions \
				   -std=c++17 -mno-red-zone $(KERNEL_INCLUDES) -fno-pie -fno-pic -mcmodel=large

COMMON_LDFLAGS  := -T $(USERLAND_LINKER_SCRIPT) -nostdlib -z max-page-size=0x1000 -no-pie \
				   --just-symbols=$(KERNEL_ELF) -L$(USERLAND_DIR)/lib/crt -lstellux_crt

export CXXFLAGS := $(COMMON_CXXFLAGS)
export LDFLAGS := $(COMMON_LDFLAGS)

# =========================
# Targets
# =========================

.PHONY: all clean install install_to_initrd check-kernel build-libs build-apps

# Build all applications
all: check-kernel build-libs build-apps

# Check if the Kernel ELF file exists
check-kernel:
	@if [ ! -f "$(KERNEL_ELF)" ]; then \
		echo "[ERROR] Kernel ELF file '$(KERNEL_ELF)' not found. Please build the kernel first."; \
		exit 1; \
	fi

build-libs:
	@echo "Building userland libraries..."
	@$(MAKE) -C $(USERLAND_DIR)/lib CXXFLAGS="$(COMMON_CXXFLAGS)" LDFLAGS="$(COMMON_LDFLAGS)"

build-apps:
	@echo "Building all userland applications..."
	@for app in $(APPS_DIR)/*; do \
		if [ -f $$app/Makefile ]; then \
			OBJ_DIR=$$app/obj; \
			BIN_DIR=$$app/bin; \
			$(MAKE) -C $$app \
				CXX="$(CXX)" \
				LD="$(LD)" \
				OBJ_DIR=$$OBJ_DIR \
				BIN_DIR=$$BIN_DIR || exit 1; \
		fi \
	done
	@echo "All applications built successfully."

# Install applications to the initrd/bin directory
install_to_initrd: check-kernel build-apps
	@echo "Copying applications to $(INITRD_BIN_DIR)..."
	@mkdir -p $(INITRD_BIN_DIR)
	@for app in $(APPS_DIR)/*; do \
		bin_file=$$app/bin/$$(basename $$app); \
		cp $$bin_file $(INITRD_BIN_DIR)/ || exit 1; \
	done
	@echo "Applications installed to $(INITRD_BIN_DIR)."

# Clean all application builds
clean:
	@echo "Cleaning userland libraries..."
	@$(MAKE) -C $(USERLAND_DIR)/lib clean
	@echo "Cleaning all userland applications..."
	@for app in $(APPS_DIR)/*; do \
		if [ -f $$app/Makefile ]; then \
			$(MAKE) -C $$app clean || exit 1; \
		fi \
	done
	@echo "Userland cleaned."
