#!/bin/sh
# Runs inside the python recipe's container: builds the Stellux sysroot, then zlib and CPython for ARCH against it,
# and packs the interpreter, its stdlib and their license texts into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the tree in /in and the sources in /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="${JOBS:-$(nproc)}"
SYSROOT="/work/sysroot/$ARCH"
TOOLS="/work/tools/$ARCH"
SRC="/work/src/$ARCH"
BUILD="/work/build/$ARCH"
ROOT="/work/root/$ARCH"
PYTHON_SRC="$SRC/Python-$PYTHON_VER"
ZLIB_SRC="$SRC/zlib-$ZLIB_VER"
DEPS="$BUILD/deps"
PYTHON_BIN="python${PYTHON_VER%.*}"
STDLIB="$ROOT/usr/lib/$PYTHON_BIN"
LICENSES="$ROOT/usr/share/licenses/python"
APACHE_LICENSE="/usr/share/common-licenses/Apache-2.0"

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    step="$(basename "$log" .log)"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "python: the $step step failed, its output is in $log inside the stellux-python-work volume" >&2
        exit 1
    }
}

# The sysroot every Stellux program links against, without the C++ runtime but with the Linux headers CPython's
# configure looks for, kept in the /work volume until SYSROOT_KEY or a pin changes
build_sysroot() {
    key="$SYSROOT_KEY $MUSL_VER $LLVM_VER"
    if [ "$(cat /work/sysroot/.key 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== python: building the Stellux sysroot ==="
    rm -rf /work/tree /work/sysroot
    mkdir -p /work/tree
    tar -C /work/tree -xf /in/tree.tar
    cp "/sources/musl-$MUSL_VER.tar.gz" "/sources/llvm-project-$LLVM_VER.src.tar.xz" /work/tree/userland/
    run_logged /work/sysroot.log make -C /work/tree NPROC="$JOBS" musl linux-headers compiler-rt

    mv /work/tree/userland/sysroot /work/sysroot
    rm -rf /work/tree
    echo "$key" > /work/sysroot/.key
}

# A compiler driver for ARCH that takes headers from the sysroot alone, as userland/mk/toolchain.mk does, and links
# static programs against musl as userland/mk/app.mk does, since configure links test programs
write_compiler_driver() {
    rm -rf "$TOOLS"
    mkdir -p "$TOOLS"
    cat > "$TOOLS/cc" << EOF
#!/bin/sh
link=1
for arg in "\$@"; do
    case "\$arg" in
    -c|-S|-E|-M|-MM|-fsyntax-only|-v|--version|-dumpversion|-dumpmachine|-print-*|-###) link=0 ;;
    esac
done

flags="--target=$ARCH-linux-musl --sysroot=$SYSROOT -nostdlibinc -isystem $SYSROOT/include"
if [ "\$link" = 0 ]; then
    exec clang \$flags "\$@"
fi
exec clang \$flags -nostdlib -fuse-ld=lld -static $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o -L$SYSROOT/lib "\$@" \\
    -Wl,--start-group -lc -lm $SYSROOT/lib/libclang_rt.builtins-$ARCH.a -Wl,--end-group $SYSROOT/lib/crtn.o
EOF
    chmod +x "$TOOLS/cc"
}

# The sources unpacked fresh on every run, with Stellux's patches applied to CPython
unpack_sources() {
    rm -rf "$SRC" "$BUILD"
    mkdir -p "$SRC" "$BUILD/python/Modules"
    tar -C "$SRC" -xf "/sources/Python-$PYTHON_VER.tar.xz"
    tar -C "$SRC" -xf "/sources/zlib-$ZLIB_VER.tar.gz"
    for patch in /recipe/patches/*.patch; do
        patch -s -d "$PYTHON_SRC" -p1 < "$patch"
    done
}

# zlib built as a static library into DEPS, for CPython's zlib and binascii modules to link
build_zlib() {
    echo "=== python: building zlib $ZLIB_VER for $ARCH ==="
    cd "$ZLIB_SRC"
    run_logged "$BUILD/zlib-configure.log" env CC="$TOOLS/cc" AR=llvm-ar RANLIB=llvm-ranlib CFLAGS=-O2 \
        ./configure --static --prefix="$DEPS"
    run_logged "$BUILD/zlib-make.log" make -j"$JOBS" install
    cd /work
}

build_python() {
    echo "=== python: building CPython $PYTHON_VER for $ARCH ==="
    cp /recipe/Modules.Setup.local "$BUILD/python/Modules/Setup.local"

    cd "$BUILD/python"
    run_logged "$BUILD/configure.log" env \
        CONFIG_SITE=/recipe/config.site \
        CC="$TOOLS/cc" \
        AR=llvm-ar \
        CFLAGS="-std=c11 -O2 -D_GNU_SOURCE -D__STELLUX__ -fno-stack-protector" \
        ZLIB_CFLAGS="-I$DEPS/include" \
        ZLIB_LIBS="-L$DEPS/lib -lz" \
        PKG_CONFIG=false \
        ac_cv_file__dev_ptmx=no \
        ac_cv_file__dev_ptc=no \
        "$PYTHON_SRC/configure" \
            --build="$("$PYTHON_SRC/config.guess")" \
            --host="$ARCH-linux-musl" \
            --prefix=/usr \
            --disable-shared \
            --disable-test-modules \
            --disable-ipv6 \
            --without-ensurepip \
            --with-build-python=python3
    run_logged "$BUILD/make.log" make -j"$JOBS" LINKFORSHARED=""
    cd /work
}

# The MIT notice a HACL* source opens with, without its comment marks
hacl_notice() {
    awk 'NR == 1 { sub(/^\/\* /, ""); print; next } /^ \*\// { exit } { sub(/^ \* ?/, ""); print }' "$1"
}

# HACL*'s notice, which the four hash sources built into the interpreter share. The build stops unless it holds a
# copyright line and the MIT grant and every source carries it, so a CPython bump cannot ship a wrong notice.
print_hacl_notice() {
    notice="$(hacl_notice "$PYTHON_SRC/Modules/_hacl/Hacl_Hash_SHA2.c")"
    case "$notice" in
    *Copyright*"Permission is hereby granted"*) ;;
    *)
        echo "python: Modules/_hacl/Hacl_Hash_SHA2.c lacks a copyright line or the MIT license" >&2
        exit 1
        ;;
    esac

    for source in Hacl_Hash_MD5.c Hacl_Hash_SHA1.c Hacl_Hash_SHA3.c; do
        if [ "$(hacl_notice "$PYTHON_SRC/Modules/_hacl/$source")" != "$notice" ]; then
            echo "python: Modules/_hacl/$source carries a different notice from Hacl_Hash_SHA2.c" >&2
            exit 1
        fi
    done

    printf '%s\n' "$notice"
}

# The interpreter and its stdlib, with the license texts of the code built into the interpreter: the software CPython
# records as incorporated, HACL*'s hash code and its KaRaMeL headers, musl and zlib
install_package() {
    rm -rf "$ROOT"
    run_logged "$BUILD/install.log" make -C "$BUILD/python" DESTDIR="$ROOT" altbininstall libinstall

    rm -rf "$STDLIB/lib-dynload" "$STDLIB"/config-* "$ROOT/usr/lib/pkgconfig"
    rm -rf "$STDLIB/test" "$STDLIB/idlelib" "$STDLIB/tkinter" "$STDLIB/turtledemo" \
           "$STDLIB/ensurepip" "$STDLIB/venv" "$STDLIB/lib2to3"
    find "$ROOT" -name '__pycache__' -type d -prune -exec rm -rf {} +
    find "$ROOT" \( -name '*.so' -o -name '*.a' -o -name '*.o' -o -name '*.pyc' \) -delete
    rm -f "$ROOT"/usr/bin/python*-config "$ROOT"/usr/lib/libpython*

    llvm-strip "$ROOT/usr/bin/$PYTHON_BIN"
    mkdir -p "$ROOT/bin"
    ln -sf "/usr/bin/$PYTHON_BIN" "$ROOT/bin/python3"

    mkdir -p "$LICENSES/cpython" "$LICENSES/hacl" "$LICENSES/musl" "$LICENSES/zlib"
    cp "$PYTHON_SRC/Doc/license.rst" "$LICENSES/cpython/"
    print_hacl_notice > "$LICENSES/hacl/LICENSE"
    tar -xzf "/sources/musl-$MUSL_VER.tar.gz" -O "musl-$MUSL_VER/COPYRIGHT" > "$LICENSES/musl/COPYRIGHT"
    cp "$ZLIB_SRC/LICENSE" "$LICENSES/zlib/"

    if ! echo "$APACHE_LICENSE_SHA256  $APACHE_LICENSE" | sha256sum -c --status -; then
        echo "python: $APACHE_LICENSE is missing or differs from the text APACHE_LICENSE_SHA256 pins" >&2
        exit 1
    fi
    cp "$APACHE_LICENSE" "$LICENSES/hacl/"
}

# A failing tar or zstd stops the build before the archive takes its name, since the image build takes any archive
# of an unpinned version in the cache as a local build
package() {
    archive="/out/python-$(package_version python)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf "/work/python-$ARCH.tar" .
    zstd -19 -T"$JOBS" -q --no-progress -f "/work/python-$ARCH.tar" -o "$archive.tmp"
    rm -f "/work/python-$ARCH.tar"
    chown "$OUT_OWNER" "$archive.tmp"
    mv "$archive.tmp" "$archive"
}

build_sysroot
write_compiler_driver
unpack_sources
build_zlib
build_python
install_package
package
