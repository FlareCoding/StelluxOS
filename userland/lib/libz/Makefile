#
# Stellux Userland libz Static Library
#
# Cross-compiles all of zlib (https://zlib.net/), the libz.a and zlib.h that programs and configure scripts expect.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libz.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include

SOURCES := $(sort $(wildcard zlib/*.c))
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

# The defines zlib's configure would find on a POSIX system
ZLIB_CFLAGS := \
	$(CFLAGS_COMMON) \
	-DHAVE_UNISTD_H \
	-DHAVE_STDARG_H

all: $(TARGET) install-headers

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libz.a ($(ARCH))"
	$(UQ)rm -f $@
	$(UQ)$(AR) crs $@ $(OBJECTS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp -p zlib/zlib.h zlib/zconf.h $(SYSROOT_INC)/

$(BUILD_DIR)/%.o: %.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(ZLIB_CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -f $(SYSROOT_INC)/zlib.h $(SYSROOT_INC)/zconf.h

.PHONY: all install-headers clean
