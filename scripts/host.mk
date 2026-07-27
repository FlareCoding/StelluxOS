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

define HOST_USB_INSTRUCTIONS
	@echo "  1. Identify your USB device (use 'diskutil list' to find it)"
	@echo "     Example: /dev/disk4 (NOT a partition like /dev/disk4s1)"
	@echo ""
	@echo "  2. Unmount the disk:"
	@echo "     diskutil unmountDisk /dev/diskN"
	@echo ""
	@echo "  3. Write the image (DESTRUCTIVE - double-check the device!):"
	@echo "     sudo dd if=$(DISK_IMAGE) of=/dev/rdiskN bs=4m"
	@echo ""
	@echo "  4. Eject: diskutil eject /dev/diskN, then boot the PC from USB"
endef

else

STLX_CC      := clang
STLX_CXX     := clang++
STLX_LLD     := ld.lld
STLX_AR      := llvm-ar
STLX_RANLIB  := llvm-ranlib
STLX_OBJCOPY := llvm-objcopy

define HOST_USB_INSTRUCTIONS
	@echo "  1. Identify your USB device (use 'lsblk' to find it)"
	@echo "     Example: /dev/sdb (NOT a partition like /dev/sdb1)"
	@echo ""
	@echo "  2. Unmount any mounted partitions:"
	@echo "     sudo umount /dev/sdX*"
	@echo ""
	@echo "  3. Write the image (DESTRUCTIVE - double-check the device!):"
	@echo "     sudo dd if=$(DISK_IMAGE) of=/dev/sdX bs=4M status=progress conv=fsync"
	@echo ""
	@echo "  4. Boot your PC from USB (check BIOS/UEFI boot menu)"
endef

endif

# Host-neutral command detection (existence-based, not OS-based).
# sgdisk lives in /sbin on Debian, which is not always on PATH.
SGDISK := $(firstword $(shell command -v sgdisk 2>/dev/null) \
	$(wildcard /sbin/sgdisk /usr/sbin/sgdisk) sgdisk)

# Debian ships aarch64 support in gdb-multiarch; Homebrew gdb is multi-target.
GDB_MULTIARCH := $(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb)

# Fail at parse time if the interface is incomplete for this host.
$(foreach v,HOST_OS STLX_CC STLX_CXX STLX_LLD STLX_AR STLX_RANLIB STLX_OBJCOPY \
	SGDISK GDB_MULTIARCH HOST_USB_INSTRUCTIONS,\
	$(if $(value $(v)),,$(error scripts/host.mk: $(v) is undefined for host '$(HOST_OS)')))

endif # STLX_HOST_MK_INCLUDED
