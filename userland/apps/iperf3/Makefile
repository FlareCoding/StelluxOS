#
# Stellux Userland - iperf 3.15 (esnet/iperf, BSD-3-Clause)
#
# src/ is the upstream source tree without its tests, plus the two files
# configure generates: iperf_config.h, cross-configured against the musl
# sysroot and then trimmed of the features Stellux has no syscall for, and
# version.h. Upstream warns under the strict -Werror policy of app.mk, so
# the compile line is spelled out here.
#

APP_NAME := iperf3

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

# -Ulinux keeps iperf3 off getsockopt(TCP_INFO), which returns the Stellux
# record here rather than the struct upstream expects.
IPERF_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=gnu11 -O2 -g \
	-Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-sign-compare -Wno-implicit-const-int-float-conversion \
	-Werror=implicit-function-declaration -Werror=int-conversion \
	-fno-stack-protector \
	-D_GNU_SOURCE -DHAVE_CONFIG_H -Ulinux \
	-I$(SRC_DIR) \
	-MD -MP

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
		-L$(SYSROOT)/lib -Wl,--start-group -lc -lm $(BUILTINS_LIB) -Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(IPERF_CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
