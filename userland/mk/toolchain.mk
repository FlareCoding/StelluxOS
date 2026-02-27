#
# Stellux Userland - Toolchain Configuration
#
# Defines compiler, flags, and paths parameterized by ARCH.
# Included by app.mk and other build rules.
#

CC := clang

SYSROOT := $(USERLAND_ROOT)/sysroot/$(ARCH)

CFLAGS_COMMON := \
	--target=$(ARCH)-linux-musl \
	--sysroot=$(SYSROOT) \
	-std=c11 -O2 -g \
	-Wall -Wextra -Werror \
	-fno-stack-protector

# Verbosity (inherited from top-level V=1)
ifeq ($(V),1)
  UQ :=
else
  UQ := @
endif
