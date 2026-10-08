#
# Stellux Userland - libsqlite3 Static Library
#
# Cross-compiles the SQLite amalgamation (https://sqlite.org/amalgamation.html)
# as a static library for Stellux userland.
#

USERLAND_ROOT ?= $(shell cd $(dir $(lastword $(MAKEFILE_LIST)))/../.. && pwd)
include $(USERLAND_ROOT)/mk/toolchain.mk

BUILD_DIR   := build/$(ARCH)
LIB_NAME    := libsqlite3.a
TARGET      := $(BUILD_DIR)/$(LIB_NAME)
OBJECT      := $(BUILD_DIR)/sqlite/sqlite3.o

SYSROOT_LIB := $(USERLAND_ROOT)/sysroot/$(ARCH)/lib
SYSROOT_INC := $(USERLAND_ROOT)/sysroot/$(ARCH)/include

# The options sqlite.org recommends for speed. Without mutexes only one
# thread of a program may use SQLite.
SQLITE_OPTIONS := \
	-DSQLITE_DQS=0 \
	-DSQLITE_THREADSAFE=0 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 \
	-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
	-DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
	-DSQLITE_MAX_EXPR_DEPTH=0 \
	-DSQLITE_OMIT_DECLTYPE \
	-DSQLITE_OMIT_DEPRECATED \
	-DSQLITE_OMIT_PROGRESS_CALLBACK \
	-DSQLITE_OMIT_SHARED_CACHE \
	-DSQLITE_USE_ALLOCA \
	-DSQLITE_STRICT_SUBTYPE=1

# Userland programs link statically, so SQLite cannot load extensions
SQLITE_OPTIONS += -DSQLITE_OMIT_LOAD_EXTENSION

# Without mutexes the unix layer assigns its global lock and never reads it. Older
# clang lacks that warning, so an unknown warning flag must not fail the build.
SQLITE_CFLAGS := \
	$(CFLAGS_COMMON) \
	$(SQLITE_OPTIONS) \
	-Wno-unknown-warning-option \
	-Wno-unused-but-set-global

all: $(TARGET) install-headers

$(TARGET): $(OBJECT)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libsqlite3.a ($(ARCH))"
	$(UQ)rm -f $@
	$(UQ)$(AR) crs $@ $(OBJECT)
	$(UQ)mkdir -p $(SYSROOT_LIB)
	$(UQ)cp $@ $(SYSROOT_LIB)/

install-headers:
	$(UQ)mkdir -p $(SYSROOT_INC)
	$(UQ)cp -p sqlite/sqlite3.h $(SYSROOT_INC)/

$(OBJECT): sqlite/sqlite3.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(SQLITE_CFLAGS) -c $< -o $@

-include $(OBJECT:.o=.d)

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(SYSROOT_LIB)/$(LIB_NAME)
	$(UQ)rm -f $(SYSROOT_INC)/sqlite3.h

.PHONY: all install-headers clean
