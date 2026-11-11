#!/bin/sh
# The sheen recipe: Sheen for one architecture, built in a container from the committed tree and the pinned Scintilla
# and Lexilla against the Qt kit the tree resolves, left in the cache as sheen-<version>-<arch>.tar.zst.
#
# Usage: packages/sheen/build.sh <x86_64|aarch64> [sheen]
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$SCRIPT_DIR/versions.sh"

arch="${1:?usage: build.sh <x86_64|aarch64> [sheen]}"
TOP="$REPO_ROOT/userland/toolchain/packages"
SOURCES_DIR="$TOP/sources/sheen"
IMAGE_TAG="stellux-sheen"

# Keeps the sysroot and the host Qt code generators between runs and architectures
VOLUME="stellux-sheen-work"

# The tree's sysroot targets expect an x86_64 Debian host, emulated elsewhere
PLATFORM="linux/amd64"

case "$arch" in
x86_64|aarch64) ;;
*) echo "sheen: unsupported architecture $arch" >&2; exit 1 ;;
esac

if ! docker info > /dev/null 2>&1; then
    echo "sheen: docker daemon unavailable, start Docker first" >&2
    exit 1
fi

# The sysroot comes from the tree's own targets, which must build the versions pinned here
check_makefile_pin() {
    actual="$(awk -v v="$1" '$1 == v && $2 == ":=" { print $3 }' "$REPO_ROOT/Makefile")"
    if [ "$actual" != "$2" ]; then
        echo "sheen: the Makefile builds $1 $actual, packages/sheen/versions.sh pins $2" >&2
        exit 1
    fi
}

check_makefile_pin MUSL_VERSION "$MUSL_VER"
check_makefile_pin LLVM_VERSION "$LLVM_VER"

# Downloads a source into the cache once, or checks the copy there, against its pin
fetch_source() {
    file="$SOURCES_DIR/$(basename "$1")"
    if [ ! -f "$file" ]; then
        echo "=== sheen: downloading $(basename "$1") ==="
        curl -fsSL --retry 3 -o "$file.tmp" "$1"
        mv "$file.tmp" "$file"
    fi

    echo "$2  $file" | shasum -a 256 -c - > /dev/null || {
        echo "sheen: $file does not match its pinned checksum, delete it to download again" >&2
        exit 1
    }
}

# The host Qt's code generators build from the qtbase source the qt recipe pins
QT_PINS="$(. "$REPO_ROOT/packages/qt/versions.sh" && echo "$QT_URL/qtbase-everywhere-src-$QT_VER.tar.xz $QTBASE_SHA256")"

mkdir -p "$TOP" "$SOURCES_DIR"
fetch_source "$SCINTILLA_URL" "$SCINTILLA_SHA256"
fetch_source "$LEXILLA_URL" "$LEXILLA_SHA256"
fetch_source "$MUSL_URL" "$MUSL_SHA256"
fetch_source "$LLVM_URL" "$LLVM_SHA256"
# shellcheck disable=SC2086
fetch_source $QT_PINS

# Sheen links the Qt kit the tree's build resolves for the architecture: an unpublished local build of the
# version packages/qt names when the cache holds one, the pinned release otherwise
kit_version="$(make -s -C "$REPO_ROOT" packages-list ARCH="$arch" PACKAGES="none qt" | awk '$1 == "qt" { print $3 }')"
if [ -z "$kit_version" ]; then
    echo "sheen: no Qt kit for $arch, run make packages-build PACKAGES=\"none qt\" ARCHES=$arch" >&2
    exit 1
fi
(
    SCRIPT_DIR="$REPO_ROOT/packages"
    . "$SCRIPT_DIR/common.sh"
    named_version="$(current_version qt)"
    [ "$kit_version" = "$named_version" ] ||
        echo "sheen: no qt $named_version for $arch in the cache, Sheen links the pinned $kit_version" >&2
    [ "$kit_version" != "$(pinned_version qt "$arch")" ] || fetch_pinned qt "$arch" "$TOP"
)

# Archive timestamps come from the last change to the recipe or to Sheen, so identical
# inputs produce identical bytes on every machine
SOURCE_DATE_EPOCH="$(git -C "$REPO_ROOT" log -1 --format=%ct -- packages/sheen userland/apps/sheen 2>/dev/null || true)"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# The committed sysroot recipe, so the volume's sysroot is rebuilt when it changes
SYSROOT_KEY="$(git -C "$REPO_ROOT" rev-parse HEAD:Makefile HEAD:scripts/host.mk | tr '\n' ' ')"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
git -C "$REPO_ROOT" archive --format=tar -o "$stage/tree.tar" HEAD
cp "$TOP/qt-$kit_version-$arch.tar.zst" "$stage/qt.tar.zst"
kit_sha256="$(shasum -a 256 "$stage/qt.tar.zst" | cut -d ' ' -f 1)"

docker build -q --platform "$PLATFORM" -t "$IMAGE_TAG" \
    --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" \
    --build-arg "UBUNTU_PKGS=$UBUNTU_PKGS" \
    "$SCRIPT_DIR" > /dev/null

echo "=== sheen: building $arch against qt $kit_version ==="
docker run --rm --platform "$PLATFORM" \
    -v "$VOLUME:/work" \
    -v "$SCRIPT_DIR:/recipe:ro" \
    -v "$SOURCES_DIR:/sources:ro" \
    -v "$stage:/in:ro" \
    -v "$TOP:/out" \
    -e "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" \
    -e "SYSROOT_KEY=$SYSROOT_KEY" \
    -e "QT_KIT=qt-$kit_version-$arch" \
    -e "QT_KIT_SHA256=$kit_sha256" \
    -e "OUT_OWNER=$(id -u):$(id -g)" \
    -e JOBS \
    "$IMAGE_TAG" "$arch"

archive="$TOP/sheen-$(package_version sheen)-$arch.tar.zst"
[ -f "$archive" ] || { echo "sheen: the container left no $(basename "$archive")" >&2; exit 1; }
echo "=== sheen: $arch done ==="
