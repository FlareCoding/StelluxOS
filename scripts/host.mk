#
# host.mk - Host OS detection and toolchain resolution
#
# Central host interface: consumer makefiles use the
# STLX_* variables and never branch on the host OS.
#

ifndef STLX_HOST_MK_INCLUDED
STLX_HOST_MK_INCLUDED := 1

HOST_OS := $(shell uname -s)

ifeq ($(HOST_OS),Darwin)

# Homebrew LLVM keg (Apple Silicon, then Intel prefix). Overridable.
STLX_LLVM_DIR ?= $(firstword $(wildcard /opt/homebrew/opt/llvm /usr/local/opt/llvm))

# $(1) = tool inside the llvm keg, $(2) = PATH fallback (also used when no keg)
stlx_llvm_tool = $(if $(and $(STLX_LLVM_DIR),$(wildcard $(STLX_LLVM_DIR)/bin/$(1))),$(STLX_LLVM_DIR)/bin/$(1),$(2))

STLX_CC      := $(call stlx_llvm_tool,clang,clang)
STLX_CXX     := $(call stlx_llvm_tool,clang++,clang++)
STLX_LLD     := $(call stlx_llvm_tool,ld.lld,ld.lld)
STLX_AR      := $(call stlx_llvm_tool,llvm-ar,llvm-ar)
STLX_RANLIB  := $(call stlx_llvm_tool,llvm-ranlib,llvm-ranlib)
STLX_OBJCOPY := $(call stlx_llvm_tool,llvm-objcopy,llvm-objcopy)

else

STLX_CC      := clang
STLX_CXX     := clang++
STLX_LLD     := ld.lld
STLX_AR      := llvm-ar
STLX_RANLIB  := llvm-ranlib
STLX_OBJCOPY := llvm-objcopy

endif

# Fail at parse time if the interface is incomplete for this host.
$(foreach v,HOST_OS STLX_CC STLX_CXX STLX_LLD STLX_AR STLX_RANLIB STLX_OBJCOPY,\
	$(if $($(v)),,$(error scripts/host.mk: $(v) is undefined for host '$(HOST_OS)')))

endif # STLX_HOST_MK_INCLUDED
