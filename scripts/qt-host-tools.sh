#!/bin/sh
# Builds Qt's code generators for this machine from the qtbase source the qt package pins,
# which the tree's Qt libraries and apps run at build time. Run by make qt.
#
# Usage: scripts/qt-host-tools.sh <install dir>
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$REPO_ROOT/packages/qt/versions.sh"

dest="${1:?usage: qt-host-tools.sh <install dir>}"
if [ "$(cat "$dest/VERSION" 2> /dev/null)" = "$QT_VER" ]; then
    echo "Qt $QT_VER code generators are up to date in $dest"
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

work="$dest/build"
rm -rf "$work" "$dest/bin" "$dest/VERSION"
mkdir -p "$work" "$dest/bin"
tar -C "$work" -xf "$tarball"

# Only QtCore and the code generators on it, built against the host's own C library
echo "Building the Qt $QT_VER code generators..."
cmake -S "$work/qtbase-everywhere-src-$QT_VER" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF \
    -DFEATURE_gui=OFF -DFEATURE_widgets=OFF -DFEATURE_network=OFF -DFEATURE_sql=OFF \
    -DFEATURE_testlib=OFF -DFEATURE_dbus=OFF -DFEATURE_concurrent=OFF -DFEATURE_xml=OFF \
    -DFEATURE_printsupport=OFF -DFEATURE_icu=OFF -DFEATURE_glib=OFF > "$work/configure.log" 2>&1 || {
    tail -40 "$work/configure.log"
    echo "qt: configuring the code generators failed, see $work/configure.log" >&2
    exit 1
}
cmake --build "$work/build" --target moc rcc

for tool in moc rcc; do
    cp "$(find "$work/build" -type f -name "$tool" -perm -u+x | head -1)" "$dest/bin/$tool"
done

echo "$QT_VER" > "$dest/VERSION"
rm -rf "$work"
echo "Qt $QT_VER code generators installed in $dest/bin"
