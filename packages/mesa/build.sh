#!/bin/sh
# The mesa recipe: the Vulkan library for one architecture, Mesa's NVK prelinked into one object
# and archived with the loader shim, cross-built in a container against the Stellux sysroot and
# left in the cache as mesa-<version>-<arch>.tar.zst. JOBS caps the container's parallel jobs.
#
# Usage: packages/mesa/build.sh <x86_64|aarch64> [mesa]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [mesa]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
IMAGE_TAG="stellux-mesa-kit"

# Below the top of the sources cache, whose files the toolchain recipe copies into its build
SOURCES_DIR="$TOP/sources/mesa"

# Keeps the sysroot, the build-time tools and Mesa's fetched crates between architectures
VOLUME="stellux-mesa-kit-work"

# The tree's sysroot targets expect an x86_64 Debian host, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "mesa: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "mesa: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The sysroot comes from the tree's own targets, which must build the versions pinned here
check_makefile_pin() {
    actual="$(awk -v v="$1" '$1 == v && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
    if [ "$actual" != "$2" ]; then
        echo "mesa: the Makefile builds $1 $actual, packages/mesa/versions.sh pins $2" >&2
        exit 1
    fi
}

check_makefile_pin MUSL_VERSION "$MUSL_VER"
check_makefile_pin LLVM_VERSION "$LLVM_VER"

# Downloads a source tarball into the cache as the named file once, or checks the copy there,
# against its pin
fetch_source() {
    file="$SOURCES_DIR/$3"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 512 -c - > /dev/null || {
            echo "mesa: $file does not match its pinned checksum, delete it to refetch" >&2
            exit 1
        }
        return 0
    fi

    echo "=== mesa: downloading $3 ==="
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 512 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "mesa: downloaded $3 does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$MESA_URL" "$MESA_SHA512" "mesa-$MESA_VER.tar.xz"
fetch_source "$LIBDRM_URL" "$LIBDRM_SHA512" "libdrm-$LIBDRM_VER.tar.xz"
fetch_source "$ZLIB_URL" "$ZLIB_SHA512" "zlib-$ZLIB_VER.tar.gz"
fetch_source "$ELFUTILS_URL" "$ELFUTILS_SHA512" "elfutils-$ELFUTILS_VER.tar.bz2"
fetch_source "$ARGP_URL" "$ARGP_SHA512" "argp-standalone-$ARGP_VER.tar.gz"
fetch_source "$FTS_URL" "$FTS_SHA512" "musl-fts-$FTS_VER.tar.gz"
fetch_source "$OBSTACK_URL" "$OBSTACK_SHA512" "musl-obstack-$OBSTACK_VER.tar.gz"
fetch_source "$MUSL_URL" "$MUSL_SHA512" "musl-$MUSL_VER.tar.gz"
fetch_source "$LLVM_URL" "$LLVM_SHA512" "llvm-project-$LLVM_VER.src.tar.xz"

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/mesa 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# The committed sysroot recipe, so the volume's sysroot is rebuilt when it changes
SYSROOT_KEY="$(git -C "$REPO_ROOT" rev-parse HEAD:Makefile HEAD:scripts/host.mk | tr '\n' ' ')"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    --build-arg "MESON_VER=$MESON_VER" \
    --build-arg "MESON_SHA256=$MESON_SHA256" \
    --build-arg "RUSTUP_VER=$RUSTUP_VER" \
    --build-arg "RUSTUP_SHA256=$RUSTUP_SHA256" \
    --build-arg "RUST_VER=$RUST_VER" \
    --build-arg "BINDGEN_VER=$BINDGEN_VER" \
    --build-arg "CBINDGEN_VER=$CBINDGEN_VER" \
    "$SCRIPT_DIR" > /dev/null

echo "=== mesa: building $arch ==="
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

archive="$TOP/mesa-$(package_version mesa)-$arch.tar.zst"
[ -f "$archive" ] || { echo "mesa: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== mesa: $arch done ==="
