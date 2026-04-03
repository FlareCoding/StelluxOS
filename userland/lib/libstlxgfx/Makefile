#
# Stellux Userland - libstlxgfx Static Library
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

AR ?= llvm-ar

SRC_DIR     := src
INCLUDE_DIR := include
BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libstlxgfx.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include/stlxgfx

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

all: $(TARGET) install-headers

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libstlxgfx.a ($(ARCH))"
	$(UQ)$(AR) crs $@ $(OBJECTS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp $(INCLUDE_DIR)/stlxgfx/*.h $(SYSROOT_INC)/

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(CFLAGS_COMMON) -I$(INCLUDE_DIR) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -rf $(SYSROOT_INC)

.PHONY: all install-headers clean
