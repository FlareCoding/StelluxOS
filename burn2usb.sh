#!/bin/bash

if [ -z "$1" ]
  then
    echo "Usage: $0 <driveID>"
    echo "Tip: use lsblk to get the drive ID"
    echo "Example: $0 sdb"
    exit 0
fi

DRIVE_ID=/dev/$1
DRIVE_MOUNT_PATH=usb_mnt_point

# Mount the drive
mkdir -p $DRIVE_MOUNT_PATH
sudo mount $DRIVE_ID $DRIVE_MOUNT_PATH

# Create the efi boot directories
mkdir -p $DRIVE_MOUNT_PATH/efi
mkdir -p $DRIVE_MOUNT_PATH/efi/boot

# Copy the bootloader
cp ./bootloader/bootloader.efi $DRIVE_MOUNT_PATH/efi/boot/bootx64.efi

# Copy the kernel
cp ./kernel/bin/kernel.elf $DRIVE_MOUNT_PATH/kernel.elf

# Copy the text rendering font file
cp ./resources/zap-light16.psf $DRIVE_MOUNT_PATH/zap-light16.psf

# Unmount the drive
sudo umount $DRIVE_ID

# Delete the temporary mount point dir
rm -rf $DRIVE_MOUNT_PATH

printf "\nYou're all done! :)\n"
