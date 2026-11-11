#
# Stellux Userland libzstd Static Library
#
# Cross-compiles Zstandard's decompressor (https://facebook.github.io/zstd/), upstream's lib/common and lib/decompress.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libzstd.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include

SOURCES := $(sort $(wildcard zstd/lib/common/*.c zstd/lib/decompress/*.c))
OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SOURCES))

# Both architectures run the same C, without assembly, the legacy formats or trace hooks
ZSTD_CFLAGS := \
	$(CFLAGS_COMMON) \
	-DZSTD_DISABLE_ASM \
	-DZSTD_LEGACY_SUPPORT=0 \
	-DZSTD_TRACE=0

all: $(TARGET) install-headers

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libzstd.a ($(ARCH))"
	$(UQ)rm -f $@
	$(UQ)$(AR) crs $@ $(OBJECTS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp -p zstd/lib/zstd.h zstd/lib/zstd_errors.h $(SYSROOT_INC)/

$(BUILD_DIR)/%.o: %.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(ZSTD_CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -f $(SYSROOT_INC)/zstd.h $(SYSROOT_INC)/zstd_errors.h

.PHONY: all install-headers clean
