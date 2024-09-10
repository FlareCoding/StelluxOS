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
export

# Subdirectories
BOOTLOADER_DIR := bootloader
KERNEL_DIR := kernel
BIN_DIR := bin
RESOURCE_DIR := resources

# Disk Image
DISK_IMG := $(BIN_DIR)/$(OSNAME).elf

# QEMU
QEMU_CORES := 8
QEMU_EMULATOR := qemu-system-x86_64
COMMON_QEMU_FLAGS := -machine q35 -device qemu-xhci,id=xhci -drive file=$(DISK_IMG),format=raw -m 4G -net none -smp $(QEMU_CORES) -serial mon:stdio -trace usb_xhci_* -D /tmp/stellux-qemu-xhci.log
QEMU_FLAGS := $(COMMON_QEMU_FLAGS) -drive if=pflash,format=raw,unit=0,file="efi/OVMF_CODE.fd",readonly=on -drive if=pflash,format=raw,unit=1,file="efi/OVMF_VARS.fd"

# Sample connected USB 2.0 devices
QEMU_FLAGS += -device usb-hub,id=usbhub -device usb-mouse,id=usbmouse

#
# Sample connected USB 3.0 devices
QEMU_FLAGS += -device usb-storage,bus=xhci.0,drive=usb3drive,id=usb3drive -drive if=none,id=usb3drive,file=usb3_disk.img
#
# *Note* in order to test it out, create the backing storage ahead of time with:
#      qemu-img create -f qcow2 usb3_disk.img 128M
#

# Architecture Specifics
ifeq ($(ARCH), aarch64)
	QEMU_EMULATOR := qemu-system-aarch64
	QEMU_FLAGS += -cpu host -machine virt --enable-kvm
else
	QEMU_FLAGS += -cpu qemu64
endif

# Unit test execution duration timeout (all tests)
UNIT_TESTS_RUN_TIMEOUT := 1m

# Name of the unit test result output file
UNIT_TESTS_LOG_FILENAME := dev_os_output.log

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
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=93750 > /dev/null 2>&1
	mkfs -t vfat $(DISK_IMG)
	mmd -i $(DISK_IMG) ::/EFI
	mmd -i $(DISK_IMG) ::/EFI/BOOT
	mcopy -i $(DISK_IMG) $(BOOTLOADER_DIR)/bootloader.efi ::/EFI/BOOT
	mcopy -i $(DISK_IMG) efi/startup.nsh ::
	mcopy -i $(DISK_IMG) $(KERNEL_DIR)/bin/kernel.elf ::
	mcopy -i $(DISK_IMG) $(RESOURCE_DIR)/zap-light16.psf ::

# Dependencies
install-dependencies:
	sudo apt-get install build-essential g++ gnu-efi mtools qemu-system -y

# Run target
run: $(DISK_IMG)
	$(QEMU_EMULATOR) $(QEMU_FLAGS)

# Run headless target
run-headless: $(DISK_IMG)
	$(QEMU_EMULATOR) $(QEMU_FLAGS) -nographic

# Run target headless with GDB support
run-debug-headless: $(DISK_IMG)
	$(QEMU_EMULATOR) -gdb tcp::4554 -S $(QEMU_FLAGS) -nographic -no-reboot -no-shutdown

# Connects GDB to a running qemu instance
connect-gdb:
	gdb -ex "source ./gdb_setup.gdb" -ex "target remote localhost:4554" -ex "add-symbol-file kernel/bin/kernel.elf" -ex "b _kentry"

# Builds and runs a clean image of the OS with appropriate unit test flags
execute-unit-tests:
	@echo "[LOG] Preparing a clean build environment"
	@$(MAKE) clean > /dev/null

	@echo "[LOG] Building the kernel with unit tests"
	@$(MAKE) KRUN_UNIT_TESTS=1 > /dev/null

	@echo "[LOG] Launching the StelluxOS image in a VM"
	@timeout $(UNIT_TESTS_RUN_TIMEOUT) setsid bash -c '$(QEMU_EMULATOR) $(QEMU_FLAGS) -nographic | tee $(UNIT_TESTS_LOG_FILENAME)'

	@echo ""
	@echo "[LOG] Parsing unit test results"
	
	# Grouping commands in a single shell to preserve RESULT and handle cleanup
	@bash -c '\
		bash parse_unit_test_results.sh; \
		RESULT=$$?; \
		echo "[LOG] Cleaning up"; \
		$(MAKE) clean > /dev/null; \
		exit $$RESULT \
	'

# Clean target
clean:
	rm -rf $(BIN_DIR)
	$(MAKE) -C $(BOOTLOADER_DIR) clean
	$(MAKE) -C $(KERNEL_DIR) clean
	rm -rf com1.serial com2.serial $(UNIT_TESTS_LOG_FILENAME)
