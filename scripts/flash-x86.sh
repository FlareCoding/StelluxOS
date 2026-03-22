#!/usr/bin/env bash
#
# Flash Stellux OS to a USB drive for UEFI x86_64 PCs.
#
# Usage:
#   ./scripts/flash-x86.sh /dev/sdb
#
# This script:
#   1. Builds the kernel for x86_64
#   2. Builds the userland
#   3. Creates a bootable EFI image (Limine + Stellux + initrd)
#   4. Unmounts any mounted partitions on the target device
#   5. Writes the image to the USB drive
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

LIMINE_DIR="$PROJECT_DIR/boot/limine"
IMG="$PROJECT_DIR/images/stellux-x86.img"
KERNEL="$PROJECT_DIR/build/kernel/x86_64/kernel.elf"

# --- Argument parsing ---

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <device>"
    echo ""
    echo "  device    Whole disk device for the USB drive (e.g., /dev/sdb)"
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

[[ -f "$LIMINE_DIR/BOOTX64.EFI" ]] || die "Limine not found. Run: make limine"

# --- Step 1: Build kernel and userland ---

info "Building kernel (ARCH=x86_64)..."
make -C "$PROJECT_DIR" clean
make -C "$PROJECT_DIR" kernel ARCH=x86_64

[[ -f "$KERNEL" ]] || die "Kernel build failed: $KERNEL not found"

info "Building userland (ARCH=x86_64)..."
make -C "$PROJECT_DIR" userland ARCH=x86_64

# --- Step 2: Create image ---

info "Creating boot image..."
mkdir -p "$(dirname "$IMG")"

dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
sgdisk --clear --new=1:2048:131038 --typecode=1:ef00 "$IMG" > /dev/null

mformat -i "$IMG"@@1M -F -v STELLUX ::

mmd   -i "$IMG"@@1M ::/EFI
mmd   -i "$IMG"@@1M ::/EFI/BOOT
mcopy -i "$IMG"@@1M "$LIMINE_DIR/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI

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
echo "  1. Safely eject the USB drive"
echo "  2. Insert into your target x86_64 PC"
echo "  3. Enter BIOS/UEFI boot menu (usually F12, F2, or Del at power-on)"
echo "  4. Select the USB drive (UEFI mode)"
echo "  5. Watch for: \"Stellux 3.0 booting...\""
echo ""
