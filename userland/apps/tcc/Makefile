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

# On-target compile environment, staged as a rootfs overlay that the
# userland install step copies onto the initrd.
ROOTFS_DIR := $(USERLAND_ROOT)/build/$(ARCH)/rootfs
TCC_HOME   := $(ROOTFS_DIR)/usr/lib/tcc
USR_LIB    := $(ROOTFS_DIR)/usr/lib
USR_INC    := $(ROOTFS_DIR)/usr/include
BUILTINS_A := $(SYSROOT)/lib/libclang_rt.builtins-$(ARCH).a

# Per-target libtcc1 members, matching the upstream lib/Makefile object
# lists. Cache flushing for -run comes from the compiler-rt builtins
# merged in below, so armflush.o is not built.
ifeq ($(ARCH),x86_64)
  LIB1_BASES := libtcc1 va_list stdatomic atomic builtin alloca alloca-bt dsohandle
else
  LIB1_BASES := lib-arm64 stdatomic atomic builtin alloca alloca-bt dsohandle
endif
LIB1_OBJS := $(addprefix $(BUILD_DIR)/lib/,$(addsuffix .o,$(LIB1_BASES)))

# Standalone objects tcc loads by name: runmain.o for -run, tcov.o for
# -ftest-coverage.
EXTRA_OBJS := $(BUILD_DIR)/lib/runmain.o $(BUILD_DIR)/lib/tcov.o

# Libraries and start files programs link against on target. The empty
# musl stub archives keep common -l flags working.
SYSROOT_LIBS := libc.a libm.a libpthread.a libdl.a librt.a libcrypt.a \
	libutil.a libxnet.a libresolv.a libstlx.a crt1.o crti.o crtn.o

LIB_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-O2 -g -fno-stack-protector \
	-Wno-attribute-alias

all: $(TARGET) $(BUILD_DIR)/.staged

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

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(LIB_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.S
	$(UQ)mkdir -p $(dir $@)
	@echo "[AS]  $< ($(ARCH))"
	$(UQ)$(CC) $(LIB_CFLAGS) -c $< -o $@

# The compiler-rt builtins are merged in flattened so tcc-linked
# programs resolve helper calls (soft fp128 on arm64, cache flush)
# without knowing about compiler-rt.
$(TCC_HOME)/libtcc1.a: $(LIB1_OBJS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[AR]  libtcc1.a ($(ARCH))"
	$(UQ)rm -f $@
	$(UQ)$(AR) qcsL $@ $(LIB1_OBJS) $(BUILTINS_A)

$(BUILD_DIR)/.staged: $(TCC_HOME)/libtcc1.a $(EXTRA_OBJS)
	@echo "[STG] compile environment ($(ARCH))"
	$(UQ)mkdir -p $(TCC_HOME)/include $(USR_LIB)
	$(UQ)cp $(EXTRA_OBJS) $(TCC_HOME)/
	$(UQ)cp $(SRC_DIR)/include/*.h $(TCC_HOME)/include/
	$(UQ)cd $(SYSROOT)/lib && cp $(SYSROOT_LIBS) $(USR_LIB)/
	$(UQ)if [ -d $(USR_INC) ]; then mv $(USR_INC) $(BUILD_DIR)/include-trash-$$$$; fi
	$(UQ)rm -rf $(BUILD_DIR)/include-trash-* || true
	$(UQ)mkdir -p $(USR_INC)
	$(UQ)cp -R $(SYSROOT)/include/. $(USR_INC)/
	$(UQ)rm -rf $(USR_INC)/c++ $(USR_INC)/linux $(USR_INC)/asm $(USR_INC)/asm-generic
	$(UQ)touch $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

-include $(BUILD_DIR)/tcc.d

.PHONY: all clean
