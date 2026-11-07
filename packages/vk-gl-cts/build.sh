#!/bin/sh
# The vk-gl-cts recipe: Khronos's Vulkan conformance tests for one architecture, deqp-vk linked
# against the mesa kit and cross-built in a container against the Stellux sysroot, left in the
# cache as vk-gl-cts-<version>-<arch>.tar.zst. JOBS caps the container's parallel jobs.
#
# Usage: packages/vk-gl-cts/build.sh <x86_64|aarch64> [vk-gl-cts]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [vk-gl-cts]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
IMAGE_TAG="stellux-vk-gl-cts"

# Below the top of the sources cache, whose files the toolchain recipe copies into its build
SOURCES_DIR="$TOP/sources/vk-gl-cts"

# Keeps the sysroot for later runs, and each architecture's last build for reading its logs
VOLUME="stellux-vk-gl-cts-work"

# The tree's sysroot targets expect an x86_64 Debian host, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "vk-gl-cts: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "vk-gl-cts: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The sysroot comes from the tree's own targets, which must build the versions pinned here
check_makefile_pin() {
    actual="$(awk -v v="$1" '$1 == v && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
    if [ "$actual" != "$2" ]; then
        echo "vk-gl-cts: the Makefile builds $1 $actual, packages/vk-gl-cts/versions.sh pins $2" >&2
        exit 1
    fi
}

check_makefile_pin MUSL_VERSION "$MUSL_VER"
check_makefile_pin LLVM_VERSION "$LLVM_VER"

# Downloads a source into the cache as the named file once, or checks the copy there, against
# its pin
fetch_source() {
    file="$SOURCES_DIR/$3"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 512 -c - > /dev/null || {
            echo "vk-gl-cts: $file does not match its pinned checksum, delete it to refetch" >&2
            exit 1
        }
        return 0
    fi

    echo "=== vk-gl-cts: downloading $3 ==="
    mkdir -p "$(dirname "$file")"
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 512 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "vk-gl-cts: downloaded $3 does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$VK_GL_CTS_URL" "$VK_GL_CTS_SHA512" "vk-gl-cts-$VK_GL_CTS_VER.tar.gz"
while read -r commit sha512; do
    [ -z "$commit" ] || fetch_source "$VK_GL_CTS_COMMIT_URL/$commit.patch" "$sha512" "backports/$commit.patch"
done <<EOF
$VK_GL_CTS_BACKPORTS
EOF
while read -r name url sha512; do
    [ -z "$name" ] || fetch_source "$url" "$sha512" "$name/${url##*/}"
done <<EOF
$VK_GL_CTS_EXTERNALS
EOF
fetch_source "$MUSL_URL" "$MUSL_SHA512" "musl-$MUSL_VER.tar.gz"
fetch_source "$LLVM_URL" "$LLVM_SHA512" "llvm-project-$LLVM_VER.src.tar.xz"

# deqp-vk links the mesa kit the tree's build resolves for the architecture: an unpublished local
# build of the version packages/mesa names when the cache holds one, the pinned release otherwise
kit_version="$(make -s -C "$REPO_ROOT" packages-list ARCH="$arch" PACKAGES="none mesa" |
    awk '$1 == "mesa" { print $3 }')"
if [ -z "$kit_version" ]; then
    echo "vk-gl-cts: no mesa kit for $arch, run make packages-build PACKAGES=\"none mesa\" ARCHES=$arch" >&2
    exit 1
fi
(
    SCRIPT_DIR="$REPO_ROOT/packages"
    . "$SCRIPT_DIR/common.sh"
    named_version="$(current_version mesa)"
    [ "$kit_version" = "$named_version" ] ||
        echo "vk-gl-cts: no mesa $named_version for $arch in the cache, deqp-vk links the pinned $kit_version" >&2
    [ "$kit_version" != "$(pinned_version mesa "$arch")" ] || fetch_pinned mesa "$arch" "$TOP"
)

# Archive timestamps come from the recipe's last change, so identical inputs
# produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/vk-gl-cts 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# The committed sysroot recipe, so the volume's sysroot is rebuilt when it changes
SYSROOT_KEY="$(git -C "$REPO_ROOT" rev-parse HEAD:Makefile HEAD:scripts/host.mk | tr '\n' ' ')"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD
cp "$TOP/mesa-$kit_version-$arch.tar.zst" "$stage/mesa.tar.zst"
kit_sha256="$(shasum -a 256 "$stage/mesa.tar.zst" | cut -d ' ' -f 1)"

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== vk-gl-cts: building $arch against mesa $kit_version ==="
docker run --rm --platform "$PLATFORM" \
    -v "$VOLUME:/work" \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$stage:/in:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "SYSROOT_KEY=$SYSROOT_KEY" \
    -e "MESA_KIT=mesa-$kit_version-$arch" \
    -e "MESA_KIT_SHA256=$kit_sha256" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    -e JOBS \
    "$IMAGE_TAG" "$arch"

archive="$TOP/vk-gl-cts-$(package_version vk-gl-cts)-$arch.tar.zst"
[ -f "$archive" ] || { echo "vk-gl-cts: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== vk-gl-cts: $arch done ==="
