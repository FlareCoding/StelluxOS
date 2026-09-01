#!/bin/sh
# Builds the static on-target GCC toolchain artifacts using Docker.
# Produces build/gcc-toolchain/stellux-gcc-<ver>-<arch>.tar.xz plus a
# sha256 file for each requested architecture.
#
# Usage: scripts/gcc-toolchain/build.sh [x86_64] [aarch64]
# With no arguments both architectures are built.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

ARCHES="${*:-x86_64 aarch64}"
TOP="$REPO_ROOT/build/gcc-toolchain"
SOURCES_CACHE="$TOP/sources-cache"
PATCH_ROOT="$REPO_ROOT/userland/apps/gcc/patches"

if ! docker info > /dev/null 2>&1; then
    echo "gcc-toolchain: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

mkdir -p "$TOP" "$SOURCES_CACHE"

# One pristine clone at the pinned commit, local-cloned per stage so
# every build tree gets consistent file timestamps from extraction
if [ ! -d "$TOP/mcm-pristine" ]; then
    git clone "$MCM_REPO" "$TOP/mcm-pristine"
fi
git -C "$TOP/mcm-pristine" fetch -q origin "$MCM_COMMIT" 2>/dev/null || true
git -C "$TOP/mcm-pristine" checkout -q "$MCM_COMMIT"

for arch in $ARCHES; do
    work="$TOP/$arch"
    echo "=== gcc-toolchain: building $arch ==="

    rm -rf "$work"
    mkdir -p "$work"
    cp "$SCRIPT_DIR/versions.sh" "$work/versions.sh"
    cp "$SCRIPT_DIR/container-build.sh" "$work/container-build.sh"

    for stage in mcm-stage1 mcm-stage2; do
        git clone -q "$TOP/mcm-pristine" "$work/$stage"
        mkdir -p "$work/$stage/sources"
        find "$SOURCES_CACHE" -maxdepth 1 -type f \
            -exec cp {} "$work/$stage/sources/" \;
    done

    # Stellux patches apply only to the native stage, since stage 1
    # tools run on the build host, not on the target
    if [ -d "$PATCH_ROOT" ]; then
        for d in "$PATCH_ROOT"/*/; do
            [ -d "$d" ] || continue
            name="$(basename "$d")"
            mkdir -p "$work/mcm-stage2/patches/$name"
            cp "$d"/* "$work/mcm-stage2/patches/$name/"
        done
    fi

    docker run --rm --platform linux/arm64 \
        -v "$work:/work" -w /work "$ALPINE_IMAGE" \
        sh /work/container-build.sh "$arch"

    # Keep downloaded tarballs so later builds skip the mirrors
    find "$work/mcm-stage1/sources" -maxdepth 1 -type f \
        -exec cp {} "$SOURCES_CACHE/" \;

    artifact="$TOP/stellux-gcc-$GCC_VER-$arch.tar.xz"
    echo "=== gcc-toolchain: packaging $(basename "$artifact") ==="
    tar -C "$work/out" -cJf "$artifact" .
    (cd "$TOP" && shasum -a 256 "$(basename "$artifact")" > "$artifact.sha256")

    echo "=== gcc-toolchain: $arch done ==="
done

echo "gcc-toolchain: artifacts in build/gcc-toolchain/"
