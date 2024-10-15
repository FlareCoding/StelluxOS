#!/bin/bash

if [ -z "$1" ]
  then
    echo "Usage: $0 <driveID>"
    echo "Tip: use lsblk to get the drive ID"
    echo "Example: $0 sdb"
    exit 0
fi

DRIVE_ID=/dev/$1

sudo mkfs.vfat -F 32 $DRIVE_ID
