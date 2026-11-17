#!/bin/sh
# The xtensa-esp32-elf recipe: Espressif's binutils and GCC for the ESP32 as static Stellux programs for one
# architecture, with the ESP32-side libraries from Espressif's release, built in a container and left in the cache
# as xtensa-esp32-elf-<version>-<arch>.tar.zst. JOBS caps the container's parallel jobs.
#
# Usage: packages/xtensa-esp32-elf/build.sh <x86_64|aarch64> [xtensa-esp32-elf]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [xtensa-esp32-elf]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_DIR="$TOP/sources/xtensa-esp32-elf"
IMAGE_TAG="stellux-xtensa-esp32-elf"

# Keeps the compiler for Stellux with its downloads, the container's tools and the unpacked release between runs
VOLUME="stellux-xtensa-esp32-elf-work"

# GCC records the machine it was built on, so the container is x86_64 everywhere, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "xtensa-esp32-elf: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "xtensa-esp32-elf: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The tree's musl patches apply to the musl version the tree builds
actual="$(awk '$1 == "MUSL_VERSION" && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
if [ "$actual" != "$MUSL_VER" ]; then
    echo "xtensa-esp32-elf: the Makefile builds musl $actual, packages/xtensa-esp32-elf/versions.sh pins $MUSL_VER" >&2
    exit 1
fi

# Downloads a source into the cache as the named file once, or checks the copy there, against its pin
fetch_source() {
    file="$SOURCES_DIR/$3"
    if [ -f "$file" ]; then
        echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
            echo "xtensa-esp32-elf: $file does not match its pinned checksum, delete it to download again" >&2
            exit 1
        }
        return 0
    fi

    echo "=== xtensa-esp32-elf: downloading $3 ==="
    curl -fsSL --retry 3 -o "$file.tmp" "$1"
    echo "$2  $file.tmp" | shasum -a 256 -c - > /dev/null || {
        rm -f "$file.tmp"
        echo "xtensa-esp32-elf: downloaded $3 does not match its pinned checksum" >&2
        exit 1
    }
    mv "$file.tmp" "$file"
}

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$GCC_URL" "$GCC_SHA256" "gcc-$ESP_RELEASE.tar.gz"
fetch_source "$BINUTILS_URL" "$BINUTILS_SHA256" "binutils-$BINUTILS_TAG.tar.gz"
fetch_source "$CTNG_URL" "$CTNG_SHA256" "$(basename "$CTNG_URL")"
fetch_source "$RELEASE_URL" "$RELEASE_SHA256" "$(basename "$RELEASE_URL")"
fetch_source "$GNU_SITE/gmp/$GMP_FILE" "$GMP_SHA256" "$GMP_FILE"
fetch_source "$GNU_SITE/mpfr/$MPFR_FILE" "$MPFR_SHA256" "$MPFR_FILE"
fetch_source "$GNU_SITE/mpc/$MPC_FILE" "$MPC_SHA256" "$MPC_FILE"
fetch_source "$MCM_URL" "$MCM_SHA256" "musl-cross-make-$MCM_COMMIT.tar.gz"

# Archive timestamps come from the last change to the recipe or to the tree paths it builds from, so
# identical inputs produce identical bytes on every machine
recipe_paths="packages/xtensa-esp32-elf $RECIPE_TREE"
# shellcheck disable=SC2086
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- $recipe_paths 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# The musl patches and the headers they include, as committed
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
# shellcheck disable=SC2086
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD $RECIPE_TREE

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "ALPINE_IMAGE=$ALPINE_IMAGE" \
    --build-arg "ALPINE_PKGS=$ALPINE_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== xtensa-esp32-elf: building $arch ==="
docker run --rm --platform "$PLATFORM" \
    -v "$VOLUME:/work" \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$stage:/in:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    -e JOBS \
    "$IMAGE_TAG" "$arch"

archive="$TOP/xtensa-esp32-elf-$(package_version xtensa-esp32-elf)-$arch.tar.zst"
[ -f "$archive" ] || { echo "xtensa-esp32-elf: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== xtensa-esp32-elf: $arch done ==="
