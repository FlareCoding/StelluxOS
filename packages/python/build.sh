#!/bin/sh
# The python recipe: CPython for one architecture, cross-built in a container with the tree's compiler against the
# sysroot every Stellux program links, and left in the cache as python-<version>-<arch>.tar.zst. JOBS caps the
# container's parallel jobs.
#
# Usage: packages/python/build.sh <x86_64|aarch64> [python]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [python]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_DIR="$TOP/sources/python"
IMAGE_TAG="stellux-python"

# Keeps the sysroot between runs and architectures
VOLUME="stellux-python-work"

# The tree's sysroot targets expect an x86_64 Debian host, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "python: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "python: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The sysroot comes from the tree's own targets, which must build the versions pinned here
check_makefile_pin() {
    actual="$(awk -v v="$1" '$1 == v && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
    if [ "$actual" != "$2" ]; then
        echo "python: the Makefile builds $1 $actual, packages/python/versions.sh pins $2" >&2
        exit 1
    fi
}

check_makefile_pin MUSL_VERSION "$MUSL_VER"
check_makefile_pin LLVM_VERSION "$LLVM_VER"

# Downloads a source into the cache once, or checks the copy there, against its pin
fetch_source() {
    file="$SOURCES_DIR/$(basename "$1")"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
            echo "python: $file does not match its pinned checksum, delete it to download again" >&2
            exit 1
        }
        return 0
    fi

    echo "=== python: downloading $(basename "$1") ==="
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "python: downloaded $(basename "$1") does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$PYTHON_URL" "$PYTHON_SHA256"
fetch_source "$MUSL_URL" "$MUSL_SHA256"
fetch_source "$LLVM_URL" "$LLVM_SHA256"

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/python 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# The committed sysroot recipe, musl's patches and the headers they include, so the volume's sysroot is rebuilt
# when any of them changes
SYSROOT_KEY="$(git -C "$REPO_ROOT" rev-parse HEAD:Makefile HEAD:scripts/host.mk HEAD:userland/musl \
    HEAD:userland/lib/libstlx/include | tr '\n' ' ')"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== python: building $arch ==="
docker run --rm --platform "$PLATFORM" \
    -v "$VOLUME:/work" \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$stage:/in:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "SYSROOT_KEY=$SYSROOT_KEY" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    -e JOBS \
    "$IMAGE_TAG" "$arch"

archive="$TOP/python-$(package_version python)-$arch.tar.zst"
[ -f "$archive" ] || { echo "python: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== python: $arch done ==="
