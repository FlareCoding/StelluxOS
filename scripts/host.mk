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
STLX_STRIP   := $(call stlx_llvm_tool,llvm-strip,llvm-strip)
STLX_READELF := $(call stlx_llvm_tool,llvm-readelf,llvm-readelf)

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

define HOST_DEPS_INSTALL
	@echo "Installing required packages (Homebrew)..."
	@command -v brew > /dev/null 2>&1 || \
		{ echo "ERROR: Homebrew not found. Install it from https://brew.sh first."; exit 1; }
	brew install llvm lld cmake qemu mtools gptfdisk coreutils bash gdb zstd
	@echo ""
	@echo "Note: the 'python' userland app also needs a host python3.12"
	@echo "(e.g. 'brew install python@3.12')."
endef

# Extra CMake flags for cross-building the LLVM runtimes from a Darwin
# host: force cross mode so CMake applies no Apple platform rules.
CMAKE_HOST_FLAGS := \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_AR=$(STLX_AR) \
	-DCMAKE_RANLIB=$(STLX_RANLIB)

# Alpine's linux-headers package (plain tar.gz, one per arch). Alpine keeps
# only the latest version per branch: on a stale pin, bump version + sha256s.
LINUX_HEADERS_ALPINE  := v3.22
LINUX_HEADERS_VERSION := 6.14.2-r0
LINUX_HEADERS_SHA256_x86_64  := b0d7184f0e8d926961b82dff3d8a6a1f85db100dfc97e3fa1e6a25bbe9fd0f71
LINUX_HEADERS_SHA256_aarch64 := 08bc7264055d4ceca249e21f47875ccd7ae2dc7eaf49e235a83b1059e06d9089

# $(1) = target arch
define stlx_install_alpine_headers
	@apk="userland/linux-headers-$(LINUX_HEADERS_VERSION)-$(1).apk"; \
	url="https://dl-cdn.alpinelinux.org/alpine/$(LINUX_HEADERS_ALPINE)/main/$(1)/linux-headers-$(LINUX_HEADERS_VERSION).apk"; \
	if [ ! -f "$$apk" ]; then \
		curl -sfL -o "$$apk" "$$url" || { rm -f "$$apk"; \
			echo "ERROR: download failed: $$url"; \
			echo "The Alpine linux-headers pin is likely stale; bump it in scripts/host.mk."; \
			exit 1; }; \
	fi && \
	{ echo "$(LINUX_HEADERS_SHA256_$(1))  $$apk" | shasum -a 256 -c - > /dev/null || { \
		echo "ERROR: sha256 mismatch for $$apk; bump the pin in scripts/host.mk."; exit 1; }; } && \
	tmp="userland/linux-headers-extract-$(1)" && \
	rm -rf "$$tmp" && mkdir -p "$$tmp" && \
	tar -xzf "$$apk" -C "$$tmp" usr/include 2>/dev/null && \
	cp -R "$$tmp/usr/include/linux" "$$tmp/usr/include/asm" "$$tmp/usr/include/asm-generic" \
		"userland/sysroot/$(1)/include/" && \
	rm -rf "$$tmp" && \
	echo "  $(1): Alpine linux-headers $(LINUX_HEADERS_VERSION) installed"
endef

define HOST_SYSROOT_HEADERS_INSTALL
$(call stlx_install_alpine_headers,x86_64)
$(call stlx_install_alpine_headers,aarch64)
endef

else

STLX_CC      := clang
STLX_CXX     := clang++
STLX_LLD     := ld.lld
STLX_AR      := llvm-ar
STLX_RANLIB  := llvm-ranlib
STLX_OBJCOPY := llvm-objcopy
STLX_STRIP   := llvm-strip
STLX_READELF := readelf

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

define HOST_DEPS_INSTALL
	@echo "Installing required packages (Debian/Ubuntu)..."
	sudo apt install -y clang lld llvm \
		libclang-rt-dev \
		gcc-aarch64-linux-gnu \
		linux-libc-dev-arm64-cross \
		cmake \
		qemu-system-x86 qemu-system-arm \
		ovmf qemu-efi-aarch64 \
		mtools gdisk xorriso zstd \
		gdb-multiarch
endef

# No extra CMake flags: the LLVM runtimes cross-build natively on Linux.
CMAKE_HOST_FLAGS :=

# Linux UAPI headers come from the distro packages (make deps).
define HOST_SYSROOT_HEADERS_INSTALL
	@cp -r /usr/include/linux userland/sysroot/x86_64/include/
	@cp -r /usr/include/asm-generic userland/sysroot/x86_64/include/
	@cp -r /usr/include/x86_64-linux-gnu/asm userland/sysroot/x86_64/include/
	@cp -r /usr/aarch64-linux-gnu/include/linux userland/sysroot/aarch64/include/
	@cp -r /usr/aarch64-linux-gnu/include/asm-generic userland/sysroot/aarch64/include/
	@cp -r /usr/aarch64-linux-gnu/include/asm userland/sysroot/aarch64/include/
endef

endif

# Host-neutral command detection (existence-based, not OS-based).
# sgdisk lives in /sbin on Debian, which is not always on PATH.
SGDISK := $(firstword $(shell command -v sgdisk 2>/dev/null) \
	$(wildcard /sbin/sgdisk /usr/sbin/sgdisk) sgdisk)

# Debian ships aarch64 support in gdb-multiarch; Homebrew gdb is multi-target.
GDB_MULTIARCH := $(if $(shell command -v gdb-multiarch 2>/dev/null),gdb-multiarch,gdb)

# Logical CPU count for third-party builds (getconf works on both hosts).
NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# Fail at parse time if the interface is incomplete for this host.
# CMAKE_HOST_FLAGS is exempt: it is legitimately empty on Linux.
$(foreach v,HOST_OS STLX_CC STLX_CXX STLX_LLD STLX_AR STLX_RANLIB STLX_OBJCOPY \
	STLX_STRIP STLX_READELF \
	SGDISK GDB_MULTIARCH NPROC \
	HOST_USB_INSTRUCTIONS HOST_SYSROOT_HEADERS_INSTALL HOST_DEPS_INSTALL,\
	$(if $(value $(v)),,$(error scripts/host.mk: $(v) is undefined for host '$(HOST_OS)')))

endif # STLX_HOST_MK_INCLUDED
