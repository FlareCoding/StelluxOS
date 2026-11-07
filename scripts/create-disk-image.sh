#!/bin/sh
# Creates a disk image with one EFI system partition, sized to hold an initrd beside the kernel and boot files.
# The partition starts 1 MiB in and holds a FAT32 filesystem, which callers reach with mtools at <image>@@1M.
# make image and the flash scripts create their images with it.
#
# Usage: scripts/create-disk-image.sh <image> <initrd>
set -eu

IMAGE_MIN_MB=512
IMAGE_SLACK_MB=64
SECTORS_PER_MB=2048
GPT_BACKUP_SECTORS=33
PARTITION_START_MB=1

image="${1:?usage: create-disk-image.sh <image> <initrd>}"
initrd="${2:?usage: create-disk-image.sh <image> <initrd>}"
sgdisk="${SGDISK:-sgdisk}"

mb=$(( $(wc -c < "$initrd") / 1048576 + IMAGE_SLACK_MB ))
if [ "$mb" -lt "$IMAGE_MIN_MB" ]; then
    mb=$IMAGE_MIN_MB
fi

partition_first_sector=$(( PARTITION_START_MB * SECTORS_PER_MB ))
partition_last_sector=$(( mb * SECTORS_PER_MB - GPT_BACKUP_SECTORS - 1 ))
partition_sectors=$(( partition_last_sector - partition_first_sector + 1 ))

dd if=/dev/zero of="$image" bs=1M count="$mb" status=none
"$sgdisk" --clear --new=1:$partition_first_sector:$partition_last_sector --typecode=1:ef00 "$image" > /dev/null

# Without -T, mformat sizes the filesystem to the image's end, which holds the backup GPT
mformat -i "$image@@${PARTITION_START_MB}M" -T "$partition_sectors" -F -v STELLUX ::
