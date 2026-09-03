#!/bin/sh
# Builds the Stellux developer packages in Docker and drops the archives
# into userland/toolchain/packages/, which make clean leaves alone. Runs
# on developer machines and on the publish workflow alike.
#
# Usage: packages/build.sh [x86_64] [aarch64]
# With no arguments both architectures are built.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

ARCHES="${*:-x86_64 aarch64}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_CACHE="$TOP/sources"
IMAGE_TAG="stellux-packages"

if ! docker info > /dev/null 2>&1; then
    echo "packages: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

mkdir -p "$TOP" "$SOURCES_CACHE"

# Archive timestamps come from the recipe's last change so identical
# inputs produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages 2>/dev/null || echo 0)"

docker build -q -t "$IMAGE_TAG" \
    --build-arg "ALPINE_IMAGE=$ALPINE_IMAGE" \
    --build-arg "ALPINE_PKGS=$ALPINE_PKGS" \
    "$SCRIPT_DIR" > /dev/null

# One pristine clone at the pinned commit, local-cloned per stage so
# every build tree gets consistent file timestamps from extraction
if [ ! -d "$TOP/mcm" ]; then
    git clone "$MCM_REPO" "$TOP/mcm"
fi
git -C "$TOP/mcm" fetch -q origin "$MCM_COMMIT" 2>/dev/null || true
git -C "$TOP/mcm" checkout -q "$MCM_COMMIT"

for arch in $ARCHES; do
    work="$TOP/work/$arch"
    echo "=== packages: building $arch ==="

    rm -rf "$work"
    mkdir -p "$work"
    cp "$SCRIPT_DIR/versions.sh" "$work/versions.sh"
    cp "$SCRIPT_DIR/container/build.sh" "$work/build.sh"

    for stage in mcm-stage1 mcm-stage2; do
        git clone -q "$TOP/mcm" "$work/$stage"
        patch -s -d "$work/$stage" -p1 < "$SCRIPT_DIR/patches/mcm-prefix.patch"
        cp "$SCRIPT_DIR"/hashes/* "$work/$stage/hashes/"
        mkdir -p "$work/$stage/sources"
        find "$SOURCES_CACHE" -maxdepth 1 -type f \
            -exec cp {} "$work/$stage/sources/" \;
    done

    # Stellux patches apply only to the native stage, since stage 1
    # tools run on the build host, not on the target
    for d in "$SCRIPT_DIR"/patches/*/; do
        name="$(basename "$d")"
        mkdir -p "$work/mcm-stage2/patches/$name"
        cp "$d"/* "$work/mcm-stage2/patches/$name/"
    done

    docker run --rm -v "$work:/work" \
        -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
        "$IMAGE_TAG" "$arch"

    # Keep downloaded tarballs so later builds skip the mirrors
    find "$work/mcm-stage1/sources" -maxdepth 1 -type f \
        -exec cp {} "$SOURCES_CACHE/" \;

    cp "$work"/dist/*.tar.zst "$TOP/"
    echo "=== packages: $arch done ==="
done

echo "packages: archives in userland/toolchain/packages/"
(cd "$TOP" && shasum -a 256 *.tar.zst)
