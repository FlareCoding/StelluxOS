#
# Stellux Userland - Generic App Build Rules
#
# Include from an app Makefile after setting APP_NAME:
#   APP_NAME := init
#   include ../../mk/app.mk
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
TARGET  := $(BIN_DIR)/$(APP_NAME)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	$(UQ)$(CC) -nostdlib -fuse-ld=lld --target=$(ARCH)-linux-musl \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(OBJECTS) \
		-L$(SYSROOT)/lib -lc \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(CFLAGS_COMMON) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
