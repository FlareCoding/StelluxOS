#!/bin/bash

# Usage: sudo ./burn_to_usb <device>
# Example: sudo ./burn_to_usb sda

IMG_PATH="build/image/stellux.img"

# Check if the script is run with sudo
if [[ $EUID -ne 0 ]]; then
  echo "This script must be run as root."
  exit 1
fi

# Check if the device is provided as an argument
if [ -z "$1" ]; then
  echo "Usage: sudo ./burn_to_usb <device>"
  echo "Example: sudo ./burn_to_usb sda"
  exit 1
fi

DEVICE="/dev/$1"

# Check if the image file exists
if [ ! -f "$IMG_PATH" ]; then
  echo "Image file not found: $IMG_PATH"
  exit 1
fi

# Confirm the device
echo "This will erase all data on $DEVICE. Are you sure? (yes/NO)"
read -r CONFIRM
if [[ "$CONFIRM" != "yes" ]]; then
  echo "Aborting."
  exit 1
fi

# Unmount any mounted partitions on the device
echo "Unmounting any mounted partitions on $DEVICE..."
umount "${DEVICE}"* 2>/dev/null || true

# Write the image to the device
echo "Writing image $IMG_PATH to $DEVICE..."
dd if="$IMG_PATH" of="$DEVICE" bs=4M status=progress oflag=sync

# Flush write buffers
echo "Flushing write buffers..."
sync

echo "Done. You can safely remove the USB drive."
