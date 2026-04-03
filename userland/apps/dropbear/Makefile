#
# Dropbear SSH Server - Stellux Build
#
# Cross-compiles Dropbear against the Stellux musl sysroot.
# Uses bundled libtomcrypt and libtommath.
#

APP_NAME := dropbear
ARCH ?= x86_64
USERLAND_ROOT ?= ../..

SYSROOT := $(USERLAND_ROOT)/sysroot/$(ARCH)
TARGET_TRIPLE := $(ARCH)-linux-musl
CC := clang
BUILD_DIR := build/$(ARCH)
OUTDIR := $(USERLAND_ROOT)/build/$(ARCH)/bin

CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=c11 -O2 -g \
	-D_GNU_SOURCE \
	-Wall -Wno-unused-parameter -Wno-unused-variable -Wno-sign-compare \
	-Wno-pointer-sign -Wno-implicit-fallthrough \
	-Werror=implicit-function-declaration -Werror=int-conversion \
	-fno-stack-protector \
	-DLOCALOPTIONS_H_EXISTS \
	-DBUNDLED_LIBTOM \
	-I. \
	-Isrc \
	-Ilibtomcrypt/src/headers \
	-Ilibtommath

LDFLAGS := \
	-nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) -static

BUILTINS_LIB := $(shell $(CC) --target=$(TARGET_TRIPLE) -print-libgcc-file-name 2>/dev/null || \
	$(CC) --target=$(TARGET_TRIPLE) --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null)

# Dropbear server sources (no client, no scp, no fuzz)
DROPBEAR_SRCS := \
	src/svr-main.c src/svr-auth.c src/svr-authpasswd.c \
	src/svr-chansession.c src/svr-kex.c src/svr-runopts.c \
	src/svr-service.c src/svr-session.c src/svr-agentfwd.c \
	src/svr-authpubkey.c src/svr-authpubkeyoptions.c \
	src/svr-tcpfwd.c src/svr-x11fwd.c \
	src/common-session.c src/common-algo.c src/common-channel.c \
	src/common-chansession.c src/common-kex.c src/common-runopts.c \
	src/packet.c src/process-packet.c \
	src/dbrandom.c src/dbutil.c src/dbhelpers.c src/dbmalloc.c \
	src/buffer.c src/bignum.c src/signkey.c src/signkey_ossh.c \
	src/rsa.c src/genrsa.c \
	src/ed25519.c src/gened25519.c \
	src/gensignkey.c src/dh_groups.c \
	src/crypto_desc.c src/curve25519.c \
	src/ltc_prng.c \
	src/chachapoly.c src/gcm.c \
	src/circbuffer.c src/list.c src/queue.c \
	src/listener.c src/tcp-accept.c \
	src/netio.c src/atomicio.c \
	src/loginrec.c src/compat.c src/fake-rfc2553.c \
	src/sshpty.c src/termcodes.c src/progressmeter.c \
	src/dss.c src/gendss.c \
	src/ecc.c src/ecdsa.c \
	src/sk-ecdsa.c src/sk-ed25519.c \
	src/keyimport.c \
	stellux_stubs.c

# libtomcrypt - find all .c files, excluding tab files that are #included
LTC_SRCS := $(shell find libtomcrypt/src -name '*.c' \
	-not -name '*tab.c' \
	-not -name 'aes_enc.c' \
	| sort)

# libtommath - find all .c files
LTM_SRCS := $(shell find libtommath -name '*.c' -not -path '*/mtest/*' -not -path '*/demo/*' | sort)

DROPBEAR_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DROPBEAR_SRCS))
LTC_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LTC_SRCS))
LTM_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(LTM_SRCS))
ALL_OBJS := $(DROPBEAR_OBJS) $(LTC_OBJS) $(LTM_OBJS)

TARGET := $(OUTDIR)/$(APP_NAME)

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	@echo "[LD]  $(APP_NAME) ($(ARCH))"
	@$(CC) $(LDFLAGS) -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(ALL_OBJS) \
		-L$(SYSROOT)/lib -Wl,--start-group -lstlx -lc -lm $(BUILTINS_LIB) -Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

LTC_CFLAGS := $(CFLAGS) -DLTC_SOURCE -DLTM_DESC
LTM_CFLAGS := $(CFLAGS)

$(BUILD_DIR)/libtomcrypt/%.o: libtomcrypt/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	@$(CC) $(LTC_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/libtommath/%.o: libtommath/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	@$(CC) $(LTM_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@rm -rf build
	@echo "[CLEAN] $(APP_NAME)"

.PHONY: all clean
