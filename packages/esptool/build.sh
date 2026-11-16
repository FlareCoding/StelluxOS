#!/bin/sh
# The esptool recipe: esptool and the modules it imports, installed into Python's site-packages in a container from
# their pinned releases and left in the cache as esptool-<version>-<arch>.tar.zst.
#
# Usage: packages/esptool/build.sh <x86_64|aarch64> [esptool]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [esptool]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_DIR="$TOP/sources/esptool"
IMAGE_TAG="stellux-esptool"

case "$arch" in
x86_64|aarch64) ;;
*) echo "esptool: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "esptool: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The modules install into the site-packages of the Python the python recipe builds
python_recipe_lib="$(. "$REPO_ROOT/packages/python/versions.sh" && echo "python${PYTHON_VER%.*}")"
if [ "$python_recipe_lib" != "$PYTHON_LIB" ]; then
    echo "esptool: packages/python builds $python_recipe_lib but PYTHON_LIB names $PYTHON_LIB, update it and bump" \
        "ESPTOOL_PKG_REL" >&2
    exit 1
fi

# Downloads a source into the cache once, or checks the copy there, against its pin
fetch_source() {
    file="$SOURCES_DIR/$(basename "$1")"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
            echo "esptool: $file does not match its pin, delete it to download again" >&2
            exit 1
        }
        return 0
    fi

    echo "=== esptool: downloading $(basename "$1") ==="
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "esptool: downloaded $(basename "$1") does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$ESPTOOL_URL" "$ESPTOOL_SHA256"
fetch_source "$PYSERIAL_URL" "$PYSERIAL_SHA256"

# shellcheck disable=SC2086
set -- $WHEELS
while [ $# -gt 0 ]; do
    fetch_source "$1" "$2"
    shift 2
done

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/esptool 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

docker build -q -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== esptool: building $arch ==="
docker run --rm \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    "$IMAGE_TAG" "$arch"

archive="$TOP/esptool-$(package_version esptool)-$arch.tar.zst"
[ -f "$archive" ] || { echo "esptool: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== esptool: $arch done ==="
