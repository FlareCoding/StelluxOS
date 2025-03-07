# =========================
# Variables
# =========================

# Directories and Files
KERNEL_DIR      := kernel
USERLAND_DIR    := userland
GRUB_DIR        := grub
BUILD_DIR       := build
INITRD_DIR  	:= initrd
IMAGE_DIR       := $(BUILD_DIR)/image
STELLUX_IMAGE   := $(IMAGE_DIR)/stellux.img
KERNEL_FILE     := $(BUILD_DIR)/stellux
GRUB_CFG_PATH   := $(GRUB_DIR)/grub.cfg
GRUB_FONT_PATH  := $(GRUB_DIR)/fonts/unicode.pf2
INITRD_ARCHIVE  := $(BUILD_DIR)/initrd

# OVMF Firmware Files
OVMF_DIR        := ovmf
OVMF_CODE       := $(OVMF_DIR)/OVMF_CODE.fd
OVMF_VARS       := $(OVMF_DIR)/OVMF_VARS.fd

# Image Configuration
IMAGE_SIZE_MB    := 60
ESP_SIZE_MB      := 40

# QEMU Configuration
QEMU             := qemu-system-x86_64
QEMU_CORES       := 8
QEMU_RAM         := 4G
QEMU_FLAGS       := \
    -machine q35 \
	-cpu qemu64,+fsgsbase \
    -m $(QEMU_RAM) \
    -serial mon:stdio \
	-serial pty \
    -drive file=$(STELLUX_IMAGE),format=raw \
    -net none \
    -smp $(QEMU_CORES) \
    -drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
    -drive if=pflash,format=raw,file="$(OVMF_VARS)" \
    -boot order=c \
	-device qemu-xhci,id=xhci \
	-trace usb_xhci_* -D /tmp/stellux-qemu-xhci.log

# Sample connected USB 2.0 devices
QEMU_FLAGS += -device usb-kbd,id=usbkbd
QEMU_FLAGS += -device usb-mouse,id=usbmouse

# Unit test execution duration timeout (all tests)
UNIT_TESTS_RUN_TIMEOUT := 5m

# Name of the unit test result output file
UNIT_TESTS_LOG_FILENAME := unit_tests.out

# QEMU_FLAGS += -device pci-serial,bus=pcie.0,addr=0x3,chardev=serial_pci -chardev file,id=serial_pci,path=uart_pci.log

# GDB Configuration
GDB_SETUP       := gdb_setup.gdb

# =========================
# Tools
# =========================
LOSETUP         := losetup
GRUB_INSTALL    := grub-install
MKFS_FAT        := mkfs.fat
PARTED          := parted
MOUNT           := mount
UMOUNT          := umount
RM              := rm
MKDIR           := mkdir -p
CP              := cp

# =========================
# Targets
# =========================

# Default Target: Show Help
all: help

# Help Target: Displays available targets
help:
	@echo "---- Stellux Makefile Options ----"
	@echo ""
	@echo "Available Targets:"
	@echo "  make help            		Show this help message"
	@echo "  make install-dependencies	Installs the necessary tools and packages for the current Linux distribution"
	@echo "  make kernel          		Build the Stellux kernel"
	@echo "  make userland          	Build the userspace Stellux applications"
	@echo "  make image           		Create the UEFI-compatible disk image (requires sudo)"
	@echo "  make initrd           		Rebuild and package an initrd cpio ramdisk"
	@echo "  make run             		Run the Stellux image in QEMU"
	@echo "  make run-headless    		Run QEMU without graphical output"
	@echo "  make run-debug       		Run QEMU with GDB support"
	@echo "  make connect-gdb     		Connect GDB to a running QEMU instance"
	@echo "  make execute-unit-tests	Build and run a special image version with unit tests"
	@echo "  make generate-docs	        Generate Doxygen documentation in the docs/ directory"
	@echo "  make clean-docs           	Clean up and remove the Doxygen-generated docs"
	@echo "  make clean           		Clean build artifacts and disk image"
	@echo ""

# Builds the kernel
kernel:
	@$(MKDIR) $(BUILD_DIR)
	@$(MAKE) -C kernel
	@cp $(KERNEL_DIR)/build/stellux $(KERNEL_FILE)

# Builds userland applications and modules
userland: $(KERNEL_FILE)
	@$(MAKE) -C userland
	@$(MAKE) -C userland install_to_initrd

# Builds the initrd ramdisk
initrd:
	@echo "Building initrd from $(INITRD_DIR)"
	@mkdir -p $(BUILD_DIR)
	@cd $(INITRD_DIR) && find . | cpio -o --format=newc > ../$(INITRD_ARCHIVE)
	@echo "Initrd created at $(INITRD_ARCHIVE)"

# Builds the final .img stellux image
image: kernel userland initrd $(STELLUX_IMAGE)

$(BUILD_DIR):
	@$(MKDIR) $(BUILD_DIR)

$(IMAGE_DIR): $(BUILD_DIR)
	@$(MKDIR) $(IMAGE_DIR)

$(KERNEL_FILE): $(BUILD_DIR)
	$(MAKE) -C $(KERNEL_DIR)
	@cp $(KERNEL_DIR)/build/stellux $(KERNEL_FILE)

$(INITRD_ARCHIVE): $(BUILD_DIR)
	$(MAKE) initrd

$(STELLUX_IMAGE): $(IMAGE_DIR) $(KERNEL_FILE) $(INITRD_ARCHIVE) $(GRUB_CFG_PATH)
	@echo "Creating raw disk image..."
	@dd if=/dev/zero of=$(STELLUX_IMAGE) bs=1M count=$(IMAGE_SIZE_MB)

	@echo "Partitioning the disk image with GPT and creating EFI System Partition (ESP)..."
	@sudo $(PARTED) $(STELLUX_IMAGE) --script mklabel gpt \
		mkpart ESP fat32 1MiB $$(($(ESP_SIZE_MB)+1))MiB \
		set 1 boot on

	@echo "Setting up loop device, formatting ESP, mounting, installing GRUB, and copying files..."
	@set -e; \
	LOOP_DEV=$$(sudo $(LOSETUP) -Pf --show $(STELLUX_IMAGE)); \
	echo "Loop device: $$LOOP_DEV"; \
	sudo $(MKFS_FAT) -F32 $${LOOP_DEV}p1; \
	sudo $(MKDIR) /mnt/efi; \
	sudo $(MOUNT) $${LOOP_DEV}p1 /mnt/efi; \
	trap "sudo umount /mnt/efi && sudo rmdir /mnt/efi && sudo losetup -d $${LOOP_DEV}" EXIT; \
	sudo $(GRUB_INSTALL) \
		--target=x86_64-efi \
		--efi-directory=/mnt/efi \
		--boot-directory=/mnt/efi/boot \
		--removable \
		--recheck \
		--no-floppy; \
	sudo $(MKDIR) /mnt/efi/boot/grub; \
	sudo $(CP) $(GRUB_CFG_PATH) /mnt/efi/boot/grub/grub.cfg; \
	sudo $(CP) $(GRUB_FONT_PATH) /mnt/efi/boot/grub/fonts/; \
	sudo $(CP) $(KERNEL_FILE) /mnt/efi/boot/stellux; \
	sudo $(CP) $(INITRD_ARCHIVE) /mnt/efi/boot/initrd;
	sudo $(UMOUNT) /mnt/efi; \
	sudo $(RM) -rf /mnt/efi; \
	sudo $(LOSETUP) -d $${LOOP_DEV}; \
	trap - EXIT; \
	echo "Final image '$(STELLUX_IMAGE)' is ready."

# Clean Up Build Files and Image
clean:
	@echo "Cleaning up build files and disk image..."
	$(MAKE) -C $(KERNEL_DIR) clean
	$(MAKE) -C $(USERLAND_DIR) clean
	@rm -rf $(BUILD_DIR)
	@rm -rf *.log
	@rm -rf $(UNIT_TESTS_LOG_FILENAME)
	@rm -rf $(INITRD_DIR)/bin

# Run the Disk Image in QEMU
run: $(STELLUX_IMAGE)
	$(QEMU) $(QEMU_FLAGS)

# Run the Disk Image in QEMU Headless
run-headless: $(STELLUX_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -nographic

# Run QEMU with GDB Support
run-debug: $(STELLUX_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -gdb tcp::4554 -S -no-reboot -no-shutdown

# Run QEMU Headless with GDB Support
run-debug-headless: $(STELLUX_IMAGE)
	$(QEMU) $(QEMU_FLAGS) -gdb tcp::4554 -S -nographic -no-reboot -no-shutdown

# Connect GDB to a Running QEMU Instance
connect-gdb:
	gdb -ex "source ./$(GDB_SETUP)" \
	    -ex "target remote localhost:4554" \
	    -ex "add-symbol-file $(KERNEL_FILE)" \
	    -ex "b init.cpp:init"

# Connect GDB to a Running QEMU Instance through a serial connection
connect-gdb-serial:
	gdb -ex "source ./$(GDB_SETUP)" \
	    -ex "add-symbol-file $(KERNEL_FILE)" \
		-ex "set architecture i386:x86-64" \
	    -ex "target remote /dev/ttyS1"

# Builds and runs a clean image of the OS with appropriate unit test flags
execute-unit-tests:
	@echo "[LOG] Preparing a clean build environment"
	@$(MAKE) clean > /dev/null

	@echo "[LOG] Building the kernel with unit tests"
	@$(MAKE) image BUILD_UNIT_TESTS=1 > /dev/null

	@echo "[LOG] Launching the StelluxOS image in a VM"
	@timeout $(UNIT_TESTS_RUN_TIMEOUT) setsid bash -c '$(QEMU) $(QEMU_FLAGS) -nographic | tee $(UNIT_TESTS_LOG_FILENAME)'

	@echo ""
	@echo "[LOG] Parsing unit test results"
	@bash -c '\
		bash ./tools/parse_unit_test_results.sh; \
		RESULT=$$?; \
		echo "[LOG] Cleaning up"; \
		$(MAKE) clean > /dev/null; \
		exit $$RESULT \
	'

# Install all necessary dependencies based on the Linux distribution
install-dependencies:
	@echo "Installing dependencies..."
	@if [ -f /etc/debian_version ]; then \
		echo "Detected Debian-based system."; \
		sudo apt-get update && sudo apt-get install -y \
			build-essential \
			grub2 \
			dosfstools \
			parted \
			qemu-system-x86 \
			gdb \
			ovmf \
			cpio \
			doxygen \
			$$( [ -f /usr/lib/grub/x86_64-efi/modinfo.sh ] && echo "" || echo "grub-efi-amd64" ); \
	elif [ -f /etc/redhat-release ]; then \
		echo "Detected RedHat-based system."; \
		sudo dnf groupinstall -y "Development Tools" && \
		sudo dnf install -y \
			dosfstools \
			parted \
			qemu-system-x86_64 \
			gdb \
			edk2-ovmf \
			cpio \
			doxygen \
			grub2-efi-x64; \
	elif [ -f /etc/arch-release ]; then \
		echo "Detected Arch-based system."; \
		sudo pacman -Sy --noconfirm \
			base-devel \
			grub \
			dosfstools \
			parted \
			qemu \
			gdb \
			ovmf \
			cpio \
			doxygen; \
	else \
		echo "Unsupported Linux distribution. Please install the following packages manually:"; \
		echo "  - build-essential / Development Tools / base-devel"; \
		echo "  - grub2 / grub / grub2-efi-x64"; \
		echo "  - dosfstools"; \
		echo "  - parted"; \
		echo "  - qemu-system-x86 / qemu-system-x86_64 / qemu"; \
		echo "  - gdb"; \
		echo "  - ovmf / edk2-ovmf"; \
		echo "  - cpio"; \
		echo "  - doxygen"; \
		exit 1; \
	fi

# Builds the doxygen documentation
generate-docs:
	@echo "Generating documentation using Doxygen..."
	@if [ -f docs/Doxyfile ]; then \
		cd docs && doxygen Doxyfile; \
		echo "Documentation generated successfully. Check the docs/output/html directory."; \
	else \
		echo "Error: Doxyfile not found in docs/ directory. Please create it first."; \
		exit 1; \
	fi

clean-docs:
	@echo "Cleaning generated documentation..."
	@rm -rf docs/output

# =========================
# Phony Targets
# =========================

.PHONY: all help kernel initrd clean run run-headless run-debug \
        run-debug-headless connect-gdb install-dependencies \
        generate-docs clean-docs
