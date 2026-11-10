#
# Stellux Userland libstlxtheme Headers
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

INCLUDE_DIR := include
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include/stlxtheme

all: install-headers

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp -p $(INCLUDE_DIR)/stlxtheme/*.h $(SYSROOT_INC)/

clean:
	$(UQ)rm -rf $(SYSROOT_INC)

.PHONY: all install-headers clean
