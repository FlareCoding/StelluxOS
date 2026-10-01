#!/bin/sh
# Runs inside the build container. Builds the Stellux GCC toolchain for
# one architecture in two stages and packages it:
#   stage 1: a cross toolchain targeting <arch>-linux-musl, runs here
#   stage 2: that cross toolchain rebuilds everything as static
#            Stellux binaries
# Expects the work tree prepared by build.sh mounted at /host: mcm-stage1/,
# mcm-stage2/, python/, and versions.sh. Leaves the package archives in
# /host/dist together with the downloaded sources and the built trees.
#
# STAGE=package skips the builds and repackages the existing trees.
set -eu

ARCH="${1:?usage: build.sh <x86_64|aarch64>}"
TRIPLE="$ARCH-linux-musl"
PREFIX=/usr/gcc
OUT=/work/out
DIST=/work/dist
STAGE="${STAGE:-all}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}"

# Building happens on the container's own filesystem. Bind mounts on some
# hosts misreport directory identity, which makes tar refuse archives
# containing directory symlinks, so the mount only carries inputs and results.
rm -rf /work
cp -a /host /work
cd /work
. /work/versions.sh

# Whatever exists when the run ends goes back to the host, including the
# sources a failed build already downloaded
sync_back() {
    mkdir -p /host/mcm-stage1/sources
    cp -p /work/mcm-stage1/sources/* /host/mcm-stage1/sources/ 2>/dev/null || true
    for d in dist out pkg; do
        rm -rf "/host/$d"
        [ -d "/work/$d" ] && cp -a "/work/$d" "/host/$d"
    done
}
trap sync_back EXIT

JOBS="$(nproc)"

# Standalone binutils package: the toolchain's own tools installed where
# programs expect them. ld.bfd duplicates ld and gprof needs profiling
# support, so neither ships.
BINUTILS_TOOLS="as ld ar ranlib nm objcopy strip readelf objdump strings size addr2line c++filt"

write_config() {
    cat > "$1/config.mak" <<EOF
TARGET = $TRIPLE
GCC_VER = $GCC_VER
BINUTILS_VER = $BINUTILS_VER
MUSL_VER = $MUSL_VER
GNU_SITE = $GNU_SITE
DL_CMD = curl -fL --retry 10 --retry-delay 3 -C - -o
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --enable-languages=c,c++
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-multilib
EOF
}

build_stage1() {
    write_config /work/mcm-stage1
    echo "OUTPUT = /work/stage1" >> /work/mcm-stage1/config.mak

    echo "=== stage 1: cross toolchain ($TRIPLE) ==="
    make -C /work/mcm-stage1 -j"$JOBS"
    make -C /work/mcm-stage1 install
}

build_stage2() {
    write_config /work/mcm-stage2
    cat >> /work/mcm-stage2/config.mak <<EOF
HOST = $TRIPLE
OUTPUT = $OUT
PREFIX = $PREFIX
COMMON_CONFIG += CC="$TRIPLE-gcc -static --static" CXX="$TRIPLE-g++ -static --static"
COMMON_CONFIG += CFLAGS="-g0 -Os" CXXFLAGS="-g0 -Os" LDFLAGS="-s -static --static"
GCC_CONFIG += --disable-lto --disable-host-shared --disable-shared
GCC_CONFIG += --disable-fixincludes
GCC_CONFIG += --with-native-system-header-dir=/include
BINUTILS_CONFIG += --disable-lto
MUSL_CONFIG += --disable-shared
EOF

    echo "=== stage 2: static native toolchain ($TRIPLE) ==="
    export PATH="/work/stage1/bin:$PATH"
    make -C /work/mcm-stage2 -j"$JOBS"
    make -C /work/mcm-stage2 install
}

finish_tree() {
    # Documentation and fixinclude tooling never run on the target, and
    # the kernel interface headers describe a different kernel entirely
    rm -rf "$OUT$PREFIX/share"
    rm -rf "$OUT$PREFIX/libexec/gcc/$TRIPLE/$GCC_VER/install-tools"
    rm -rf "$OUT$PREFIX/include/linux" "$OUT$PREFIX/include/asm" "$OUT$PREFIX/include/asm-generic"

    # The target has no dynamic loader, so the driver defaults to static
    # linking through a specs file it loads from its own lib directory
    cat > "$OUT$PREFIX/lib/gcc/$TRIPLE/$GCC_VER/specs" <<'SPECS'
*self_spec:
+ %{!static:%{!shared:%{!r:-static}}}
SPECS

    # Hardlink groups collapse to zero byte files through the initrd
    # cpio, so every name gets its own copy
    find "$OUT" -type f -links +1 | while read -r f; do
        cp -p "$f" "$f.unlink.tmp" && mv "$f.unlink.tmp" "$f"
    done
}

# CPython is cross-built with the stage 1 toolchain and its musl sysroot.
# Stellux has no dynamic loader, so everything is static and the stdlib
# ships without extension modules, tests, or tooling nobody runs there.
build_python() {
    src=/work/python/Python-$PYTHON_VER
    bld=/work/python/build
    out=/work/pkg/python
    stdlib="$out/usr/lib/python${PYTHON_VER%.*}"

    echo "=== python: cross-building CPython $PYTHON_VER ($TRIPLE) ==="
    rm -rf "$src" "$bld" "$out"
    tar -C /work/python -xf "/work/python/Python-$PYTHON_VER.tar.xz"
    for p in /work/python/*.patch; do
        patch -s -d "$src" -p1 < "$p"
    done
    mkdir -p "$bld/Modules"
    cp /work/python/Modules.Setup.local "$bld/Modules/Setup.local"

    (cd "$bld" && \
        CONFIG_SITE=/work/python/config.site \
        CC="$TRIPLE-gcc" \
        CFLAGS="-std=c11 -O2 -D_GNU_SOURCE -D__STELLUX__ -fno-stack-protector" \
        LDFLAGS="-static" \
        PKG_CONFIG=false \
        ac_cv_file__dev_ptmx=no \
        ac_cv_file__dev_ptc=no \
        "$src/configure" \
            --build="$("$src/config.guess")" \
            --host="$TRIPLE" \
            --prefix=/usr \
            --disable-shared \
            --disable-test-modules \
            --disable-ipv6 \
            --without-ensurepip \
            --with-build-python=python3 > /dev/null)
    make -C "$bld" -j"$JOBS" LINKFORSHARED="" > /dev/null
    make -C "$bld" DESTDIR="$out" altbininstall libinstall > /dev/null

    # Only the interpreter and the parts of the stdlib that run without
    # dynamic loading or a terminal library ship
    rm -rf "$stdlib/lib-dynload" "$stdlib"/config-* "$out/usr/lib/pkgconfig"
    rm -rf "$stdlib/test" "$stdlib/idlelib" "$stdlib/tkinter" "$stdlib/turtledemo" \
           "$stdlib/ensurepip" "$stdlib/venv" "$stdlib/lib2to3"
    find "$out" -name '__pycache__' -type d -prune -exec rm -rf {} +
    find "$out" \( -name '*.so' -o -name '*.a' -o -name '*.o' -o -name '*.pyc' \) -delete
    rm -f "$out"/usr/bin/python*-config "$out"/usr/lib/libpython*
    "$TRIPLE-strip" "$out/usr/bin/python${PYTHON_VER%.*}"
    mkdir -p "$out/bin"
    ln -sf "/usr/bin/python${PYTHON_VER%.*}" "$out/bin/python3"
}

# package <name> <version> <release> <tree>: reproducible archive rooted
# at the tree, so identical inputs yield identical bytes everywhere
package() {
    archive="$DIST/$1-$2-$3-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$4" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T0 -q --no-progress -f -o "$archive"
}

package_all() {
    rm -rf "$DIST" /work/pkg/binutils
    mkdir -p "$DIST" /work/pkg/binutils/usr/bin

    package gcc "$GCC_VER" "$GCC_PKG_REL" "$OUT"

    for t in $BINUTILS_TOOLS; do
        cp -p "$OUT$PREFIX/bin/$t" /work/pkg/binutils/usr/bin/
    done
    package binutils "$BINUTILS_VER" "$BINUTILS_PKG_REL" /work/pkg/binutils

    if [ -d /work/pkg/python ]; then
        package python "$PYTHON_VER" "$PYTHON_PKG_REL" /work/pkg/python
    else
        echo "=== python: no build tree in this work directory, not packaged ==="
    fi

    (cd "$DIST" && sha256sum *.tar.zst > SHA256SUMS && cat SHA256SUMS)
}

if [ "$STAGE" = "all" ]; then
    build_stage1
    build_stage2
    finish_tree
    export PATH="/work/stage1/bin:$PATH"
    build_python
fi
package_all

echo "=== container build done ($TRIPLE) ==="
