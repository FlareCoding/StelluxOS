#!/bin/sh
# Runs inside the nvidia-firmware recipe's container: takes the GSP image and NVIDIA's license
# agreement out of the driver package, writes out GA102's bootloader and booters with NVIDIA's
# extraction tool, and packs the firmware with its license texts into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the driver package in /sources and the open
# kernel modules' files under /sources/open-gpu-kernel-modules
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
DRIVER_PACKAGE="$(basename "$DRIVER_URL")"
OPEN_MODULES="/sources/open-gpu-kernel-modules"
UNPACKED_DRIVER="/work/driver"
GSP_IMAGE="gsp_ga10x.bin"

# Where the package installs the firmware, in a directory per version, and its license texts, in
# a directory of its own under /usr/share/licenses
FIRMWARE_DIR="/usr/lib/firmware/nvidia/$NVIDIA_FW_VER"
LICENSES_DIR="/usr/share/licenses/nvidia-firmware"
ROOT="/work/root"

# NVIDIA's generated files and extraction tool name the GA10x family's boot files after GA102
BOOT_FILES_DIR="$FIRMWARE_DIR/ga102"

# --extract-only unpacks the driver package without running NVIDIA's installer. Its output is
# progress dots, so it is hidden and a failure gets a message of its own.
install_driver_package_files() {
    echo "=== unpacking $DRIVER_PACKAGE ==="
    if ! sh "/sources/$DRIVER_PACKAGE" --extract-only --target "$UNPACKED_DRIVER" > /dev/null; then
        echo "nvidia-firmware: $DRIVER_PACKAGE did not unpack into $UNPACKED_DRIVER" >&2
        exit 1
    fi

    if ! echo "$LICENSE_SHA256  $UNPACKED_DRIVER/LICENSE" | sha256sum -c --status -; then
        echo "nvidia-firmware: the LICENSE in $DRIVER_PACKAGE is missing or differs from the" \
            "agreement LICENSE_SHA256 pins, read it and update the pin" >&2
        exit 1
    fi

    install -D -m 644 "$UNPACKED_DRIVER/firmware/$GSP_IMAGE" "$ROOT$FIRMWARE_DIR/$GSP_IMAGE"
    install -D -m 644 "$UNPACKED_DRIVER/LICENSE" "$ROOT$LICENSES_DIR/LICENSE"
}

write_boot_files() {
    python3 /recipe/extract.py "$OPEN_MODULES" "$TOOL" "$NVIDIA_FW_VER" "$ROOT$BOOT_FILES_DIR"
}

# The notice a C file or a Python file opens with, without its comment marks
c_notice() {
    awk 'NR == 1 { next } /^ \*\// { exit } { sub(/^ \* ?/, ""); print }' "$1"
}

py_notice() {
    awk '/^# SPDX/ { found = 1 } found && /^$/ { exit } found { sub(/^# ?/, ""); print }' "$1"
}

# Stops the build unless a notice holds an SPDX copyright line and then an SPDX MIT identifier,
# the terms NOTICE states
check_mit_notice() {
    case "$2" in
    *"SPDX-FileCopyrightText: Copyright"*"SPDX-License-Identifier: MIT"*) ;;
    *)
        echo "nvidia-firmware: $1 lacks a copyright line or the MIT license" >&2
        exit 1
        ;;
    esac
}

# Which terms cover which file, with the notice the three generated files share stated once
write_notice() {
    generated_notice="$(c_notice "$OPEN_MODULES/$BOOTLOADER_SRC")"
    tool_notice="$(py_notice "$OPEN_MODULES/$TOOL")"
    check_mit_notice "$BOOTLOADER_SRC" "$generated_notice"
    check_mit_notice "$TOOL" "$tool_notice"

    for file in "$BOOTER_LOAD_SRC" "$BOOTER_UNLOAD_SRC"; do
        if [ "$(c_notice "$OPEN_MODULES/$file")" != "$generated_notice" ]; then
            echo "nvidia-firmware: $file carries a different notice from $BOOTLOADER_SRC," \
                "and NOTICE states one notice for all three" >&2
            exit 1
        fi
    done

    cat > "$ROOT$LICENSES_DIR/NOTICE" << EOF
The firmware files of this package are NVIDIA's, unmodified, and the license of the
StelluxOS source tree, whose recipe builds this package, does not cover them.

$FIRMWARE_DIR/$GSP_IMAGE comes from NVIDIA's driver package
$DRIVER_PACKAGE and is distributed under NVIDIA's license agreement in LICENSE.

The bootloader and booters in $BOOT_FILES_DIR/ were written out by
NVIDIA's extraction tool from files of NVIDIA's open kernel modules
(https://github.com/NVIDIA/open-gpu-kernel-modules) at release $NVIDIA_FW_VER. Those
files carry the MIT notice below, under which the bootloader and booters are distributed,
and the tool carries the one after it.

$BOOTLOADER_SRC
$BOOTER_LOAD_SRC
$BOOTER_UNLOAD_SRC

$generated_notice

$TOOL

$tool_notice
EOF
}

# The tar file is written first, so a failing tar stops the build instead of leaving a truncated
# archive
package_firmware() {
    archive="/out/nvidia-firmware-$(package_version nvidia-firmware)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf /work/firmware.tar .
    zstd -19 -T0 -q --no-progress -f /work/firmware.tar -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

install_driver_package_files
write_boot_files
write_notice
package_firmware
