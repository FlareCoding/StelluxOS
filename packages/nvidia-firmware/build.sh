#!/bin/sh
# The nvidia-firmware recipe: NVIDIA's GSP firmware for GA10x GPUs, taken out of NVIDIA's
# downloads in a container and left in the cache as
# nvidia-firmware-<version>-<release>-<arch>.tar.zst. Whoever runs it accepts NVIDIA's license
# agreement, see packages/README.md.
#
# Usage: packages/nvidia-firmware/build.sh <x86_64|aarch64> [nvidia-firmware]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [nvidia-firmware]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
IMAGE_TAG="stellux-nvidia-firmware"

# One directory per firmware version, since the generated files keep their names across versions.
# It sits below the top of the sources cache, whose files the toolchain recipe copies into its build
SOURCES_DIR="$TOP/sources/nvidia-firmware-$NVIDIA_FW_VER"

case "$arch" in
x86_64|aarch64) ;;
*) echo "nvidia-firmware: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "nvidia-firmware: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# Downloads a file to a path in the sources directory, or checks the copy there, against its pin
fetch_source() {
    file="$SOURCES_DIR/$3"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
            echo "nvidia-firmware: $file does not match its pin, delete it to refetch" >&2
            exit 1
        }
        return 0
    fi

    mkdir -p "$(dirname "$file")"
    echo "=== nvidia-firmware: downloading $3 ==="
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "nvidia-firmware: downloaded $3 does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

# The open kernel modules' files keep their paths, where the extraction tool looks for them
fetch_open_modules_file() {
    fetch_source "$OPEN_MODULES_URL/$1" "$2" "open-gpu-kernel-modules/$1"
}

fetch_source "$DRIVER_URL" "$DRIVER_SHA256" "$(basename "$DRIVER_URL")"
fetch_open_modules_file "$TOOL" "$TOOL_SHA256"
fetch_open_modules_file "$BOOTLOADER_SRC" "$BOOTLOADER_SRC_SHA256"
fetch_open_modules_file "$BOOTER_LOAD_SRC" "$BOOTER_LOAD_SRC_SHA256"
fetch_open_modules_file "$BOOTER_UNLOAD_SRC" "$BOOTER_UNLOAD_SRC_SHA256"

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/nvidia-firmware 2>/dev/null || echo 0)"

docker build -q -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== nvidia-firmware: building $arch ==="
docker run --rm \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    "$IMAGE_TAG" "$arch"

archive="$TOP/nvidia-firmware-$(package_version nvidia-firmware)-$arch.tar.zst"
[ -f "$archive" ] || { echo "nvidia-firmware: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== nvidia-firmware: $arch done ==="
