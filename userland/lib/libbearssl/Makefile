#
# Stellux Userland - libbearssl Static Library
#
# Cross-compiles BearSSL (https://www.bearssl.org/) as a static library
# for Stellux userland. BearSSL provides TLS 1.2 with zero dynamic
# allocation and constant-time crypto.
#
# Trust anchors (CA certificates) are compiled into the library so that
# any application linking against libbearssl gets certificate validation.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

AR ?= llvm-ar

BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libbearssl.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include/bearssl

# BearSSL library sources (all .c files under bearssl/src/)
BEARSSL_SRCS := $(shell find bearssl/src -name '*.c' | sort)

# Trust anchor data (generated separately, see gen_trust_anchors.sh)
EXTRA_SRCS := trust_anchors.c

ALL_SRCS := $(BEARSSL_SRCS) $(EXTRA_SRCS)

BEARSSL_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(BEARSSL_SRCS))
EXTRA_OBJS   := $(patsubst %.c,$(BUILD_DIR)/%.o,$(EXTRA_SRCS))
ALL_OBJS     := $(BEARSSL_OBJS) $(EXTRA_OBJS)

# BearSSL is upstream code: relax warnings that we cannot fix without
# modifying the source. The exact set was determined empirically.
BEARSSL_CFLAGS := \
	$(CFLAGS_COMMON) \
	-Ibearssl/inc \
	-Ibearssl/src \
	-Wno-unused-parameter \
	-Wno-sign-compare \
	-Wno-cast-qual

# Trust anchor file uses BearSSL public headers only
EXTRA_CFLAGS := \
	$(CFLAGS_COMMON) \
	-Ibearssl/inc \
	-Wno-unused-parameter

all: $(TARGET) install-headers

$(TARGET): $(ALL_OBJS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libbearssl.a ($(ARCH))"
	$(UQ)$(AR) crs $@ $(ALL_OBJS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp bearssl/inc/*.h $(SYSROOT_INC)/
	$(UQ)cp trust_anchors.h $(SYSROOT_INC)/

$(BUILD_DIR)/bearssl/%.o: bearssl/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(EXTRA_CFLAGS) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -rf $(SYSROOT_INC)

.PHONY: all install-headers clean
