# =========================
# Variables
# =========================

# Directories and Files
KERNEL_DIR      := kernel
GRUB_DIR        := grub
BUILD_DIR       := build
IMAGE_DIR       := $(BUILD_DIR)/image
STELLUX_IMAGE   := $(IMAGE_DIR)/stellux.img
KERNEL_FILE     := $(BUILD_DIR)/stellux
GRUB_CFG_PATH   := $(GRUB_DIR)/grub.cfg

# OVMF Firmware Files
OVMF_DIR        := ovmf
OVMF_CODE       := $(OVMF_DIR)/OVMF_CODE.fd
OVMF_VARS       := $(OVMF_DIR)/OVMF_VARS.fd

# Image Configuration
IMAGE_SIZE_MB    := 200
ESP_SIZE_MB      := 100

# QEMU Configuration
QEMU             := qemu-system-x86_64
QEMU_CORES       := 8
QEMU_RAM         := 4G
QEMU_FLAGS       := \
    -machine q35 \
	--enable-kvm \
	-cpu host,+fsgsbase \
    -m $(QEMU_RAM) \
    -serial mon:stdio \
    -drive file=$(STELLUX_IMAGE),format=raw \
    -net none \
    -smp $(QEMU_CORES) \
    -drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
    -drive if=pflash,format=raw,file="$(OVMF_VARS)" \
    -boot order=c

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
	@echo "  make help            Show this help message"
	@echo "  make kernel          Build the Stellux kernel"
	@echo "  make image           Create the UEFI-compatible disk image (requires sudo)"
	@echo "  make run             Run the Stellux image in QEMU"
	@echo "  make run-headless    Run QEMU without graphical output"
	@echo "  make run-debug       Run QEMU with GDB support"
	@echo "  make connect-gdb     Connect GDB to a running QEMU instance"
	@echo "  make clean           Clean build artifacts and disk image"
	@echo ""

# Builds the Kernel
kernel:
	@$(MKDIR) $(BUILD_DIR)
	@$(MAKE) -C kernel
	@cp $(KERNEL_DIR)/build/stellux $(KERNEL_FILE)

# Builds the final .img stellux image
image: kernel $(STELLUX_IMAGE)

$(BUILD_DIR):
	@$(MKDIR) $(BUILD_DIR)

$(IMAGE_DIR): $(BUILD_DIR)
	@$(MKDIR) $(IMAGE_DIR)

$(KERNEL_FILE): $(BUILD_DIR)
	$(MAKE) -C $(KERNEL_DIR)
	@cp $(KERNEL_DIR)/build/stellux $(KERNEL_FILE)

$(STELLUX_IMAGE): $(IMAGE_DIR) $(KERNEL_FILE) $(GRUB_CFG_PATH)
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
	sudo $(CP) $(KERNEL_FILE) /mnt/efi/boot/stellux; \
	sudo $(UMOUNT) /mnt/efi; \
	sudo $(RM) -rf /mnt/efi; \
	sudo $(LOSETUP) -d $${LOOP_DEV}; \
	trap - EXIT; \
	echo "Final image '$(STELLUX_IMAGE)' is ready."

# Clean Up Build Files and Image
clean:
	@echo "Cleaning up build files and disk image..."
	$(MAKE) -C $(KERNEL_DIR) clean
	@rm -rf $(BUILD_DIR)

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
	    -ex "b init"

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
			ovmf; \
	elif [ -f /etc/redhat-release ]; then \
		echo "Detected RedHat-based system."; \
		sudo dnf groupinstall -y "Development Tools" && \
		sudo dnf install -y \
			grub2-efi-x64 \
			dosfstools \
			parted \
			qemu-system-x86_64 \
			gdb \
			edk2-ovmf; \
	elif [ -f /etc/arch-release ]; then \
		echo "Detected Arch-based system."; \
		sudo pacman -Sy --noconfirm \
			base-devel \
			grub \
			dosfstools \
			parted \
			qemu \
			gdb \
			ovmf; \
	else \
		echo "Unsupported Linux distribution. Please install the following packages manually:"; \
		echo "  - build-essential / Development Tools / base-devel"; \
		echo "  - grub2 / grub / grub2-efi-x64"; \
		echo "  - dosfstools"; \
		echo "  - parted"; \
		echo "  - qemu-system-x86 / qemu-system-x86_64 / qemu"; \
		echo "  - gdb"; \
		echo "  - ovmf / edk2-ovmf"; \
		exit 1; \
	fi

# =========================
# Phony Targets
# =========================

.PHONY: all help kernel clean run run-headless run-debug run-debug-headless connect-gdb install-dependencies
