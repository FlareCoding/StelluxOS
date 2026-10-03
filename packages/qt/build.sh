#!/bin/sh
# The qt recipe: a static Qt kit for one architecture, cross-built in a container against
# the Stellux sysroot and left in the cache as qt-<version>-<arch>.tar.zst.
#
# Usage: packages/qt/build.sh <x86_64|aarch64> [qt]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [qt]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_CACHE="$TOP/sources"
IMAGE_TAG="stellux-qt"

# Keeps the sysroot and the host Qt between architectures
VOLUME="stellux-qt-work"

# The tree's sysroot targets expect an x86_64 Debian host, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "qt: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "qt: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The sysroot comes from the tree's own targets, which must build the versions pinned here
check_makefile_pin() {
    actual="$(awk -v v="$1" '$1 == v && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
    if [ "$actual" != "$2" ]; then
        echo "qt: the Makefile builds $1 $actual, packages/qt/versions.sh pins $2" >&2
        exit 1
    fi
}

check_makefile_pin MUSL_VERSION "$MUSL_VER"
check_makefile_pin LLVM_VERSION "$LLVM_VER"

# Downloads a source tarball into the cache once and checks it against its pin
fetch_source() {
    file="$SOURCES_CACHE/$(basename "$1")"
    if [ ! -f "$file" ]; then
        curl -fsSL --retry 3 -o "$file.tmp" "$1"
        mv "$file.tmp" "$file"
    fi

    echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
        echo "qt: $file does not match its pinned checksum" >&2
        exit 1
    }
}

mkdir -p "$TOP" "$SOURCES_CACHE"
fetch_source "$QT_URL/qtbase-everywhere-src-$QT_VER.tar.xz" "$QTBASE_SHA256"
fetch_source "$QT_URL/qtsvg-everywhere-src-$QT_VER.tar.xz" "$QTSVG_SHA256"
fetch_source "$MUSL_URL" "$MUSL_SHA256"
fetch_source "$LLVM_URL" "$LLVM_SHA256"

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/qt 2>/dev/null || echo 0)"

# The committed sysroot recipe, so the volume's sysroot is rebuilt when it changes
SYSROOT_KEY="$(git -C "$REPO_ROOT" rev-parse HEAD:Makefile HEAD:scripts/host.mk | tr '\n' ' ')"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== qt: building $arch ==="
docker run --rm --platform "$PLATFORM" \
    -v "$VOLUME:/work" \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_CACHE:/sources:ro" \
    -v "$stage:/in:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "SYSROOT_KEY=$SYSROOT_KEY" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    "$IMAGE_TAG" "$arch"

archive="$TOP/qt-$(package_version qt)-$arch.tar.zst"
[ -f "$archive" ] || { echo "qt: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== qt: $arch done ==="
