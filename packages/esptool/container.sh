#!/bin/sh
# Runs inside the esptool recipe's container: installs the wheels of esptool's modules and esptool itself into
# Python's site-packages, adds each project's license texts and packs the tree into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the sources in /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"
ROOT="/work/root"
SITE_PACKAGES="$ROOT/usr/lib/$PYTHON_LIB/site-packages"
LICENSES="$ROOT/usr/share/licenses/esptool"
ESPTOOL_SRC="/work/esptool-$ESPTOOL_VER"

# Unpacking a pure-Python wheel into site-packages installs it, apart from its commands, which Stellux cannot start.
# Their scripts are dropped from .data, and rmdir stops the build when a .data holds anything else.
install_wheels() {
    mkdir -p "$SITE_PACKAGES"
    # shellcheck disable=SC2086
    set -- $WHEELS
    while [ $# -gt 0 ]; do
        python3 -m zipfile -e "/sources/$(basename "$1")" "$SITE_PACKAGES"
        shift 2
    done

    for data in "$SITE_PACKAGES"/*.data; do
        if [ -d "$data" ]; then
            rm -rf "$data/scripts"
            rmdir "$data"
        fi
    done
}

install_esptool() {
    tar -C /work -xzf "/sources/esptool-$ESPTOOL_VER.tar.gz"
    cp -R "$ESPTOOL_SRC/esptool" "$SITE_PACKAGES/"
}

# License texts in a directory per project, and an empty one stops the build so no project ships without its license
install_licenses() {
    for dist_info in "$SITE_PACKAGES"/*.dist-info; do
        project="$(basename "${dist_info%-*.dist-info}")"
        mkdir -p "$LICENSES/$project"
        find "$dist_info" \( -path '*/licenses/*' -o -name 'LICEN[CS]E*' -o -name 'COPYING*' -o -name 'NOTICE*' \) \
            -type f -exec cp {} "$LICENSES/$project/" \;
    done

    tar -xzf "/sources/pyserial-$PYSERIAL_VER.tar.gz" -O "pyserial-$PYSERIAL_VER/LICENSE.txt" \
        > "$LICENSES/pyserial/LICENSE.txt"
    mkdir -p "$LICENSES/esptool"
    cp "$ESPTOOL_SRC/LICENSE" "$LICENSES/esptool/"

    for project_licenses in "$LICENSES"/*; do
        if [ -z "$(ls -A "$project_licenses")" ]; then
            echo "esptool: the $(basename "$project_licenses") wheel holds no license file, take its license text" \
                "from its source release, as for pyserial" >&2
            exit 1
        fi
    done
}

# The tar file is written first, so a failing tar stops the build instead of leaving a truncated archive
package() {
    archive="/out/esptool-$(package_version esptool)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf /work/esptool.tar .
    zstd -19 -T0 -q --no-progress -f /work/esptool.tar -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

install_wheels
install_esptool
install_licenses
package
