#
# Stellux Vim Build
#
# Cross-compiles Vim with --features=tiny for Stellux OS.
# Uses musl libc, no ncurses (builtin termcap), no GUI.
#

USERLAND_ROOT ?= $(shell cd ../.. && pwd)
include ../../mk/toolchain.mk

SRC_DIR   := src
BUILD_DIR := build/$(ARCH)
BIN_DIR   := $(USERLAND_ROOT)/build/$(ARCH)/bin

# Vim-specific CFLAGS
VIM_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=gnu11 -O2 -g \
	-Wall -Wno-unused-parameter -Wno-unused-variable \
	-Wno-sign-compare -Wno-missing-field-initializers \
	-Wno-implicit-fallthrough -Wno-string-plus-int \
	-Wno-unused-function -Wno-parentheses \
	-Wno-missing-braces -Wno-tautological-compare \
	-Wno-pointer-sign -Wno-empty-body \
	-fno-stack-protector \
	-I$(SRC_DIR) -I$(SRC_DIR)/proto \
	-DHAVE_CONFIG_H \
	-DFEAT_TINY

# All Vim source files (from BASIC_SRC + required extras)
VIM_SOURCES := \
	alloc.c arabic.c arglist.c autocmd.c beval.c blob.c blowfish.c \
	buffer.c bufwrite.c change.c charset.c cindent.c clientserver.c \
	clipboard.c cmdexpand.c cmdhist.c crypt.c crypt_zip.c debugger.c \
	dict.c diff.c digraph.c drawline.c drawscreen.c edit.c eval.c \
	evalbuffer.c evalfunc.c evalvars.c evalwindow.c ex_cmds.c \
	ex_cmds2.c ex_docmd.c ex_eval.c ex_getln.c fileio.c filepath.c \
	findfile.c float.c fold.c getchar.c hardcopy.c hashtab.c help.c \
	highlight.c if_cscope.c indent.c insexpand.c json.c list.c \
	locale.c logfile.c main.c map.c mark.c match.c mbyte.c memfile.c \
	memline.c menu.c message.c misc1.c misc2.c mouse.c move.c \
	normal.c ops.c option.c optionstr.c os_unix.c popupmenu.c \
	popupwin.c profiler.c quickfix.c regexp.c register.c screen.c \
	scriptfile.c search.c session.c sha256.c sign.c sound.c spell.c \
	spellfile.c spellsuggest.c strings.c syntax.c tag.c term.c \
	testing.c textformat.c textobject.c textprop.c time.c typval.c \
	ui.c undo.c usercmd.c userfunc.c version.c viminfo.c window.c \
	auto/pathdef.c \
	stellux_stubs.c

# Note: gui_xim.c, if_xcmdsrv.c, pty.c, terminal.c, vim9*.c excluded
# (not needed for tiny build on Stellux)

VIM_OBJECTS := $(VIM_SOURCES:%.c=$(BUILD_DIR)/%.o)

TARGET := $(BIN_DIR)/vim

all: $(TARGET)

$(TARGET): $(VIM_OBJECTS)
	$(UQ)mkdir -p $(dir $@)
	@echo "[LD]  vim ($(ARCH))"
	$(UQ)$(CC) -nostdlib -fuse-ld=lld --target=$(TARGET_TRIPLE) \
		-static -o $@ \
		$(SYSROOT)/lib/crt1.o $(SYSROOT)/lib/crti.o \
		$(VIM_OBJECTS) \
		-L$(SYSROOT)/lib -Wl,--start-group -lstlx -lc -lm $(BUILTINS_LIB) -Wl,--end-group \
		$(SYSROOT)/lib/crtn.o

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(VIM_CFLAGS) -c $< -o $@

$(BUILD_DIR)/auto/%.o: $(SRC_DIR)/auto/%.c
	$(UQ)mkdir -p $(dir $@)
	@echo "[CC]  $< ($(ARCH))"
	$(UQ)$(CC) $(VIM_CFLAGS) -c $< -o $@

clean:
	$(UQ)rm -rf build
	$(UQ)rm -f $(TARGET)

.PHONY: all clean
