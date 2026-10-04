#!/bin/sh
# Builds and installs a host Qt from the qtbase source the qt package pins. The tree's Qt
# programs run its code generators at build time, and CMake builds against the kit use it
# as their host Qt. Run by make qt.
#
# Usage: scripts/qt-host-tools.sh <install prefix>
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO_ROOT/packages/qt/versions.sh"

prefix="${1:?usage: qt-host-tools.sh <install prefix>}"

# An install without the CMake packages cross builds look up is incomplete
if [ "$(cat "$prefix/VERSION" 2> /dev/null)" = "$QT_VER" ] && [ -d "$prefix/lib/cmake/Qt6WidgetsTools" ]; then
    echo "Qt $QT_VER host build is up to date in $prefix"
    exit 0
fi

sources="$REPO_ROOT/userland/toolchain/packages/sources"
tarball="$sources/qtbase-everywhere-src-$QT_VER.tar.xz"
mkdir -p "$sources"
if [ ! -f "$tarball" ]; then
    echo "Downloading qtbase $QT_VER source (~50MB)..."
    curl -fL --retry 3 -o "$tarball.tmp" "$QT_URL/qtbase-everywhere-src-$QT_VER.tar.xz"
    mv "$tarball.tmp" "$tarball"
fi

echo "$QTBASE_SHA256  $tarball" | shasum -a 256 -c - > /dev/null || {
    echo "qt: $tarball does not match its pinned checksum, delete it to download again" >&2
    exit 1
}

work="$prefix.build"
rm -rf "$work" "$prefix"
mkdir -p "$work/build"
tar -C "$work" -xf "$tarball"
cd "$work/build"

# The features of the qt recipe's host Qt, which the kit's CMake packages were generated against
echo "Building the Qt $QT_VER host build, which takes several minutes..."
"$work/qtbase-everywhere-src-$QT_VER/configure" -prefix "$prefix" -static -release \
    -opensource -confirm-license -nomake tests -nomake examples -no-pch \
    -no-dbus -no-glib -no-icu -no-opengl -no-fontconfig -no-openssl -no-zstd \
    -no-feature-vulkan -no-feature-xcb -no-feature-network -no-feature-sql \
    -no-feature-printsupport -no-feature-testlib \
    -qt-zlib -qt-libpng -qt-libjpeg -qt-freetype -qt-harfbuzz -qt-pcre > "$work/configure.log" 2>&1 || {
    tail -40 "$work/configure.log"
    echo "qt: configuring the host build failed, see $work/configure.log" >&2
    exit 1
}

cmake --build . > "$work/build.log" 2>&1 || {
    tail -40 "$work/build.log"
    echo "qt: the host build failed, see $work/build.log" >&2
    exit 1
}

cmake --install . > /dev/null
echo "$QT_VER" > "$prefix/VERSION"
rm -rf "$work"
echo "Qt $QT_VER host build installed in $prefix"
