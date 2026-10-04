#!/bin/sh
# Creates an empty disk image with one EFI system partition, sized to hold an initrd beside the
# kernel and boot files. make image and the flash scripts create their images with it.
#
# Usage: scripts/create-disk-image.sh <image> <initrd>
set -eu

IMAGE_MIN_MB=512
IMAGE_SLACK_MB=64
GPT_BACKUP_SECTORS=34

image="${1:?usage: create-disk-image.sh <image> <initrd>}"
initrd="${2:?usage: create-disk-image.sh <image> <initrd>}"
sgdisk="${SGDISK:-sgdisk}"

mb=$(( $(wc -c < "$initrd") / 1048576 + IMAGE_SLACK_MB ))
if [ "$mb" -lt "$IMAGE_MIN_MB" ]; then
    mb=$IMAGE_MIN_MB
fi

dd if=/dev/zero of="$image" bs=1M count="$mb" status=none
"$sgdisk" --clear --new=1:2048:$(( mb * 2048 - GPT_BACKUP_SECTORS )) --typecode=1:ef00 "$image" > /dev/null
