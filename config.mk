#
# Stellux Kernel Configuration
#
# Common configuration shared across all architectures.
# Architecture-specific flags are in kernel/arch/<arch>/defconfig.
#
# Override at build time: make kernel ARCH=x86_64 RELEASE=1 LOG_LEVEL=2
#

# ============================================================================
# Build Mode
# ============================================================================

# Set to 1 for release build, 0 for debug (default)
# Can be overridden from command line: make RELEASE=1
RELEASE ?= 0

# ============================================================================
# Toolchain
# ============================================================================

CXX := clang++
AS := clang
LD := ld.lld
OBJCOPY := llvm-objcopy

# ============================================================================
# Debug Flags
# ============================================================================

CXXFLAGS_DEBUG := \
	-O0 \
	-g \
	-DDEBUG \
	-fno-omit-frame-pointer

# ============================================================================
# Release Flags
# ============================================================================

CXXFLAGS_RELEASE := \
	-O2 \
	-DNDEBUG \
	-fno-omit-frame-pointer

# ============================================================================
# Build Mode Selection
# ============================================================================

# Select debug/release mode (0=debug, 1=release)
ifeq ($(RELEASE),1)
CXXFLAGS_MODE := $(CXXFLAGS_RELEASE)
MODE_NAME := release
else
CXXFLAGS_MODE := $(CXXFLAGS_DEBUG)
MODE_NAME := debug
endif

# ============================================================================
# Kernel Configuration
# ============================================================================

# Maximum number of CPUs supported
MAX_CPUS ?= 64

# Log level (0=debug, 1=info, 2=warn, 3=error, 4=fatal, 5=none)
LOG_LEVEL ?= 0

# Build epoch (Unix timestamp for RTC fallback on platforms without hardware RTC)
STLX_BUILD_EPOCH ?= $(shell date +%s)

# ============================================================================
# Platform Selection
# ============================================================================

# Target platform (affects hardware addresses, clock rates, etc.)
# Override at build time: make ARCH=aarch64 PLATFORM=rpi4
#
# Supported platforms:
#   qemu-virt   - QEMU virt machine (default for aarch64)
#   rpi4        - Raspberry Pi 4 (BCM2711)
PLATFORM ?= qemu-virt
