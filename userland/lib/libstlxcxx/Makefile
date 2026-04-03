#
# Stellux Userland - libstlxcxx Static Library
#
# C++ threading wrappers (stlxstd::thread, mutex, etc.) built on libstlx.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

AR ?= llvm-ar

SRC_DIR     := src
INCLUDE_DIR := include
BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libstlxcxx.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include/stlxstd

SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

all: $(TARGET) install-headers

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libstlxcxx.a ($(ARCH))"
	$(UQ)$(AR) crs $@ $(OBJECTS)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp $(INCLUDE_DIR)/stlxstd/*.h $(SYSROOT_INC)/

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) -I$(INCLUDE_DIR) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -rf $(SYSROOT_INC)

.PHONY: all install-headers clean
