# Root Makefile for Stellux OS

# General Variables
OSNAME := stellux
UNAME_S := $(shell uname -s)
ARCH := $(shell uname -m | sed s,i[3456789]86,ia32,)
export

# Compiler and Linker
CC  := gcc
CXX := g++
LD  := ld

# Subdirectories
BOOTLOADER_DIR := bootloader
KERNEL_DIR := kernel
BIN_DIR := bin
RESOURCE_DIR := resources

# Disk Image
DISK_IMG := $(BIN_DIR)/$(OSNAME).elf

# QEMU
QEMU_EMULATOR := qemu-system-x86_64
COMMON_QEMU_FLAGS := -drive file=$(DISK_IMG),format=raw -m 2G -net none
QEMU_FLAGS := $(COMMON_QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,file="efi/OVMF_CODE.fd",readonly=on -drive if=pflash,format=raw,unit=1,file="efi/OVMF_VARS.fd"

# Architecture Specifics
ifeq ($(ARCH), aarch64)
	QEMU_EMULATOR := qemu-system-aarch64
	QEMU_FLAGS += -cpu host -machine virt --enable-kvm
else
	QEMU_FLAGS += -cpu qemu64
endif

# Targets
.PHONY: all bootloader kernel buildimg run run-headless clean

# Default target
all: bootloader kernel buildimg

# Bootloader target
bootloader:
	$(MAKE) -C $(BOOTLOADER_DIR)

# Kernel target
kernel:
	$(MAKE) -C $(KERNEL_DIR)

# Image creation target
buildimg: $(DISK_IMG)

$(DISK_IMG): bootloader kernel
	@mkdir -p $(@D)
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=93750
	mkfs -t vfat $(DISK_IMG)
	mmd -i $(DISK_IMG) ::/EFI
	mmd -i $(DISK_IMG) ::/EFI/BOOT
	mcopy -i $(DISK_IMG) $(BOOTLOADER_DIR)/bootloader.efi ::/EFI/BOOT
	mcopy -i $(DISK_IMG) efi/startup.nsh ::
	mcopy -i $(DISK_IMG) $(KERNEL_DIR)/kernel.elf ::
	mcopy -i $(DISK_IMG) $(RESOURCE_DIR)/zap-light16.psf ::

# Run target
run: $(DISK_IMG)
	$(QEMU_EMULATOR) $(QEMU_FLAGS)

# Run headless target
run-headless: $(DISK_IMG)
	$(QEMU_EMULATOR) $(QEMU_FLAGS) -nographic

# Run target with GDB support
run-debug: $(DISK_IMG)
	$(QEMU_EMULATOR) -s -S $(QEMU_FLAGS)

# Run target with GDB support
run-debug-headless: $(DISK_IMG)
	$(QEMU_EMULATOR) -s -S $(QEMU_FLAGS) -nographic

# Connects GDB to a running qemu instance
connect-gdb:
	gdb -ex "source ./gdb_setup.gdb" -ex "target remote localhost:1234" -ex "add-symbol-file kernel/kernel.elf 0x7E5FB000 -y" -ex "b _kentry"

# Clean target
clean:
	rm -rf $(BIN_DIR)
	$(MAKE) -C $(BOOTLOADER_DIR) clean
	$(MAKE) -C $(KERNEL_DIR) clean
