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
	-DDEBUG

# ============================================================================
# Release Flags
# ============================================================================

CXXFLAGS_RELEASE := \
	-O2 \
	-DNDEBUG

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

# ============================================================================
# Unit Test Configuration
# ============================================================================

# Build with in-kernel unit test framework enabled.
# 0 = disabled (default), 1 = enabled.
UNIT_TEST ?= 0

# Optional test filter:
#   ""                    => run all tests
#   "suite_prefix"        => run suites matching prefix
#   "suite_name.case_name" => run one test case
UNIT_TEST_FILTER ?=

# Stop execution immediately after the first failing test case.
# 0 = continue running all tests (default), 1 = fail-fast.
UNIT_TEST_FAIL_FAST ?= 0

# Repeat each matching test case this many times (>= 1).
UNIT_TEST_REPEAT ?= 1

# Seed for deterministic per-test pseudo-random generation.
UNIT_TEST_SEED ?= 0xC0FFEE

# Host-side unit test timeout (seconds) for QEMU runner.
UNIT_TEST_TIMEOUT ?= 120
