#!/usr/bin/env bash
#
# Flash Stellux OS to an SD card for Raspberry Pi 4.
#
# Usage:
#   ./scripts/flash-rpi4.sh /dev/sdb
#
# This script:
#   1. Builds the kernel with PLATFORM=rpi4
#   2. Builds the userland (init binary, etc.)
#   3. Creates a bootable image (RPi4 UEFI + Limine + Stellux + initrd)
#   4. Unmounts any mounted partitions on the target device
#   5. Writes the image to the SD card
#
# The device argument should be the whole disk (e.g., /dev/sdb),
# NOT a partition (e.g., /dev/sdb1).
#

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
RESET='\033[0m'

die() { echo -e "${RED}ERROR: $*${RESET}" >&2; exit 1; }
info() { echo -e "${GREEN}==>${RESET} ${BOLD}$*${RESET}"; }
warn() { echo -e "${YELLOW}WARNING: $*${RESET}"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

UEFI_DIR="$PROJECT_DIR/boot/rpi4-uefi"
LIMINE_DIR="$PROJECT_DIR/boot/limine"
IMG="$PROJECT_DIR/images/stellux-rpi4.img"
KERNEL="$PROJECT_DIR/build/kernel/aarch64/kernel.elf"

# --- Argument parsing ---

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <device>"
    echo ""
    echo "  device    Whole disk device for the SD card (e.g., /dev/sdb)"
    echo ""
    echo "Example:"
    echo "  $0 /dev/sdb"
    exit 1
fi

DEVICE="$1"

# Strip partition number if user passed e.g. /dev/sdb1
if [[ "$DEVICE" =~ ^(/dev/sd[a-z])[0-9]+$ ]]; then
    DEVICE="${BASH_REMATCH[1]}"
    warn "Stripped partition number, using whole disk: $DEVICE"
fi

# --- Safety checks ---

[[ -b "$DEVICE" ]] || die "$DEVICE is not a block device"

# Refuse to write to NVMe / system disks
if [[ "$DEVICE" == /dev/nvme* ]] || [[ "$DEVICE" == /dev/sda ]] && ! lsblk -no RM "$DEVICE" 2>/dev/null | grep -q '1'; then
    die "Refusing to write to $DEVICE (doesn't look like a removable disk)"
fi

REMOVABLE=$(lsblk -no RM "$DEVICE" 2>/dev/null | head -1 | tr -d ' ')
if [[ "$REMOVABLE" != "1" ]]; then
    die "$DEVICE is not a removable device (RM=$REMOVABLE). Aborting for safety."
fi

SIZE=$(lsblk -no SIZE "$DEVICE" 2>/dev/null | head -1 | tr -d ' ')
MODEL=$(lsblk -no MODEL "$DEVICE" 2>/dev/null | head -1 | sed 's/ *$//')

echo ""
echo -e "  Device:    ${BOLD}$DEVICE${RESET}"
echo -e "  Size:      ${BOLD}$SIZE${RESET}"
echo -e "  Model:     ${BOLD}$MODEL${RESET}"
echo ""
echo -e "  ${RED}${BOLD}ALL DATA ON $DEVICE WILL BE ERASED${RESET}"
echo ""
read -rp "Continue? [y/N] " confirm
[[ "$confirm" =~ ^[Yy]$ ]] || { echo "Aborted."; exit 0; }

# --- Check prerequisites ---

for tool in sgdisk mformat mcopy; do
    command -v "$tool" &>/dev/null || die "'$tool' not found. Run: make deps"
done

[[ -f "$UEFI_DIR/RPI_EFI.fd" ]] || die "RPi4 UEFI firmware not found. Run: make rpi4-firmware"
[[ -f "$LIMINE_DIR/BOOTAA64.EFI" ]] || die "Limine not found. Run: make limine"

# --- Step 1: Build kernel and userland ---

info "Building kernel (ARCH=aarch64 PLATFORM=rpi4)..."
make -C "$PROJECT_DIR" clean
make -C "$PROJECT_DIR" kernel ARCH=aarch64 PLATFORM=rpi4

[[ -f "$KERNEL" ]] || die "Kernel build failed: $KERNEL not found"

info "Building userland (ARCH=aarch64)..."
make -C "$PROJECT_DIR" userland ARCH=aarch64

# --- Step 2: Create image ---

info "Creating boot image..."
mkdir -p "$(dirname "$IMG")"

dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
sgdisk --clear --new=1:2048:131038 --typecode=1:ef00 "$IMG" > /dev/null

mformat -i "$IMG"@@1M -F -v STELLUX ::

mcopy -i "$IMG"@@1M "$UEFI_DIR/start4.elf" ::
mcopy -i "$IMG"@@1M "$UEFI_DIR/fixup4.dat" ::
mcopy -i "$IMG"@@1M "$UEFI_DIR/config.txt" ::
mcopy -i "$IMG"@@1M "$UEFI_DIR/RPI_EFI.fd" ::
mcopy -i "$IMG"@@1M "$UEFI_DIR/bcm2711-rpi-4-b.dtb" ::
mmd   -i "$IMG"@@1M ::/overlays
mcopy -i "$IMG"@@1M "$UEFI_DIR/overlays/miniuart-bt.dtbo" ::/overlays/
mcopy -i "$IMG"@@1M "$UEFI_DIR/overlays/upstream-pi4.dtbo" ::/overlays/

mmd   -i "$IMG"@@1M ::/EFI
mmd   -i "$IMG"@@1M ::/EFI/BOOT
mcopy -i "$IMG"@@1M "$LIMINE_DIR/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI

mcopy -i "$IMG"@@1M "$KERNEL" ::/kernel.elf
mcopy -i "$IMG"@@1M "$LIMINE_DIR/limine.conf" ::/limine.conf

INITRD_CPIO="$PROJECT_DIR/build/initrd.cpio"
info "Creating initrd.cpio..."
mkdir -p "$PROJECT_DIR/build"
(cd "$PROJECT_DIR/initrd" && find . -mindepth 1 | cpio -o -H newc > "$INITRD_CPIO" 2>/dev/null)
mcopy -i "$IMG"@@1M "$INITRD_CPIO" ::/initrd.cpio

# --- Step 3: Unmount & flash ---

info "Unmounting $DEVICE partitions..."
for part in "${DEVICE}"*; do
    if mountpoint -q "$(lsblk -no MOUNTPOINT "$part" 2>/dev/null | head -1)" 2>/dev/null; then
        sudo umount "$part" 2>/dev/null || true
    fi
done
sudo umount "${DEVICE}"* 2>/dev/null || true

info "Writing image to $DEVICE..."
sudo dd if="$IMG" of="$DEVICE" bs=4M status=progress conv=fsync
sync

# --- Done ---

echo ""
echo -e "${GREEN}${BOLD}Done!${RESET} Stellux is on $DEVICE."
echo ""
echo "Next steps:"
echo "  1. Remove the SD card from your desktop"
echo "  2. Power off the Pi"
echo "  3. Insert the SD card into the Pi"
echo "  4. Open serial on desktop:  sudo screen /dev/serial/by-id/usb-FTDI_FT232R_USB_UART_BG03CX76-if00-port0 115200"
echo "  5. Power on the Pi"
echo "  6. Watch for: \"Stellux 3.0 booting...\""
echo ""
