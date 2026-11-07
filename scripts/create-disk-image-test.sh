#!/bin/sh
# Tests that create-disk-image.sh keeps the filesystem inside its partition, off the backup GPT at the image's end:
# firmware that finds that table damaged can write it back over whatever file is stored there.
#
# Usage: scripts/create-disk-image-test.sh
set -eu

sgdisk="${SGDISK:-sgdisk}"
scripts_dir=$(cd "$(dirname "$0")" && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT
trap 'exit 1' HUP INT TERM

image="$work_dir/disk.img"
head -c 4096 /dev/zero > "$work_dir/initrd.cpio"
"$scripts_dir/create-disk-image.sh" "$image" "$work_dir/initrd.cpio"

partition_first_sector=$("$sgdisk" -i 1 "$image" | sed -n 's/^First sector: \([0-9]*\) .*/\1/p')
partition_sectors=$("$sgdisk" -i 1 "$image" | sed -n 's/^Partition size: \([0-9]*\) sectors.*/\1/p')
filesystem="$image@@$(( ${partition_first_sector:?sgdisk printed no first sector for partition 1} * 512 ))"
filesystem_sectors=$(minfo -i "$filesystem" | sed -n 's/^big size: \([0-9]*\) sectors.*/\1/p')
if [ -z "$filesystem_sectors" ]; then
    echo "create-disk-image-test: minfo printed no filesystem size for $filesystem" >&2
    exit 1
fi

if ! [ "$filesystem_sectors" -le "$partition_sectors" ]; then
    echo "create-disk-image-test: $filesystem_sectors filesystem sectors overrun the partition's $partition_sectors" >&2
    exit 1
fi

free_bytes=$(mdir -i "$filesystem" :: | sed -n 's/^ *\([0-9 ]*\) bytes free.*/\1/p' | tr -d ' ')
dd if=/dev/zero of="$work_dir/fill" bs=1 count=0 seek="$free_bytes" status=none
mcopy -i "$filesystem" "$work_dir/fill" ::/fill

if ! "$sgdisk" -v "$image" | grep -q "No problems found"; then
    "$sgdisk" -v "$image"
    echo "create-disk-image-test: sgdisk -v found problems in the GPT after the filesystem was filled" >&2
    exit 1
fi
echo "create-disk-image-test: passed"
