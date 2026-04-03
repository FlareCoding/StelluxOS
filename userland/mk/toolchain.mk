#
# Stellux Userland - Toolchain Configuration
#
# Defines compiler, flags, and paths parameterized by ARCH.
# Included by app.mk and other build rules.
#

CC := clang

SYSROOT := $(USERLAND_ROOT)/sysroot/$(ARCH)
TARGET_TRIPLE := $(ARCH)-linux-musl

ifeq ($(ARCH),x86_64)
  GCC_TRIPLE := x86_64-linux-gnu
else ifeq ($(ARCH),aarch64)
  GCC_TRIPLE := aarch64-linux-gnu
else
  $(error Unsupported ARCH=$(ARCH))
endif

# Keep userland header resolution hermetic to the musl sysroot so host/cross
# distro headers (e.g., /usr/aarch64-linux-gnu/include) cannot leak in.
CFLAGS_COMMON := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=c11 -O2 -g \
	-Wall -Wextra -Werror \
	-fno-stack-protector

# Runtime builtins are required for compiler helper symbols referenced by musl
# (notably on aarch64 long-double printf paths). Prefer compiler-rt when
# available; otherwise fall back to the target GCC libgcc archive.
COMPILER_RT_BUILTINS := $(shell $(CC) --target=$(TARGET_TRIPLE) --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null)
GCC_BUILTINS := $(shell $(GCC_TRIPLE)-gcc -print-libgcc-file-name 2>/dev/null)

ifneq ($(wildcard $(COMPILER_RT_BUILTINS)),)
  BUILTINS_LIB := $(COMPILER_RT_BUILTINS)
else ifneq ($(wildcard $(GCC_BUILTINS)),)
  BUILTINS_LIB := $(GCC_BUILTINS)
else
  $(error Missing runtime builtins for ARCH=$(ARCH). Install dependencies with 'make deps')
endif

# Verbosity (inherited from top-level V=1)
ifeq ($(V),1)
  UQ :=
else
  UQ := @
endif
