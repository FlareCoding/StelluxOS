#!/bin/sh
# Runs inside the sheen recipe's container: builds the Stellux sysroot, the host Qt's moc, the userland libraries
# Sheen links and then Sheen for ARCH against the Qt kit, and packs it with its drop-in, icon and licenses into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the tree and the Qt kit in /in and the sources in /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="${JOBS:-$(nproc)}"
TREE="/work/tree/$ARCH"
SRC="/work/src"
HOST_QT="/work/qt-host"
ROOT="/work/root/$ARCH"

# The libraries Sheen links, in the order the userland build makes them
SHEEN_LIBS="libstlxtheme libstlx libstlxconf libstlxwin libstlxqpa"

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    step="$(basename "$log" .log)"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "sheen: the $step step failed, its output is in $log inside the stellux-sheen-work volume" >&2
        exit 1
    }
}

# The sysroot every Stellux program links against, built by the tree's own targets and
# kept in the /work volume until the tree's recipe for it changes
build_sysroot() {
    key="$SYSROOT_KEY $MUSL_VER $LLVM_VER"
    if [ "$(cat /work/sysroot/.key 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== sheen: building the Stellux sysroot ==="
    rm -rf /work/sysroot-tree /work/sysroot
    mkdir -p /work/sysroot-tree
    tar -C /work/sysroot-tree -xf /in/tree.tar
    cp "/sources/musl-$MUSL_VER.tar.gz" "/sources/llvm-project-$LLVM_VER.src.tar.xz" /work/sysroot-tree/userland/
    run_logged /work/sysroot.log make -C /work/sysroot-tree NPROC="$JOBS" musl libcxx compiler-rt

    mv /work/sysroot-tree/userland/sysroot /work/sysroot
    rm -rf /work/sysroot-tree
    echo "$key" > /work/sysroot/.key
}

# moc, the one code generator the build runs, from the qtbase source the qt recipe pins, configured as the tree's
# make qt configures its host Qt
build_host_moc() {
    qtbase="$(basename /sources/qtbase-everywhere-src-*.tar.xz .tar.xz)"
    version="${qtbase#qtbase-everywhere-src-}"
    if [ "$(cat "$HOST_QT/VERSION" 2> /dev/null)" = "$version" ]; then
        return
    fi

    echo "=== sheen: building moc $version ==="
    rm -rf /work/qt-host-build "$HOST_QT"
    mkdir -p /work/qt-host-build/build "$HOST_QT/libexec"
    tar -C /work/qt-host-build -xf "/sources/$qtbase.tar.xz"
    cd /work/qt-host-build/build
    run_logged /work/qt-host-build/configure.log "../$qtbase/configure" -static -release -opensource \
        -confirm-license -nomake tests -nomake examples -no-pch -no-dbus -no-glib -no-icu -no-opengl \
        -no-gui -no-widgets -no-fontconfig -no-openssl -no-zstd -no-feature-network
    run_logged /work/qt-host-build/build.log cmake --build . --parallel "$JOBS" --target moc

    cp libexec/moc "$HOST_QT/libexec/moc"
    cd /work
    rm -rf /work/qt-host-build
    echo "$version" > "$HOST_QT/VERSION"
}

# The committed tree with the sysroot, the host moc and the Qt kit where its Makefiles look for them
prepare_tree() {
    rm -rf "$TREE"
    mkdir -p "$TREE/userland/sysroot" "$TREE/userland/toolchain/sdk/$ARCH"
    tar -C "$TREE" -xf /in/tree.tar
    cp -R "/work/sysroot/$ARCH" "$TREE/userland/sysroot/$ARCH"
    ln -s "$HOST_QT" "$TREE/userland/toolchain/qt-host"
    zstd -dc /in/qt.tar.zst | tar -C "$TREE/userland/toolchain/sdk/$ARCH" -xf -

    mkdir -p "$SRC"
    tar -C "$SRC" -xzf /sources/scintilla*.tgz
    tar -C "$SRC" -xzf /sources/lexilla*.tgz
}

build_sheen() {
    echo "=== sheen: building Sheen $SHEEN_VER for $ARCH ==="
    for lib in $SHEEN_LIBS; do
        run_logged "/work/$lib-$ARCH.log" make -C "$TREE/userland/lib/$lib" ARCH="$ARCH" \
            USERLAND_ROOT="$TREE/userland" -j"$JOBS"
    done

    run_logged "/work/sheen-$ARCH.log" make -C "$TREE/userland/apps/sheen" ARCH="$ARCH" \
        USERLAND_ROOT="$TREE/userland" SCINTILLA_DIR="$SRC/scintilla" LEXILLA_DIR="$SRC/lexilla" -j"$JOBS"
}

# The program, its dock launcher and icon, the license texts of Scintilla, Lexilla, the Qt kit and musl, and
# BUILDINFO naming what it was built from
install_package() {
    rm -rf "$ROOT"
    licenses="$ROOT/usr/share/licenses/sheen"
    mkdir -p "$ROOT/usr/bin" "$ROOT/etc/stlxdm/conf.d" "$ROOT/etc/res/icons" "$ROOT/usr/share/doc/sheen" \
        "$licenses/scintilla" "$licenses/lexilla" "$licenses/qt" "$licenses/musl"

    llvm-strip -o "$ROOT/usr/bin/sheen" "$TREE/userland/build/$ARCH/bin/sheen"
    cp /recipe/sheen.conf "$ROOT/etc/stlxdm/conf.d/sheen.conf"
    cp /recipe/sheen-32.png "$ROOT/etc/res/icons/sheen-32.png"

    cp "$SRC/scintilla/License.txt" "$licenses/scintilla/"
    cp "$SRC/lexilla/License.txt" "$licenses/lexilla/"
    cp -R "$TREE/userland/toolchain/sdk/$ARCH/qt/LICENSES/." "$licenses/qt/"
    tar -xzf "/sources/musl-$MUSL_VER.tar.gz" -O "musl-$MUSL_VER/COPYRIGHT" > "$licenses/musl/COPYRIGHT"

    {
        echo "sheen $(package_version sheen) with Scintilla $SCINTILLA_VER and Lexilla $LEXILLA_VER"
        echo "linked against $QT_KIT, archive sha256 $QT_KIT_SHA256"
    } > "$ROOT/usr/share/doc/sheen/BUILDINFO"
}

package() {
    archive="/out/sheen-$(package_version sheen)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T"$JOBS" -q --no-progress -f -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

build_sysroot
build_host_moc
prepare_tree
build_sheen
install_package
package
