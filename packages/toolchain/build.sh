#!/bin/sh
# The toolchain recipe: one Docker build of the GCC toolchain for an architecture,
# leaving the archives of the packages asked for in the cache, or of both when none
# is named, so a pinned sibling in the cache is never replaced.
#
# Usage: packages/toolchain/build.sh <x86_64|aarch64> [package...]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [package...]}"
shift
packages="${*:-gcc binutils}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_CACHE="$TOP/sources"
IMAGE_TAG="stellux-toolchain"

if ! docker info > /dev/null 2>&1; then
    echo "toolchain: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The tree's musl patches apply to the musl version the tree builds
actual="$(awk '$1 == "MUSL_VERSION" && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
if [ "$actual" != "$MUSL_VER" ]; then
    echo "toolchain: the Makefile builds musl $actual, packages/toolchain/versions.sh pins $MUSL_VER" >&2
    exit 1
fi

mkdir -p "$TOP" "$SOURCES_CACHE"

# Archive timestamps come from the last change to the recipe or the tree paths it
# builds from, so identical inputs produce identical bytes on every machine
recipe_paths="packages/toolchain $RECIPE_TREE"
# shellcheck disable=SC2086
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- $recipe_paths 2>/dev/null || echo 0)"

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

work="$TOP/work/$arch"
echo "=== toolchain: building $arch ==="

rm -rf "$work"
mkdir -p "$work"
cp "$SCRIPT_DIR/versions.sh" "$work/versions.sh"
cp "$SCRIPT_DIR/container.sh" "$work/build.sh"
cp "$SCRIPT_DIR/fetch.sh" "$work/fetch.sh"

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

# The tree's musl patches and the headers they include, as committed
# shellcheck disable=SC2086
git -C "$REPO_ROOT" archive --format=tar -o "$work/tree.tar" HEAD $RECIPE_TREE
mkdir -p "$work/tree"
tar -C "$work/tree" -xf "$work/tree.tar"

# musl-cross-make applies a source's patches in name order, so the prefix puts
# the tree's last
for p in "$work/tree/userland/musl/patches"/*.patch; do
    cp "$p" "$work/mcm-stage2/patches/musl-$MUSL_VER/stellux-$(basename "$p")"
done

docker run --rm -v "$work:/host" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    "$IMAGE_TAG" "$arch"

# Keep downloaded tarballs so later builds skip the mirrors
find "$work/mcm-stage1/sources" -maxdepth 1 -type f \
    -exec cp {} "$SOURCES_CACHE/" \;

for package in $packages; do
    cp "$work/dist/$package-$(package_version "$package")-$arch.tar.zst" "$TOP/"
done

echo "=== toolchain: $arch done ==="
