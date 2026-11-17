#!/bin/sh
# Runs inside the xtensa-esp32-elf recipe's container: builds Espressif's binutils and GCC for the ESP32 as static
# Stellux programs for ARCH, places the ESP32-side libraries of Espressif's release beside them, and packs the
# package into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the recipe in /recipe, the sources in /sources and the tree paths the
# recipe builds from in /in
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="${JOBS:-$(nproc)}"
BUILD="$(gcc -dumpmachine)"
HOST="$ARCH-linux-musl"
CROSS="/work/cross/$ARCH"
CONTAINER_TOOLS="/work/container-tools"
RELEASE="/work/release"
TREE="/work/tree"
SRC="/work/src"
OBJ="/work/obj/$ARCH"
LOGS="/work/logs/$ARCH"
ROOT="/work/root/$ARCH"
SYSROOT="$ROOT$PREFIX/$TARGET"
GCC_LIB="$ROOT$PREFIX/lib/gcc/$TARGET/$GCC_VER"
LICENSES="$ROOT/usr/share/licenses/xtensa-esp32-elf"

# binutils without the parts of the binutils-gdb tree the package does not ship, and without plugins, which static
# programs cannot load, or zstd, so they cannot read or write zstd-compressed sections
BINUTILS_OPTIONS="--target=$TARGET --disable-nls --disable-werror --disable-gdb --disable-gdbserver --disable-sim
    --disable-gprofng --disable-readline --disable-libdecnumber --disable-plugins --without-zstd --without-debuginfod
    --without-msgpack"

# Espressif's options wherever code generation or the driver depend on them, except plugins, which static programs
# cannot load, LTO and isl, which no Arduino-ESP32 build uses, and the version string, which names Espressif's build
GCC_OPTIONS="--target=$TARGET --with-newlib --enable-languages=c,c++ --enable-threads=posix --disable-shared
    --disable-__cxa_atexit --disable-libssp --disable-libstdcxx-verbose --enable-libstdcxx-time=yes --disable-plugin
    --disable-lto --disable-nls --without-isl --without-zstd"

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    step="$(basename "$log" .log)"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "xtensa-esp32-elf: the $step step failed, its output is in $log" \
            "in the stellux-xtensa-esp32-elf-work volume" >&2
        exit 1
    }
}

# A configure run for static Stellux programs, built by the compiler for Stellux
configure_for_stellux() {
    "$@" --build="$BUILD" --host="$HOST" --prefix="$PREFIX" \
        CC="$HOST-gcc -static --static" CXX="$HOST-g++ -static --static" \
        CFLAGS="-g0 -O2" CXXFLAGS="-g0 -O2" LDFLAGS="-s -static --static"
}

# The compiler for Stellux, its musl built with the tree's patches and the libstlx headers they include, as the tree
# builds its own musl. Kept in the volume while musl-cross-make, its config.mak and the staged tree files stay the same.
build_cross() {
    rm -rf "$TREE"
    mkdir -p "$TREE"
    tar -C "$TREE" -xf /in/tree.tar
    config="TARGET = $HOST
GCC_VER = $CROSS_GCC_VER
MUSL_VER = $MUSL_VER
GNU_SITE = $GNU_SITE
OUTPUT = $CROSS
DL_CMD = sh /recipe/fetch.sh $GNU_SITE
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --enable-languages=c,c++
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-multilib
MUSL_CONFIG += CFLAGS=-I$TREE/$STLX_HEADERS"
    tree_sha256="$(cd "$TREE" && find . -type f | sort | xargs sha256sum | sha256sum | cut -d ' ' -f 1)"
    config_sha256="$(echo "$config" | sha256sum | cut -d ' ' -f 1)"
    key="$MCM_SHA256 $config_sha256 $tree_sha256"
    if [ "$(cat "$CROSS/.key" 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== xtensa-esp32-elf: building the compiler for Stellux ($HOST) ==="
    mcm="/work/mcm/$ARCH"
    rm -rf "$CROSS" "$mcm"
    mkdir -p "$mcm" /work/mcm-sources
    tar -C "$mcm" --strip-components=1 -xzf "/sources/musl-cross-make-$MCM_COMMIT.tar.gz"

    # musl-cross-make downloads only into its own sources directory, which the volume keeps for both architectures
    ln -s /work/mcm-sources "$mcm/sources"

    # musl-cross-make applies a source's patches in name order, so the prefix puts the tree's last
    for p in "$TREE/userland/musl/patches"/*.patch; do
        cp "$p" "$mcm/patches/musl-$MUSL_VER/stellux-$(basename "$p")"
    done

    echo "$config" > "$mcm/config.mak"
    run_logged "$LOGS/cross.log" make -C "$mcm" -j"$JOBS"
    run_logged "$LOGS/cross-install.log" make -C "$mcm" install
    rm -rf "$mcm"
    echo "$key" > "$CROSS/.key"
}

# Espressif's GCC and binutils with the ESP32's core configuration from crosstool-NG's overlay, and GCC with its esp32
# library variants (multilibs) and with GMP, MPFR and MPC in its tree, built for each machine GCC runs on
unpack_sources() {
    rm -rf "$SRC"
    mkdir -p "$SRC/gcc" "$SRC/binutils" "$SRC/ctng"
    tar -C "$SRC/gcc" --strip-components=1 -xzf "/sources/gcc-$ESP_RELEASE.tar.gz"
    tar -C "$SRC/binutils" --strip-components=1 -xzf "/sources/binutils-$BINUTILS_TAG.tar.gz"
    tar -C "$SRC/ctng" --strip-components=1 -xJf "/sources/$(basename "$CTNG_URL")"
    cp -R "$SRC/ctng/overlays/xtensa_esp32/gcc/." "$SRC/gcc/"
    cp -R "$SRC/ctng/overlays/xtensa_esp32/binutils/." "$SRC/binutils/"
    patch -s -d "$SRC/gcc" -p1 < /recipe/patches/gcc-esp32-multilib.patch

    for lib in "gmp $GMP_FILE" "mpfr $MPFR_FILE" "mpc $MPC_FILE"; do
        dir="$SRC/gcc/${lib% *}"
        mkdir -p "$dir"
        tar -C "$dir" --strip-components=1 -xf "/sources/${lib#* }"
    done
}

# The ESP32-side files of Espressif's release where a compiler for xtensa-esp32-elf looks: each esp32/ variant
# directory of the multi-chip release becomes the default, with its psram/ and no-rtti/ variants under it. Placed
# before the Stellux GCC builds, so its build finds newlib's headers where the installed compiler will.
place_esp32_side() {
    if [ "$(cat "$RELEASE/.key" 2> /dev/null)" != "$RELEASE_SHA256" ]; then
        echo "=== xtensa-esp32-elf: unpacking Espressif's release ==="
        rm -rf "$RELEASE"
        mkdir -p "$RELEASE"
        # Unpacked whole, since tar extracts a hardlink only with its target and the trees taken link to each other
        tar -C "$RELEASE" --strip-components=1 -xJf "/sources/$(basename "$RELEASE_URL")"
        echo "$RELEASE_SHA256" > "$RELEASE/.key"
    fi

    rm -rf "$ROOT"
    mkdir -p "$SYSROOT/include" "$SYSROOT/lib" "$GCC_LIB/include"
    cp -R "$RELEASE/xtensa-esp-elf/include/." "$SYSROOT/include/"
    cxx="$SYSROOT/include/c++/$GCC_VER"
    mv "$cxx/xtensa-esp-elf/esp32" "$cxx/$TARGET"
    rm -rf "$cxx/xtensa-esp-elf"
    cp -R "$RELEASE/xtensa-esp-elf/lib/esp32/." "$SYSROOT/lib/"

    release_gcc="$RELEASE/lib/gcc/xtensa-esp-elf/$GCC_VER"
    cp -R "$release_gcc/esp32/." "$GCC_LIB/"
    # libgcc installs these two headers beside GCC's own
    cp "$release_gcc/include/unwind.h" "$release_gcc/include/gcov.h" "$GCC_LIB/include/"
}

# binutils and GCC's compiler programs for the container: building GCC to run on Stellux runs an assembler and
# compiler for the ESP32 in the container, to probe the assembler and dump the specs. Kept in the volume while the
# sources, the multilib patch and the options stay the same.
build_container_tools() {
    patch_sha256="$(sha256sum /recipe/patches/gcc-esp32-multilib.patch | cut -d ' ' -f 1)"
    options_sha256="$(echo "$BINUTILS_OPTIONS $GCC_OPTIONS" | sha256sum | cut -d ' ' -f 1)"
    key="$GCC_SHA256 $BINUTILS_SHA256 $CTNG_SHA256 $GMP_SHA256 $MPFR_SHA256 $MPC_SHA256 $patch_sha256 $options_sha256"
    if [ "$(cat "$CONTAINER_TOOLS/.key" 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== xtensa-esp32-elf: building binutils and GCC for the container ==="
    obj="/work/obj/container-tools"
    rm -rf "$CONTAINER_TOOLS" "$obj"
    mkdir -p "$obj/binutils" "$obj/gcc"

    cd "$obj/binutils"
    # shellcheck disable=SC2086
    run_logged "$LOGS/container-binutils-configure.log" "$SRC/binutils/configure" --prefix="$CONTAINER_TOOLS" \
        $BINUTILS_OPTIONS
    run_logged "$LOGS/container-binutils-make.log" make -j"$JOBS" MAKEINFO=true
    run_logged "$LOGS/container-binutils-install.log" make install MAKEINFO=true

    cd "$obj/gcc"
    # shellcheck disable=SC2086
    run_logged "$LOGS/container-gcc-configure.log" "$SRC/gcc/configure" --prefix="$CONTAINER_TOOLS" $GCC_OPTIONS
    run_logged "$LOGS/container-gcc-make.log" make -j"$JOBS" all-gcc MAKEINFO=true
    run_logged "$LOGS/container-gcc-install.log" make install-gcc MAKEINFO=true

    cd /work
    rm -rf "$obj"
    echo "$key" > "$CONTAINER_TOOLS/.key"
}

# binutils and GCC to run on Stellux, with the sysroot Espressif's release uses under the prefix
build_stellux_tools() {
    echo "=== xtensa-esp32-elf: building binutils and GCC to run on $HOST ==="
    rm -rf "$OBJ"
    mkdir -p "$OBJ/binutils" "$OBJ/gcc"

    cd "$OBJ/binutils"
    # shellcheck disable=SC2086
    run_logged "$LOGS/binutils-configure.log" configure_for_stellux "$SRC/binutils/configure" \
        --with-sysroot="$PREFIX/$TARGET" --disable-shared $BINUTILS_OPTIONS
    run_logged "$LOGS/binutils-make.log" make -j"$JOBS" MAKEINFO=true
    run_logged "$LOGS/binutils-install.log" make install DESTDIR="$ROOT" MAKEINFO=true

    cd "$OBJ/gcc"
    # shellcheck disable=SC2086
    run_logged "$LOGS/gcc-configure.log" configure_for_stellux "$SRC/gcc/configure" \
        --with-sysroot="$PREFIX/$TARGET" --with-build-sysroot="$SYSROOT" --with-native-system-header-dir=/include \
        $GCC_OPTIONS
    run_logged "$LOGS/gcc-make.log" make -j"$JOBS" all-gcc MAKEINFO=true
    run_logged "$LOGS/gcc-install.log" make install-gcc DESTDIR="$ROOT" MAKEINFO=true

    cd /work
    rm -rf "$OBJ"
}

# What the package leaves out: documentation, fixincludes tooling, the LTO programs of a GCC without LTO, an ld
# plugin nothing loads, the empty include directory, ld.bfd (a copy of ld) and libstdc++'s gdb scripts
finish_tree() {
    p="$ROOT$PREFIX"
    libexec="$p/libexec/gcc/$TARGET/$GCC_VER"
    rm -rf "$p/share" "$libexec/install-tools" "$GCC_LIB/install-tools" "$p/lib/bfd-plugins"
    rmdir "$p/include"
    # gcc-ar, gcc-nm and gcc-ranlib need the LTO plugin loader this static build leaves out, so builds call ar
    rm -f "$p/bin/$TARGET-gcc-ar" "$p/bin/$TARGET-gcc-nm" "$p/bin/$TARGET-gcc-ranlib" "$libexec/lto-wrapper"
    rm -f "$p/bin/$TARGET-ld.bfd" "$p/$TARGET/bin/ld.bfd"
    find "$p" -name '*-gdb.py' -delete

    # stlxpkg refuses hardlinks and the initrd's cpio leaves all but one name of a group empty, so each gets a copy
    find "$ROOT" -type f -links +1 | while read -r f; do
        cp -p "$f" "$f.unlink.tmp" && mv "$f.unlink.tmp" "$f"
    done

    mkdir -p "$ROOT$(dirname "$ARDUINO_TOOL")"
    ln -s "$PREFIX" "$ROOT$ARDUINO_TOOL"
}

# The license texts of the sources the programs build from and of the code compiled into them, newlib's for the
# libraries the package carries, and a NOTICE saying which terms cover which files
collect_licenses() {
    for dir in gcc binutils gmp mpfr mpc zlib libbacktrace libcody musl; do
        mkdir -p "$LICENSES/$dir"
    done
    cp "$SRC/gcc"/COPYING* "$LICENSES/gcc/"
    cp "$SRC/binutils"/COPYING* "$LICENSES/binutils/"
    cp "$SRC/gcc/gmp"/COPYING* "$LICENSES/gmp/"
    cp "$SRC/gcc/mpfr"/COPYING* "$LICENSES/mpfr/"
    cp "$SRC/gcc/mpc"/COPYING* "$LICENSES/mpc/"
    cp "$SRC/gcc/zlib/LICENSE" "$LICENSES/zlib/"
    cp "$SRC/gcc/libcody/LICENSE" "$LICENSES/libcody/"
    sed -n '1,/\*\//p' "$SRC/gcc/libbacktrace/backtrace.h" > "$LICENSES/libbacktrace/LICENSE"
    tar -xzf "/work/mcm-sources/musl-$MUSL_VER.tar.gz" -O "musl-$MUSL_VER/COPYRIGHT" > "$LICENSES/musl/COPYRIGHT"
    cp -R "$RELEASE/share/licenses/newlib" "$LICENSES/newlib"
    write_notice
}

# Which files the recipe builds and which are Espressif's, with the notice of the core configuration file compiled
# into binutils' programs
write_notice() {
    modules_notice="$(sed -n '1,/\*\//p' "$SRC/ctng/overlays/xtensa_esp32/binutils/bfd/xtensa-modules.c")"
    cat > "$LICENSES/NOTICE" << EOF
The programs under $PREFIX/bin, $PREFIX/libexec and
$PREFIX/$TARGET/bin are built by the StelluxOS recipe packages/xtensa-esp32-elf
from Espressif's GCC at tag $ESP_RELEASE (https://github.com/espressif/gcc) and
binutils at tag $BINUTILS_TAG (https://github.com/espressif/binutils-gdb), with the
ESP32's core configuration from Espressif's crosstool-NG at release $ESP_RELEASE
(https://github.com/espressif/crosstool-NG). They are distributed under the licenses in
gcc/ and binutils/, and hold code of GMP, MPFR, MPC, zlib, libbacktrace, libcody (in
cc1plus) and musl, under the licenses in the directories of those names. binutils'
programs also hold the core configuration's bfd/xtensa-modules.c, which carries the
notice at the end of this file.

The files under $PREFIX/$TARGET/include and $PREFIX/$TARGET/lib,
and the libraries, startup files, unwind.h and gcov.h under
$PREFIX/lib/gcc/$TARGET/$GCC_VER, are Espressif's, unmodified, from
its release $(basename "$RELEASE_URL")
(https://github.com/espressif/crosstool-NG/releases/tag/$ESP_RELEASE). newlib's and
libgloss's files among them are under the licenses in newlib/, and libgcc, libgcov,
libstdc++, libsupc++ and GCC's crt*.o files under the GNU GPL version 3 with the GCC
Runtime Library Exception, both in gcc/.

bfd/xtensa-modules.c

$modules_notice
EOF
}

# A failing tar or zstd stops the build before the archive takes its name, since the image build takes any archive
# of an unpinned version in the cache as a local build
package() {
    archive="/out/xtensa-esp32-elf-$(package_version xtensa-esp32-elf)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf "/work/xtensa-esp32-elf-$ARCH.tar" .
    zstd -19 -T"$JOBS" -q --no-progress -f "/work/xtensa-esp32-elf-$ARCH.tar" -o "$archive.tmp"
    rm -f "/work/xtensa-esp32-elf-$ARCH.tar"
    chown "$OUT_OWNER" "$archive.tmp"
    mv "$archive.tmp" "$archive"
}

mkdir -p "$LOGS"
build_cross
unpack_sources
place_esp32_side
build_container_tools
export PATH="$CROSS/bin:$CONTAINER_TOOLS/bin:$PATH"
build_stellux_tools
finish_tree
collect_licenses
package
