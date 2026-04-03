#
# Stellux Userland - Generic C++ App Build Rules
#
# Include from an app Makefile after setting APP_NAME:
#   APP_NAME := myapp
#   include ../../mk/cxxapp.mk
#
# Handles both .c and .cpp sources. Links against libc++, libc++abi,
# and libunwind in addition to musl libc. Requires 'make libcxx' to
# have been run first.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

# Verify libc++ sysroot exists
ifeq ($(wildcard $(SYSROOT)/lib/libc++.a),)
  $(error libc++ not found in sysroot. Run 'make libcxx' from the top-level directory first)
endif

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

APP_LIBS ?=

C_SOURCES   := $(wildcard $(SRC_DIR)/*.c)
CXX_SOURCES := $(wildcard $(SRC_DIR)/*.cpp)
C_OBJECTS   := $(C_SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CXX_OBJECTS := $(CXX_SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
OBJECTS     := $(C_OBJECTS) $(CXX_OBJECTS)
TARGET      := $(BIN_DIR)/$(APP_NAME)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	$(UQ)$(CXX) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(OBJECTS) \
		-L$(SYSROOT)/lib \
		-Wl,--start-group \
		-lstlx $(addprefix -l,$(APP_LIBS)) -lc++ -lc++abi -lunwind -lc -lm $(BUILTINS_LIB) \
		-Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(CFLAGS_COMMON) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(UQ)mkdir -p $(dir $@)
	@echo "[CXX] $< ($(ARCH))"
	$(UQ)$(CXX) $(CXXFLAGS_COMMON) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
