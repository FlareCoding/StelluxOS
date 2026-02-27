#
# Stellux Kernel Makefile
#
# Auto-discovers all *.cpp and *.S files recursively and builds the kernel.
# Requires ARCH to be set. Include config.mk before calling.
#
# Usage:
#   make -C kernel ARCH=x86_64 BUILD_DIR=../build
#

# ============================================================================
# Configuration Check
# ============================================================================

# Allow 'clean' target without ARCH
ifneq ($(MAKECMDGOALS),clean)
ifndef ARCH
$(error ARCH is required. Use: make ARCH=x86_64 or make ARCH=aarch64)
endif

# Validate architecture
ifeq ($(filter $(ARCH),x86_64 aarch64),)
$(error Invalid ARCH=$(ARCH). Supported: x86_64, aarch64)
endif
endif

# ============================================================================
# Paths
# ============================================================================

# Build directory (passed from top-level, default for standalone use)
BUILD_DIR ?= ../build
KERNEL_BUILD_DIR := $(BUILD_DIR)/kernel/$(ARCH)
OBJ_DIR := $(KERNEL_BUILD_DIR)/obj

# Output
KERNEL_ELF := $(KERNEL_BUILD_DIR)/kernel.elf

# Linker script
LINKER_SCRIPT := arch/$(ARCH)/linker.ld

# ============================================================================
# Include Configuration and Pretty Output
# ============================================================================

# Pretty output helpers
include ../scripts/pretty.mk

# Common configuration (toolchain, build mode, limits)
include ../config.mk

# Architecture-specific flags
include arch/$(ARCH)/defconfig

# ============================================================================
# Source Discovery
# ============================================================================

# Find all C++ sources
CXX_SOURCES := $(shell find boot -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find common -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find io -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find mm -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find percpu -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find sched -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find syscall -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find acpi -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find debug -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find exec -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find sync -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find smp -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find rc -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find fs -name '*.cpp' 2>/dev/null | sort)
CXX_SOURCES += $(shell find arch/$(ARCH) -name '*.cpp' 2>/dev/null | sort)

# Unit test sources (only when STLX_UNIT_TESTS_ENABLED=1)
ifeq ($(STLX_UNIT_TESTS_ENABLED),1)
CXX_SOURCES += $(shell find tests -name '*.cpp' 2>/dev/null | sort)
endif

# Find all assembly sources (arch-specific only typically)
ASM_SOURCES := $(shell find arch/$(ARCH) -name '*.S' 2>/dev/null | sort)

# Unit test assembly sources (e.g., FPU test helpers)
ifeq ($(STLX_UNIT_TESTS_ENABLED),1)
ASM_SOURCES += $(shell find tests -name '*.S' 2>/dev/null | sort)
endif

# Generate object file paths
CXX_OBJECTS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CXX_SOURCES))
ASM_OBJECTS := $(patsubst %.S,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
OBJECTS := $(CXX_OBJECTS) $(ASM_OBJECTS)

# Total file count for progress
TOTAL_FILES := $(words $(OBJECTS))
CURRENT_FILE := 0

# ============================================================================
# Compiler Flags
# ============================================================================

# Base C++ flags
CXXFLAGS_STD ?= -std=c++20

CXXFLAGS_FREESTANDING ?= \
	-ffreestanding \
	-fno-builtin \
	-nostdinc++ \
	-nostdlib

CXXFLAGS_KERNEL ?= \
	-fno-exceptions \
	-fno-rtti \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-PIE \
	-fno-PIC

CXXFLAGS_WARN ?= \
	-Wall \
	-Wextra


# Log level config flag
CXXFLAGS_LOG := -DLOG_LEVEL=$(LOG_LEVEL)

# Configuration defines (from config.mk)
CXXFLAGS_CONFIG := -DMAX_CPUS=$(MAX_CPUS)

# Platform define (e.g., PLATFORM=rpi4 → -DSTLX_PLATFORM_RPI4)
ifneq ($(PLATFORM),qemu-virt)
PLATFORM_UPPER := $(shell echo '$(PLATFORM)' | tr 'a-z-' 'A-Z_')
CXXFLAGS_CONFIG += -DSTLX_PLATFORM_$(PLATFORM_UPPER)
endif

# Include paths
# Note: -Iarch/$(ARCH) allows arch-specific headers to be included with paths
# like #include "hw/barrier.h" - the build system picks the right arch directory
CXXFLAGS_INCLUDES := -I. -Icommon -Iarch/$(ARCH)

# Unit test flags
ifeq ($(STLX_UNIT_TESTS_ENABLED),1)
CXXFLAGS_EXTRA += -DSTLX_UNIT_TESTS_ENABLED
CXXFLAGS_INCLUDES += -Itests/framework
endif

# Combine all C++ flags
CXXFLAGS := \
	$(CXXFLAGS_STD) \
	$(CXXFLAGS_FREESTANDING) \
	$(CXXFLAGS_KERNEL) \
	$(CXXFLAGS_ARCH) \
	$(CXXFLAGS_WARN) \
	$(CXXFLAGS_MODE) \
	$(CXXFLAGS_LOG) \
	$(CXXFLAGS_CONFIG) \
	$(CXXFLAGS_INCLUDES) \
	$(CXXFLAGS_EXTRA)

# Assembly flags
ASFLAGS := \
	-ffreestanding \
	$(ASFLAGS_ARCH) \
	$(CXXFLAGS_INCLUDES)

# Linker flags
LDFLAGS := \
	-nostdlib \
	-static \
	-T $(LINKER_SCRIPT)

# For header display (truncated)
CXXFLAGS_DISPLAY := $(CXXFLAGS_STD) $(CXXFLAGS_ARCH) $(CXXFLAGS_MODE)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean header

all: header $(KERNEL_ELF)
	@printf "\n  $(COLOR_BOLD)$(COLOR_GREEN)Kernel built:$(COLOR_RESET) $(KERNEL_ELF)\n\n"

# Print build configuration header
header:
ifeq ($(V),1)
	@echo "=== Verbose Build ==="
	@echo "ARCH=$(ARCH)"
	@echo "CXXFLAGS=$(CXXFLAGS)"
	@echo "ASFLAGS=$(ASFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"
	@echo "SOURCES=$(CXX_SOURCES) $(ASM_SOURCES)"
	@echo ""
else
	$(call print_header)
endif

# Link kernel ELF
$(KERNEL_ELF): $(OBJECTS) $(LINKER_SCRIPT)
	$(call ensure_dir,$@)
	$(call link_elf,$@,$(OBJECTS),$(TOTAL_FILES)/$(TOTAL_FILES))

# ============================================================================
# Pattern Rules with Progress Counter
# ============================================================================

# We use a marker file approach to track progress across separate make invocations
# Each compile rule increments the counter

# Compile C++ files
$(OBJ_DIR)/%.o: %.cpp
	$(call ensure_dir,$@)
	$(eval CURRENT_FILE := $(shell echo $$(($(CURRENT_FILE) + 1))))
	$(call compile_cxx,$<,$@,$(CURRENT_FILE)/$(TOTAL_FILES))

# Compile assembly files
$(OBJ_DIR)/%.o: %.S
	$(call ensure_dir,$@)
	$(eval CURRENT_FILE := $(shell echo $$(($(CURRENT_FILE) + 1))))
	$(call compile_asm,$<,$@,$(CURRENT_FILE)/$(TOTAL_FILES))

# ============================================================================
# Clean
# ============================================================================

clean:
	@printf "  [CLEAN]  $(BUILD_DIR)/kernel\n"
	$(Q)rm -rf $(BUILD_DIR)/kernel

# ============================================================================
# Debug helpers
# ============================================================================

.PHONY: show-sources show-objects

show-sources:
	@echo "C++ Sources:"
	@for f in $(CXX_SOURCES); do echo "  $$f"; done
	@echo "Assembly Sources:"
	@for f in $(ASM_SOURCES); do echo "  $$f"; done

show-objects:
	@echo "Objects:"
	@for f in $(OBJECTS); do echo "  $$f"; done

# ============================================================================
# Compile Commands (for clangd/IDE support)
# ============================================================================

.PHONY: compile-commands

compile-commands:
	@echo "Generating compile_commands.json (includes test sources)..."
	$(eval CC_SOURCES := $(CXX_SOURCES))
	$(eval CC_FLAGS := $(CXXFLAGS))
ifeq ($(STLX_UNIT_TESTS_ENABLED),)
	$(eval CC_SOURCES += $(shell find tests -name '*.cpp' 2>/dev/null | sort))
	$(eval CC_FLAGS += -DSTLX_UNIT_TESTS_ENABLED -Itests/framework)
endif
	@echo "[" > compile_commands.json
	@first=1; \
	for src in $(CC_SOURCES); do \
		if [ $$first -eq 1 ]; then first=0; else echo "," >> compile_commands.json; fi; \
		args="\"$(CXX)\""; \
		for flag in $(CC_FLAGS); do args="$$args, \"$$flag\""; done; \
		args="$$args, \"-c\", \"$$src\""; \
		echo "  {" >> compile_commands.json; \
		echo "    \"directory\": \"$$(pwd)\"," >> compile_commands.json; \
		echo "    \"file\": \"$$src\"," >> compile_commands.json; \
		echo "    \"arguments\": [$$args]" >> compile_commands.json; \
		echo "  }" >> compile_commands.json; \
	done
	@echo "]" >> compile_commands.json
	@echo "Created compile_commands.json"
