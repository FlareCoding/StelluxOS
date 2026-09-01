#
# Stellux TCC Build
#
# Cross-compiles the Tiny C Compiler for on-target use. Sources are a
# pinned vendored subset of upstream tinycc (mob branch, 2ba12e83).
#
# The compiler builds as one translation unit: with ONE_SOURCE, tcc.c
# pulls in the preprocessor, code generator, assembler, ELF linker,
# and the backend selected by TCC_TARGET. tccdefs_.h is generated at
# build time from include/tccdefs.h by the c2str host tool.
#

USERLAND_ROOT ?= $(shell cd ../.. && pwd)
include ../../mk/toolchain.mk

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

ifeq ($(ARCH),x86_64)
  TCC_TARGET := TCC_TARGET_X86_64
else
  TCC_TARGET := TCC_TARGET_ARM64
endif

TCC_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=gnu11 -O2 -g \
	-Wall -Wno-unused-function \
	-fno-stack-protector \
	-MMD -MP \
	-I. -I$(BUILD_DIR) -I$(SRC_DIR) \
	-DONE_SOURCE=1 -D$(TCC_TARGET)

TARGET := $(BIN_DIR)/tcc
C2STR  := build/c2str

all: $(TARGET)

# Host tool that converts include/tccdefs.h into a C string header
$(C2STR): $(SRC_DIR)/conftest.c
	$(UQ)mkdir -p build
	@echo "[HOST] c2str"
	$(UQ)$(CC) -DC2STR $< -o $@

$(BUILD_DIR)/tccdefs_.h: $(SRC_DIR)/include/tccdefs.h $(C2STR)
	$(UQ)mkdir -p $(BUILD_DIR)
	@echo "[GEN] tccdefs_.h ($(ARCH))"
	$(UQ)$(C2STR) $< $@

$(BUILD_DIR)/tcc.o: $(SRC_DIR)/tcc.c $(BUILD_DIR)/tccdefs_.h config.h
	$(UQ)mkdir -p $(BUILD_DIR)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(TCC_CFLAGS) -c $< -o $@

$(TARGET): $(BUILD_DIR)/tcc.o
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  tcc ($(ARCH))"
	$(UQ)$(CC) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(BUILD_DIR)/tcc.o \
		-L$(SYSROOT)/lib -Wl,--start-group -lstlx -lc -lm $(BUILTINS_LIB) -Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

-include $(BUILD_DIR)/tcc.d

.PHONY: all clean
