#
# Stellux 3.0 Prototype - Top-Level Makefile
#
# Orchestrates kernel building, image creation, and QEMU execution.
# Delegates kernel compilation to kernel/Makefile.
#
# Usage:
#   make kernel ARCH=x86_64       # Build kernel
#   make image ARCH=x86_64        # Create bootable image
#   make image ARCH=x86_64        # Create disk image
#   make run ARCH=x86_64          # Run in QEMU
#   make V=1 ...                  # Verbose output
#   make RELEASE=1 ...            # Release build
#

# ============================================================================
# Configuration
# ============================================================================

# Output directories
BUILD_DIR := build
IMAGE_DIR := images

# Boot files
BOOT_DIR := boot/limine
LIMINE_BRANCH := v8.x-binary

# RPi4 UEFI firmware
RPI4_UEFI_DIR := boot/rpi4-uefi
RPI4_UEFI_VERSION := v1.41

# QEMU firmware paths (auto-detect)
OVMF_CODE := $(firstword $(wildcard \
	/usr/share/OVMF/OVMF_CODE_4M.fd \
	/usr/share/OVMF/OVMF_CODE.fd \
	/usr/share/ovmf/OVMF.fd \
	/usr/share/qemu/OVMF.fd))
OVMF_VARS := $(firstword $(wildcard \
	/usr/share/OVMF/OVMF_VARS_4M.fd \
	/usr/share/OVMF/OVMF_VARS.fd))
QEMU_EFI_AARCH64 := /usr/share/qemu-efi-aarch64/QEMU_EFI.fd

# GDB setup script
GDB_SETUP := scripts/gdb_setup.gdb
GDB_PORT := 4554

# QEMU settings
QEMU_MEMORY := 4G
QEMU_CPU_CORES ?= 4

# Verbosity (V=1 for verbose)
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# Export variables for sub-makes
export V
export DEBUG
export RELEASE
export BUILD_DIR
export STLX_UNIT_TESTS_ENABLED

# ============================================================================
# Supported Architectures
# ============================================================================

SUPPORTED_ARCHS := x86_64 aarch64

# ============================================================================
# ARCH Validation
# ============================================================================

# Targets that require ARCH
ARCH_REQUIRED_TARGETS := kernel userland image run test

# Check if current target requires ARCH
CURRENT_GOALS := $(MAKECMDGOALS)
ifeq ($(CURRENT_GOALS),)
  CURRENT_GOALS := help
endif

NEEDS_ARCH := $(filter $(ARCH_REQUIRED_TARGETS),$(CURRENT_GOALS))

ifneq ($(NEEDS_ARCH),)
  ifndef ARCH
    $(error ARCH is required for '$(NEEDS_ARCH)'. Use: make $(firstword $(NEEDS_ARCH)) ARCH=x86_64 or ARCH=aarch64)
  endif
  ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),)
    $(error Invalid ARCH=$(ARCH). Supported: $(SUPPORTED_ARCHS))
  endif
endif

# ============================================================================
# Derived Paths (when ARCH is set)
# ============================================================================

ifdef ARCH
  KERNEL_ELF := $(BUILD_DIR)/kernel/$(ARCH)/kernel.elf
  DISK_IMAGE := $(IMAGE_DIR)/stellux-$(ARCH).img
endif

# ============================================================================
# Primary Targets
# ============================================================================

.PHONY: all kernel userland image run run-headless clean test \
        image-x86_64 image-aarch64 \
        run-qemu-x86_64 run-qemu-aarch64 \
        run-qemu-x86_64-headless run-qemu-aarch64-headless \
        run-qemu-x86_64-debug run-qemu-aarch64-debug \
        run-qemu-x86_64-debug-headless run-qemu-aarch64-debug-headless \
        connect-gdb-x86_64 connect-gdb-aarch64 \
        deps limine musl rpi4-firmware toolchain-check \
        help

# Default target
all: help

# ============================================================================
# Kernel Build (delegates to kernel/Makefile)
# ============================================================================

kernel: check-lld
	$(Q)$(MAKE) -C kernel ARCH=$(ARCH) BUILD_DIR=../$(BUILD_DIR) \
		$(if $(RELEASE),RELEASE=1) \
		$(if $(DEBUG),DEBUG=1) \
		$(if $(STLX_UNIT_TESTS_ENABLED),STLX_UNIT_TESTS_ENABLED=1) \
		$(if $(V),V=1)

# ============================================================================
# Userland Build (delegates to userland/Makefile)
# ============================================================================

userland:
	$(Q)$(MAKE) -C userland ARCH=$(ARCH) $(if $(V),V=1)

# ============================================================================
# Unit Tests
# ============================================================================

test:
	$(Q)$(MAKE) clean
	$(Q)$(MAKE) image ARCH=$(ARCH) STLX_UNIT_TESTS_ENABLED=1
	@echo ""
	$(Q)./scripts/run_tests.sh $(ARCH)

# ============================================================================
# Disk Image Creation
# ============================================================================

image: kernel userland check-limine $(DISK_IMAGE)
	@echo ""
	@echo "Disk image ready: $(DISK_IMAGE)"

# Convenience targets (no ARCH= required)
image-x86_64:
	$(Q)$(MAKE) image ARCH=x86_64

image-aarch64:
	$(Q)$(MAKE) image ARCH=aarch64

INITRD_DIR  := initrd
INITRD_CPIO := $(BUILD_DIR)/initrd.cpio

.PHONY: $(INITRD_CPIO)
$(INITRD_CPIO):
	@mkdir -p $(BUILD_DIR)
	@echo "Creating initrd.cpio..."
	$(Q)cd $(INITRD_DIR) && find . -mindepth 1 | cpio -o -H newc > ../$(INITRD_CPIO) 2>/dev/null
	@echo "Created: $(INITRD_CPIO)"

$(IMAGE_DIR)/stellux-x86_64.img: $(BUILD_DIR)/kernel/x86_64/kernel.elf $(BOOT_DIR)/limine.conf $(INITRD_CPIO)
	@mkdir -p $(IMAGE_DIR)
	@echo "Creating x86_64 UEFI disk image..."
	$(Q)dd if=/dev/zero of=$@ bs=1M count=64 status=none
	$(Q)/sbin/sgdisk --clear --new=1:2048:131038 --typecode=1:ef00 $@ > /dev/null
	$(Q)mformat -i $@@@1M -F -v STELLUX ::
	$(Q)mmd -i $@@@1M ::/EFI
	$(Q)mmd -i $@@@1M ::/EFI/BOOT
	$(Q)mcopy -i $@@@1M $(BOOT_DIR)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	$(Q)mcopy -i $@@@1M $< ::/kernel.elf
	$(Q)mcopy -i $@@@1M $(BOOT_DIR)/limine.conf ::/limine.conf
	$(Q)mcopy -i $@@@1M $(INITRD_CPIO) ::/initrd.cpio
	@echo "Created: $@"

$(IMAGE_DIR)/stellux-aarch64.img: $(BUILD_DIR)/kernel/aarch64/kernel.elf $(BOOT_DIR)/limine.conf $(INITRD_CPIO)
	@mkdir -p $(IMAGE_DIR)
	@echo "Creating AArch64 UEFI disk image..."
	$(Q)dd if=/dev/zero of=$@ bs=1M count=64 status=none
	$(Q)/sbin/sgdisk --clear --new=1:2048:131038 --typecode=1:ef00 $@ > /dev/null
	$(Q)mformat -i $@@@1M -F -v STELLUX ::
	$(Q)mmd -i $@@@1M ::/EFI
	$(Q)mmd -i $@@@1M ::/EFI/BOOT
	$(Q)mcopy -i $@@@1M $(BOOT_DIR)/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
	$(Q)mcopy -i $@@@1M $< ::/kernel.elf
	$(Q)mcopy -i $@@@1M $(BOOT_DIR)/limine.conf ::/limine.conf
	$(Q)mcopy -i $@@@1M $(INITRD_CPIO) ::/initrd.cpio
	@echo "Created: $@"

# ============================================================================
# QEMU Execution
# ============================================================================

run: image
ifeq ($(ARCH),x86_64)
	$(Q)$(MAKE) run-qemu-x86_64
else ifeq ($(ARCH),aarch64)
	$(Q)$(MAKE) run-qemu-aarch64
endif

run-headless: image
ifeq ($(ARCH),x86_64)
	$(Q)$(MAKE) run-qemu-x86_64-headless
else ifeq ($(ARCH),aarch64)
	$(Q)$(MAKE) run-qemu-aarch64-headless
endif

run-qemu-x86_64: $(IMAGE_DIR)/stellux-x86_64.img $(BUILD_DIR)/OVMF_VARS.fd
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-x86_64 \
		-machine q35 \
		-cpu qemu64,+fsgsbase \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-x86_64.img \
		-device virtio-gpu-pci \
		-device qemu-xhci \
		-device usb-kbd \
		-serial mon:stdio \
		-no-reboot \
		-no-shutdown

$(BUILD_DIR)/OVMF_VARS.fd: $(OVMF_VARS)
	@mkdir -p $(BUILD_DIR)
	$(Q)cp $< $@

run-qemu-aarch64: $(IMAGE_DIR)/stellux-aarch64.img
	@echo "Starting QEMU AArch64 (TCG emulation)..."
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a57 \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-bios $(QEMU_EFI_AARCH64) \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-aarch64.img \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-serial mon:stdio \
		-no-reboot \
		-no-shutdown

# Headless QEMU (for SSH/no display)
run-qemu-x86_64-headless: $(IMAGE_DIR)/stellux-x86_64.img $(BUILD_DIR)/OVMF_VARS.fd
	@echo "Starting QEMU x86_64 (headless)..."
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-x86_64 \
		-machine q35 \
		-cpu qemu64,+fsgsbase \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-x86_64.img \
		-device qemu-xhci \
		-device usb-kbd \
		-nographic \
		-no-reboot \
		-no-shutdown

run-qemu-aarch64-headless: $(IMAGE_DIR)/stellux-aarch64.img
	@echo "Starting QEMU AArch64 (headless, TCG emulation)..."
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a57 \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-bios $(QEMU_EFI_AARCH64) \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-aarch64.img \
		-device qemu-xhci \
		-device usb-kbd \
		-nographic \
		-no-reboot \
		-no-shutdown

# ============================================================================
# QEMU Debug (with GDB support)
# ============================================================================

run-qemu-x86_64-debug: $(IMAGE_DIR)/stellux-x86_64.img $(BUILD_DIR)/OVMF_VARS.fd
	@echo "Starting QEMU x86_64 (debug, waiting for GDB on port $(GDB_PORT))..."
	@echo "Connect with: make connect-gdb-x86_64"
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-x86_64 \
		-machine q35 \
		-cpu qemu64,+fsgsbase \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-x86_64.img \
		-device virtio-gpu-pci \
		-device qemu-xhci \
		-device usb-kbd \
		-serial mon:stdio \
		-gdb tcp::$(GDB_PORT) \
		-S \
		-no-reboot \
		-no-shutdown

run-qemu-x86_64-debug-headless: $(IMAGE_DIR)/stellux-x86_64.img $(BUILD_DIR)/OVMF_VARS.fd
	@echo "Starting QEMU x86_64 (debug headless, waiting for GDB on port $(GDB_PORT))..."
	@echo "Connect with: make connect-gdb-x86_64"
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-x86_64 \
		-machine q35 \
		-cpu qemu64,+fsgsbase \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS.fd \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-x86_64.img \
		-device qemu-xhci \
		-device usb-kbd \
		-nographic \
		-gdb tcp::$(GDB_PORT) \
		-S \
		-no-reboot \
		-no-shutdown

run-qemu-aarch64-debug: $(IMAGE_DIR)/stellux-aarch64.img
	@echo "Starting QEMU AArch64 (debug, waiting for GDB on port $(GDB_PORT))..."
	@echo "Connect with: make connect-gdb-aarch64"
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a57 \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-bios $(QEMU_EFI_AARCH64) \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-aarch64.img \
		-device ramfb \
		-device qemu-xhci \
		-device usb-kbd \
		-serial mon:stdio \
		-gdb tcp::$(GDB_PORT) \
		-S \
		-no-reboot \
		-no-shutdown

run-qemu-aarch64-debug-headless: $(IMAGE_DIR)/stellux-aarch64.img
	@echo "Starting QEMU AArch64 (debug headless, waiting for GDB on port $(GDB_PORT))..."
	@echo "Connect with: make connect-gdb-aarch64"
	@echo ""
	@echo "Serial output below. QEMU monitor: Ctrl+A C | Exit: Ctrl+A X"
	@echo ""
	qemu-system-aarch64 \
		-machine virt \
		-cpu cortex-a57 \
		-m $(QEMU_MEMORY) \
		-smp $(QEMU_CPU_CORES) \
		-bios $(QEMU_EFI_AARCH64) \
		-drive format=raw,file=$(IMAGE_DIR)/stellux-aarch64.img \
		-device qemu-xhci \
		-device usb-kbd \
		-nographic \
		-gdb tcp::$(GDB_PORT) \
		-S \
		-no-reboot \
		-no-shutdown

# ============================================================================
# GDB Connection
# ============================================================================

connect-gdb-x86_64:
	gdb -ex "source ./$(GDB_SETUP)" \
	    -ex "target remote localhost:$(GDB_PORT)" \
	    -ex "add-symbol-file $(BUILD_DIR)/kernel/x86_64/kernel.elf" \
	    -ex "b boot/boot.cpp:stlx_init"

connect-gdb-aarch64:
	gdb-multiarch -ex "source ./$(GDB_SETUP)" \
	    -ex "set architecture aarch64" \
	    -ex "target remote localhost:$(GDB_PORT)" \
	    -ex "add-symbol-file $(BUILD_DIR)/kernel/aarch64/kernel.elf" \
	    -ex "b boot/boot.cpp:stlx_init"

# ============================================================================
# USB Boot
# ============================================================================

usb: image
	@echo ""
	@echo "=== USB Boot Instructions ==="
	@echo ""
	@echo "The disk image is ready: $(DISK_IMAGE)"
	@echo ""
	@echo "To write to a USB drive:"
	@echo ""
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
	@echo ""
	@echo "WARNING: This will ERASE the USB drive!"
	@echo ""

# ============================================================================
# Clean
# ============================================================================

clean:
	@echo "Cleaning build artifacts..."
	$(Q)rm -rf $(BUILD_DIR) $(IMAGE_DIR)
	$(Q)rm -rf userland/build
	$(Q)find initrd/bin -mindepth 1 ! -name '.gitkeep' -delete 2>/dev/null || true
	@echo "Done."

# ============================================================================
# Prerequisites
# ============================================================================

check-lld:
	@which ld.lld > /dev/null 2>&1 || \
		(echo ""; echo "ERROR: ld.lld not found. Run: make deps"; echo ""; exit 1)

check-limine:
	@test -f $(BOOT_DIR)/BOOTX64.EFI || \
		(echo ""; echo "ERROR: Limine not found. Run: make limine"; echo ""; exit 1)
	@test -f $(BOOT_DIR)/BOOTAA64.EFI || \
		(echo ""; echo "ERROR: Limine not found. Run: make limine"; echo ""; exit 1)

# ============================================================================
# Setup Targets (no ARCH required)
# ============================================================================

deps:
	@echo "Installing required packages (Debian/Ubuntu)..."
	sudo apt install -y clang lld llvm \
		libclang-rt-dev \
		gcc-aarch64-linux-gnu \
		qemu-system-x86 qemu-system-arm \
		ovmf qemu-efi-aarch64 \
		mtools gdisk xorriso \
		gdb-multiarch
	@echo ""
	@echo "Done. Run 'make toolchain-check' to verify."

MUSL_VERSION := 1.2.5
MUSL_URL     := https://musl.libc.org/releases/musl-$(MUSL_VERSION).tar.gz
MUSL_DIR     := userland/musl-$(MUSL_VERSION)
MUSL_TARBALL := userland/musl-$(MUSL_VERSION).tar.gz

musl:
	@echo "Building musl $(MUSL_VERSION) for x86_64 and aarch64..."
	@mkdir -p userland
	@test -f $(MUSL_TARBALL) || \
		(echo "Downloading musl $(MUSL_VERSION)..." && \
		 curl -L -o $(MUSL_TARBALL) $(MUSL_URL))
	@test -d $(MUSL_DIR) || \
		(echo "Extracting..." && \
		 cd userland && tar xf musl-$(MUSL_VERSION).tar.gz)
	@echo ""
	@echo "Building musl for x86_64..."
	@mkdir -p $(MUSL_DIR)/build-x86_64
	cd $(MUSL_DIR)/build-x86_64 && \
		CC="clang --target=x86_64-linux-musl" \
		../configure --prefix=$(abspath userland/sysroot/x86_64) --disable-shared > /dev/null
	$(MAKE) -C $(MUSL_DIR)/build-x86_64 -j$$(nproc) > /dev/null
	$(MAKE) -C $(MUSL_DIR)/build-x86_64 install > /dev/null
	@echo "musl x86_64 installed to userland/sysroot/x86_64/"
	@echo ""
	@echo "Building musl for aarch64..."
	@mkdir -p $(MUSL_DIR)/build-aarch64
	cd $(MUSL_DIR)/build-aarch64 && \
		CC="clang --target=aarch64-linux-musl" \
		../configure --prefix=$(abspath userland/sysroot/aarch64) --disable-shared > /dev/null
	$(MAKE) -C $(MUSL_DIR)/build-aarch64 -j$$(nproc) > /dev/null
	$(MAKE) -C $(MUSL_DIR)/build-aarch64 install > /dev/null
	@echo "musl aarch64 installed to userland/sysroot/aarch64/"
	@echo ""
	@echo "musl $(MUSL_VERSION) ready for both architectures."

limine:
	@echo "Downloading Limine ($(LIMINE_BRANCH))..."
	@mkdir -p $(BOOT_DIR)
	curl -L -o $(BOOT_DIR)/BOOTX64.EFI \
		"https://raw.githubusercontent.com/limine-bootloader/limine/$(LIMINE_BRANCH)/BOOTX64.EFI"
	curl -L -o $(BOOT_DIR)/BOOTAA64.EFI \
		"https://raw.githubusercontent.com/limine-bootloader/limine/$(LIMINE_BRANCH)/BOOTAA64.EFI"
	@echo ""
	@ls -la $(BOOT_DIR)/*.EFI
	@echo ""
	@echo "Limine ready in $(BOOT_DIR)/"

rpi4-firmware:
	@echo "Downloading RPi4 UEFI firmware ($(RPI4_UEFI_VERSION))..."
	@mkdir -p $(RPI4_UEFI_DIR)/overlays
	$(Q)curl -L -o $(RPI4_UEFI_DIR)/firmware.zip \
		"https://github.com/pftf/RPi4/releases/download/$(RPI4_UEFI_VERSION)/RPi4_UEFI_Firmware_$(RPI4_UEFI_VERSION).zip"
	$(Q)cd $(RPI4_UEFI_DIR) && unzip -o firmware.zip \
		RPI_EFI.fd start4.elf fixup4.dat bcm2711-rpi-4-b.dtb \
		overlays/miniuart-bt.dtbo overlays/upstream-pi4.dtbo
	$(Q)rm -f $(RPI4_UEFI_DIR)/firmware.zip
	@echo ""
	@ls -la $(RPI4_UEFI_DIR)/RPI_EFI.fd $(RPI4_UEFI_DIR)/start4.elf
	@echo ""
	@echo "RPi4 UEFI firmware ready in $(RPI4_UEFI_DIR)/"

toolchain-check:
	@echo "=== Toolchain Check ==="
	@echo ""
	@printf "%-24s" "clang:" && \
		(which clang > /dev/null 2>&1 && clang --version | head -1 || echo "NOT FOUND")
	@printf "%-24s" "clang++:" && \
		(which clang++ > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "aarch64-linux-gnu-gcc:" && \
		(which aarch64-linux-gnu-gcc > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "ld.lld:" && \
		(which ld.lld > /dev/null 2>&1 && ld.lld --version | head -1 || echo "NOT FOUND")
	@printf "%-24s" "llvm-objcopy:" && \
		(which llvm-objcopy > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND (optional)")
	@printf "%-24s" "qemu-system-x86_64:" && \
		(which qemu-system-x86_64 > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "qemu-system-aarch64:" && \
		(which qemu-system-aarch64 > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "OVMF (x86_64):" && \
		(test -n "$(OVMF_CODE)" && test -f "$(OVMF_CODE)" && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "QEMU_EFI (AArch64):" && \
		(test -f $(QEMU_EFI_AARCH64) && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "mformat:" && \
		(which mformat > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "sgdisk:" && \
		(which sgdisk > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND")
	@printf "%-24s" "gdb-multiarch:" && \
		(which gdb-multiarch > /dev/null 2>&1 && echo "OK" || echo "NOT FOUND (for AArch64 debugging)")
	@printf "%-24s" "Limine BOOTX64.EFI:" && \
		(test -f $(BOOT_DIR)/BOOTX64.EFI && echo "OK" || echo "NOT FOUND - run 'make limine'")
	@printf "%-24s" "Limine BOOTAA64.EFI:" && \
		(test -f $(BOOT_DIR)/BOOTAA64.EFI && echo "OK" || echo "NOT FOUND - run 'make limine'")
	@printf "%-24s" "RPi4 UEFI firmware:" && \
		(test -f $(RPI4_UEFI_DIR)/RPI_EFI.fd && echo "OK" || echo "NOT FOUND - run 'make rpi4-firmware'")
	@printf "%-24s" "musl (x86_64):" && \
		(test -f userland/sysroot/x86_64/lib/libc.a && echo "OK" || echo "NOT FOUND - run 'make musl'")
	@printf "%-24s" "musl (aarch64):" && \
		(test -f userland/sysroot/aarch64/lib/libc.a && echo "OK" || echo "NOT FOUND - run 'make musl'")
	@printf "%-24s" "builtins (x86_64):" && \
		(BRT=$$(clang --target=x86_64-linux-musl --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null); \
		 if [ -f "$$BRT" ]; then \
			echo "$$BRT"; \
		 elif which x86_64-linux-gnu-gcc > /dev/null 2>&1; then \
			BGCC=$$(x86_64-linux-gnu-gcc -print-libgcc-file-name 2>/dev/null); \
			[ -f "$$BGCC" ] && echo "$$BGCC" || echo "NOT FOUND"; \
		 else \
			echo "NOT FOUND"; \
		 fi)
	@printf "%-24s" "builtins (aarch64):" && \
		(BRT=$$(clang --target=aarch64-linux-musl --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null); \
		 if [ -f "$$BRT" ]; then \
			echo "$$BRT"; \
		 elif which aarch64-linux-gnu-gcc > /dev/null 2>&1; then \
			BGCC=$$(aarch64-linux-gnu-gcc -print-libgcc-file-name 2>/dev/null); \
			[ -f "$$BGCC" ] && echo "$$BGCC" || echo "NOT FOUND"; \
		 else \
			echo "NOT FOUND"; \
		 fi)
	@echo ""
	@echo "If anything is NOT FOUND, run 'make deps', 'make limine', 'make musl', and/or 'make rpi4-firmware'"

# ============================================================================
# Help
# ============================================================================

help:
	@echo "Stellux 3.0 - Build System"
	@echo ""
	@echo "Setup (run once):"
	@echo "  make deps                    Install required packages"
	@echo "  make limine                  Download Limine bootloader"
	@echo "  make musl                    Build musl libc for both architectures"
	@echo "  make rpi4-firmware           Download RPi4 UEFI firmware"
	@echo "  make toolchain-check         Verify tools are installed"
	@echo ""
	@echo "Build:"
	@echo "  make kernel ARCH=<arch>      Build kernel"
	@echo "  make userland ARCH=<arch>    Build userland applications"
	@echo "  make image ARCH=<arch>       Build kernel + userland + create disk image"
	@echo "  make image-x86_64            Build x86_64 disk image (shortcut)"
	@echo "  make image-aarch64           Build AArch64 disk image (shortcut)"
	@echo "  make run ARCH=<arch>         Build + run in QEMU (with display)"
	@echo "  make run-headless ARCH=<arch> Build + run headless (for SSH)"
	@echo "  make usb ARCH=<arch>         Build + print USB instructions"
	@echo ""
	@echo "Debugging (run QEMU target in one terminal, connect-gdb in another):"
	@echo "  make run-qemu-x86_64-debug           QEMU x86_64 with GDB (with display)"
	@echo "  make run-qemu-x86_64-debug-headless  QEMU x86_64 with GDB (headless)"
	@echo "  make run-qemu-aarch64-debug          QEMU AArch64 with GDB (with display)"
	@echo "  make run-qemu-aarch64-debug-headless QEMU AArch64 with GDB (headless)"
	@echo "  make connect-gdb-x86_64              Connect GDB to running x86_64 QEMU"
	@echo "  make connect-gdb-aarch64             Connect GDB to running AArch64 QEMU"
	@echo ""
	@echo "Options:"
	@echo "  V=1                          Verbose output (show commands)"
	@echo "  DEBUG=1                      Debug build (default)"
	@echo "  RELEASE=1                    Release build (-O2)"
	@echo ""
	@echo "Architectures: $(SUPPORTED_ARCHS)"
	@echo ""
	@echo "First-time setup:"
	@echo "  1. make deps"
	@echo "  2. make limine"
	@echo "  3. make musl"
	@echo ""
	@echo "Examples:"
	@echo "  make kernel ARCH=x86_64"
	@echo "  make run ARCH=aarch64"
	@echo "  make kernel ARCH=x86_64 RELEASE=1 V=1"
	@echo ""
	@echo "IDE Support:"
	@echo "  make -C kernel compile-commands ARCH=<arch>"
	@echo "                               Generate compile_commands.json for clangd"
	@echo ""
	@echo "Other:"
	@echo "  make clean                   Remove all build artifacts"
	@echo "  make help                    Show this help"
