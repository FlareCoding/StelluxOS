#!/bin/sh
# Runs inside the qt recipe's container: builds the Stellux sysroot, a host Qt for the
# cross build's code generators, then the static Qt kit for ARCH, and packs it into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the tree in /in and the sources in /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="$(nproc)"
QTBASE="qtbase-everywhere-src-$QT_VER"
QTSVG="qtsvg-everywhere-src-$QT_VER"
SYSROOT="/work/sysroot/$ARCH"
HOST_QT="/work/host-qt"
KIT="/work/kit/$ARCH"
PREFIX="$KIT/qt"
TOOLCHAIN="/work/toolchain-$ARCH.cmake"

# The bundled third-party libraries and no system integration the container has
COMMON_FEATURES="-no-dbus -no-glib -no-icu -no-opengl -no-fontconfig -no-openssl -no-zstd
    -no-feature-vulkan -no-feature-xcb -no-feature-network -no-feature-sql
    -no-feature-printsupport -no-feature-testlib
    -qt-zlib -qt-libpng -qt-libjpeg -qt-freetype -qt-harfbuzz -qt-pcre"

# The kit draws only through the tree's stellux platform plugin, the default
# platform of every program linked against it
TARGET_FEATURES="$COMMON_FEATURES -no-feature-xkbcommon -no-feature-linuxfb
    -no-feature-eglfs -no-feature-vnc -no-feature-directfb -no-feature-evdev
    -no-feature-libinput -no-feature-tslib -no-feature-mtdev
    -no-feature-concurrent -no-feature-xml"

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "qt: $1 failed, its output is in $log inside the stellux-qt-work volume" >&2
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

    echo "=== qt: building the Stellux sysroot ==="
    rm -rf /work/tree /work/sysroot
    mkdir -p /work/tree
    tar -C /work/tree -xf /in/tree.tar
    cp "/sources/musl-$MUSL_VER.tar.gz" "/sources/llvm-project-$LLVM_VER.src.tar.xz" /work/tree/userland/
    make -C /work/tree musl libcxx compiler-rt

    mv /work/tree/userland/sysroot /work/sysroot
    rm -rf /work/tree
    echo "$key" > /work/sysroot/.key
}

# Qt's code generators for the cross build, which run in this container
build_host_qt() {
    if [ "$(cat "$HOST_QT/.key" 2> /dev/null)" = "$QT_VER" ]; then
        return
    fi

    echo "=== qt: building the host Qt ==="
    rm -rf /work/src/host /work/build/host "$HOST_QT"
    mkdir -p /work/src/host /work/build/host
    tar -C /work/src/host -xf "/sources/$QTBASE.tar.xz"

    cd /work/build/host
    # shellcheck disable=SC2086
    run_logged configure.log "/work/src/host/$QTBASE/configure" -prefix "$HOST_QT" -static -release \
        -opensource -confirm-license -nomake tests -nomake examples -no-pch $COMMON_FEATURES
    cmake --build . --parallel "$JOBS"
    cmake --install . > /dev/null
    echo "$QT_VER" > "$HOST_QT/.key"
}

# Compiles for Stellux the way userland/mk/toolchain.mk does and links the way
# userland/mk/cxxapp.mk does, so the kit's objects link into any tree program
write_toolchain() {
    builtins="$SYSROOT/lib/libclang_rt.builtins-$ARCH.a"
    cat > "$TOOLCHAIN" << EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET $ARCH-linux-musl)
set(CMAKE_CXX_COMPILER_TARGET $ARCH-linux-musl)
set(CMAKE_AR $(command -v llvm-ar))
set(CMAKE_RANLIB $(command -v llvm-ranlib))
set(CMAKE_C_FLAGS_INIT "-nostdlibinc -isystem $SYSROOT/include")
set(CMAKE_CXX_FLAGS_INIT "-nostdlibinc -isystem $SYSROOT/include/c++/v1 -isystem $SYSROOT/include")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -fuse-ld=lld -static $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o -L$SYSROOT/lib")
set(CMAKE_C_STANDARD_LIBRARIES "-Wl,--start-group -lc -lm $builtins -Wl,--end-group $SYSROOT/lib/crtn.o")
set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,--start-group -lc++ -lc++abi -lunwind -lc -lm $builtins -Wl,--end-group $SYSROOT/lib/crtn.o")
set(CMAKE_FIND_ROOT_PATH $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
}

build_kit() {
    echo "=== qt: building the $ARCH kit ==="
    rm -rf "/work/src/$ARCH" "/work/build/$ARCH" "$KIT"
    mkdir -p "/work/src/$ARCH" "/work/build/$ARCH/qtbase" "/work/build/$ARCH/qtsvg"
    tar -C "/work/src/$ARCH" -xf "/sources/$QTBASE.tar.xz"
    tar -C "/work/src/$ARCH" -xf "/sources/$QTSVG.tar.xz"
    write_toolchain

    cd "/work/build/$ARCH/qtbase"
    # shellcheck disable=SC2086
    run_logged configure.log "/work/src/$ARCH/$QTBASE/configure" -prefix "$PREFIX" -static -release \
        -opensource -confirm-license -qt-host-path "$HOST_QT" \
        -nomake tests -nomake examples -no-pch $TARGET_FEATURES -- \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DQT_QPA_DEFAULT_PLATFORM=stellux
    cmake --build . --parallel "$JOBS"
    cmake --install . > /dev/null

    cd "/work/build/$ARCH/qtsvg"
    run_logged configure.log "$PREFIX/bin/qt-configure-module" "/work/src/$ARCH/$QTSVG"
    cmake --build . --parallel "$JOBS"
    cmake --install . > /dev/null
}

# What a program using the kit compiles and links with, measured on the probe
# programs and written as share/stellux/qt.mk for the tree's Makefiles
describe_kit() {
    probe="/work/build/$ARCH/probe"
    rm -rf "$probe" "$probe.log"
    run_logged "$probe.log" "$PREFIX/bin/qt-cmake" -S /recipe/probe -B "$probe" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DQT_NO_PRIVATE_MODULE_WARNING=ON
    run_logged "$probe.log" cmake --build "$probe" --parallel "$JOBS"

    mkdir -p "$PREFIX/share/stellux"
    python3 /recipe/qtmk.py "$probe" "$PREFIX" "$QT_VER" "$ARCH" "$PREFIX/share/stellux"
}

# The licenses of Qt and of the third-party code it bundles, which every program
# linked against the kit carries
collect_licenses() {
    for module in "$QTBASE" "$QTSVG"; do
        mkdir -p "$PREFIX/LICENSES/${module%%-*}"
        cp "/work/src/$ARCH/$module"/LICENSES/* "$PREFIX/LICENSES/${module%%-*}/"
    done
    python3 /recipe/attributions.py "/work/src/$ARCH" > "$PREFIX/LICENSES/THIRD-PARTY.txt"
}

# The cross build's scripts point into this container, and a host build of the
# code generators comes from the tree's make qt
trim_kit() {
    rm -rf "$PREFIX/bin" "$PREFIX/libexec" "$PREFIX/doc"
}

package_kit() {
    archive="/out/qt-$QT_VER-$QT_PKG_REL-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$KIT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T0 -q --no-progress -f -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

build_sysroot
build_host_qt
build_kit
describe_kit
collect_licenses
trim_kit
package_kit
