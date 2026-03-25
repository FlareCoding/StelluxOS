#
# Stellux Userland - DOOM (doomgeneric port)
#
# Custom Makefile because doomgeneric has many warnings incompatible
# with the strict -Werror policy used by the standard app.mk.
#

APP_NAME := doom

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

# Doom-specific CFLAGS: relax warnings, add DOOM defines
DOOM_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=c11 -O2 -g \
	-Wall -Wno-error \
	-Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable \
	-Wno-sign-compare -Wno-missing-field-initializers \
	-Wno-implicit-function-declaration -Wno-sometimes-uninitialized \
	-Wno-implicit-int -Wno-format -Wno-missing-braces \
	-Wno-pointer-sign -Wno-parentheses-equality -Wno-shift-negative-value \
	-fno-stack-protector \
	-DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TARGET  := $(BIN_DIR)/$(APP_NAME)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	$(UQ)$(CC) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(OBJECTS) \
		-L$(SYSROOT)/lib -Wl,--start-group -lstlx -lstlxgfx -lc -lm $(BUILTINS_LIB) -Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(DOOM_CFLAGS) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
