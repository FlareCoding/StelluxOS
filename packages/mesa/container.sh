#!/bin/sh
# Runs inside the mesa recipe's container: builds the Stellux sysroot, the static libraries Mesa
# needs, Mesa's build-time tools, then NVK for ARCH, prelinked into one object archived with the
# loader shim as the Vulkan library, and packs the kit into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the tree in /in and the sources in /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="${JOBS:-$(nproc)}"
SYSROOT="/work/sysroot/$ARCH"
BUILTINS="$SYSROOT/lib/libclang_rt.builtins-$ARCH.a"
TOOLS="/work/tools/$ARCH"
DEPS="/work/deps/$ARCH"
SRC="/work/src/$ARCH"
BUILD="/work/build/$ARCH"
CROSS_FILE="$TOOLS/cross.ini"
MESA_SRC="/work/src/mesa-$MESA_VER"
HOST_TOOLS="/work/host-tools"
RUST_TARGET="$ARCH-unknown-linux-musl"
KIT="/work/kit/$ARCH"
PREFIX="$KIT/mesa"

# What a loader looks up in NVK, the only global symbols the prelink leaves in NVK's object
ICD_ENTRY_POINTS="vk_icdGetInstanceProcAddr vk_icdGetPhysicalDeviceProcAddr vk_icdNegotiateLoaderICDInterfaceVersion"

# The loader shim answers dlopen for libvulkan, so every Vulkan program's link wraps musl's three calls
LOADER_WRAPS="-Wl,--wrap=dlopen,--wrap=dlsym,--wrap=dlclose"

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    step="$(basename "$log" .log)"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "mesa: the $step step failed, its output is in $log inside the stellux-mesa-kit-work volume" >&2
        exit 1
    }
}

# Unpacks a source tarball into SRC and prints the directory it made
unpack() {
    tar -C "$SRC" -xf "/sources/$1"
    echo "$SRC/$2"
}

# The sysroot every Stellux program links against, built by the tree's own targets and
# kept in the /work volume until the tree's recipe for it changes
build_sysroot() {
    key="$SYSROOT_KEY $MUSL_VER $LLVM_VER"
    if [ "$(cat /work/sysroot/.key 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== mesa: building the Stellux sysroot ==="
    rm -rf /work/tree /work/sysroot
    mkdir -p /work/tree
    tar -C /work/tree -xf /in/tree.tar
    cp "/sources/musl-$MUSL_VER.tar.gz" "/sources/llvm-project-$LLVM_VER.src.tar.xz" /work/tree/userland/
    run_logged /work/sysroot.log make -C /work/tree NPROC="$JOBS" musl libcxx compiler-rt

    mv /work/tree/userland/sysroot /work/sysroot
    rm -rf /work/tree
    echo "$key" > /work/sysroot/.key
}

# <stlx/gpu_abi.h>, the kernel interface NVK calls on Stellux, taken from the tree on every run
# since the sysroot outlives the tree it was built from
install_gpu_abi() {
    mkdir -p "$SYSROOT/include/stlx"
    tar -C "$SYSROOT/include/stlx" -xf /in/tree.tar --strip-components=2 kernel/gpu/gpu_abi.h
}

# <stlxwin/stlxwin.h>, the window library interface NVK's window surface calls, taken from the tree
# on every run as gpu_abi.h is
install_stlxwin_header() {
    mkdir -p "$SYSROOT/include/stlxwin"
    tar -C "$SYSROOT/include/stlxwin" -xf /in/tree.tar --strip-components=5 \
        userland/lib/libstlxwin/include/stlxwin/stlxwin.h
}

# A compiler driver for ARCH that compiles the way userland/mk/toolchain.mk does and links the way
# userland/mk/cxxapp.mk does, since configure scripts and Meson link test programs
write_driver() {
    cat > "$TOOLS/$1" << EOF
#!/bin/sh
link=1
for arg in "\$@"; do
    case "\$arg" in
    -c|-S|-E|-M|-MM|-fsyntax-only|-v|--version|-dumpversion|-dumpmachine|-print-*|-###) link=0 ;;
    esac
done

flags="--target=$ARCH-linux-musl --sysroot=$SYSROOT -nostdlibinc $3"
if [ "\$link" = 0 ]; then
    exec $2 \$flags "\$@"
fi
exec $2 \$flags -nostdlib -fuse-ld=lld -static $SYSROOT/lib/crt1.o $SYSROOT/lib/crti.o -L$SYSROOT/lib "\$@" \\
    -Wl,--start-group $4 $BUILTINS -Wl,--end-group $SYSROOT/lib/crtn.o
EOF
    chmod +x "$TOOLS/$1"
}

# The drivers, and the Meson cross file naming them with the libraries built below
write_toolchain() {
    rm -rf "$TOOLS"
    mkdir -p "$TOOLS"
    write_driver cc clang "-isystem $SYSROOT/include" "-lc -lm"
    write_driver c++ clang++ "-isystem $SYSROOT/include/c++/v1 -isystem $SYSROOT/include" \
        "-lc++ -lc++abi -lunwind -lc -lm"

    # rustc links programs with its own musl startup files, which the drivers' would duplicate.
    # It links only Meson's sanity check here, as NAK and NIL are static libraries.
    cat > "$TOOLS/rust-cc" << EOF
#!/bin/sh
exec clang --target=$ARCH-linux-musl -fuse-ld=lld "\$@"
EOF
    chmod +x "$TOOLS/rust-cc"

    cat > "$CROSS_FILE" << EOF
[binaries]
c = '$TOOLS/cc'
cpp = '$TOOLS/c++'
rust = ['rustc', '--target', '$RUST_TARGET']
rust_ld = '$TOOLS/rust-cc'
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = ['$DEPS/lib/pkgconfig', '$DEPS/share/pkgconfig']
bindgen_clang_arguments = ['--target=$ARCH-linux-musl', '--sysroot=$SYSROOT', '-nostdlibinc',
    '-isystem', '$SYSROOT/include']

[host_machine]
system = 'linux'
cpu_family = '$ARCH'
cpu = '$ARCH'
endian = 'little'
EOF
}

# The environment autotools and zlib's configure build ARCH's static libraries with
cross_env() {
    env CC="$TOOLS/cc" AR=llvm-ar RANLIB=llvm-ranlib CFLAGS="${DEPS_CFLAGS:--O2}" \
        CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig" "$@"
}

build_zlib() {
    dir="$(unpack "zlib-$ZLIB_VER.tar.gz" "zlib-$ZLIB_VER")"
    cd "$dir"
    run_logged "$BUILD/zlib.log" cross_env ./configure --static --prefix="$DEPS"
    run_logged "$BUILD/zlib.log" make -j"$JOBS" install
}

build_libdrm() {
    dir="$(unpack "libdrm-$LIBDRM_VER.tar.xz" "libdrm-$LIBDRM_VER")"
    run_logged "$BUILD/libdrm.log" meson setup "$BUILD/libdrm" "$dir" --cross-file "$CROSS_FILE" \
        --prefix="$DEPS" --libdir=lib --buildtype=release --default-library=static \
        -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled \
        -Domap=disabled -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Dvc4=disabled \
        -Detnaviv=disabled -Dcairo-tests=disabled -Dman-pages=disabled -Dvalgrind=disabled \
        -Dudev=false -Dtests=false -Dinstall-test-programs=false
    run_logged "$BUILD/libdrm.log" ninja -C "$BUILD/libdrm" -j"$JOBS" install
}

# argp's header defines its inline functions the GNU89 way
build_argp() {
    dir="$(unpack "argp-standalone-$ARGP_VER.tar.gz" "argp-standalone-$ARGP_VER")"
    cd "$dir"
    run_logged "$BUILD/argp.log" autoreconf -fi
    DEPS_CFLAGS="-O2 -fgnu89-inline" run_logged "$BUILD/argp.log" \
        cross_env ./configure --host="$ARCH-linux-musl" --prefix="$DEPS"
    run_logged "$BUILD/argp.log" make -j"$JOBS"
    install -D -m644 argp.h "$DEPS/include/argp.h"
    install -D -m644 libargp.a "$DEPS/lib/libargp.a"
}

build_fts() {
    dir="$(unpack "musl-fts-$FTS_VER.tar.gz" "musl-fts-$FTS_VER")"
    cd "$dir"
    run_logged "$BUILD/fts.log" ./bootstrap.sh
    run_logged "$BUILD/fts.log" cross_env ./configure --host="$ARCH-linux-musl" --prefix="$DEPS" --disable-shared
    run_logged "$BUILD/fts.log" make -j"$JOBS" install
}

build_obstack() {
    dir="$(unpack "musl-obstack-$OBSTACK_VER.tar.gz" "musl-obstack-$OBSTACK_VER")"
    cd "$dir"
    run_logged "$BUILD/obstack.log" ./bootstrap.sh
    run_logged "$BUILD/obstack.log" cross_env ./configure --host="$ARCH-linux-musl" --prefix="$DEPS" \
        --disable-shared
    run_logged "$BUILD/obstack.log" make -j"$JOBS" install
}

# elfutils builds only libelf, the one part of it Mesa links
build_libelf() {
    dir="$(unpack "elfutils-$ELFUTILS_VER.tar.bz2" "elfutils-$ELFUTILS_VER")"
    cd "$dir"
    run_logged "$BUILD/libelf.log" cross_env ./configure --host="$ARCH-linux-musl" --prefix="$DEPS" \
        --disable-nls --disable-demangler --disable-debuginfod --disable-libdebuginfod \
        --without-bzlib --without-lzma --without-zstd --without-valgrind
    run_logged "$BUILD/libelf.log" make -C libelf -j"$JOBS" libelf.a

    # libelf calls elfutils' search tree wrappers, which only its shared library links in
    run_logged "$BUILD/libelf.log" make -C lib eu-search.o
    llvm-ar rs libelf/libelf.a lib/eu-search.o
    install -D -m644 libelf/libelf.a "$DEPS/lib/libelf.a"
    install -D -m644 libelf/libelf.h libelf/gelf.h libelf/nlist.h -t "$DEPS/include"
    install -D -m644 config/libelf.pc "$DEPS/lib/pkgconfig/libelf.pc"
}

# The static libraries Mesa needs that the sysroot lacks, rebuilt on every run
build_deps() {
    echo "=== mesa: building the $ARCH static libraries ==="
    rm -rf "$SRC" "$BUILD" "$DEPS"
    mkdir -p "$SRC" "$BUILD" "$DEPS"
    build_zlib
    build_libdrm
    build_argp
    build_fts
    build_obstack
    build_libelf
}

# Mesa's source with the recipe's patches, sharing one cache of the crates its wraps pin
prepare_mesa() {
    rm -rf "$MESA_SRC"
    mkdir -p /work/packagecache
    tar -C /work/src -xf "/sources/mesa-$MESA_VER.tar.xz"
    for patch in /recipe/patches/*.patch; do
        patch -s -d "$MESA_SRC" -p1 --forward --no-backup-if-mismatch -i "$patch"
    done
    ln -s /work/packagecache "$MESA_SRC/subprojects/packagecache"
}

# mesa_clc and vtn_bindgen2, which turn NVK's OpenCL shaders into SPIR-V and C while NVK builds,
# built for the host once per Mesa release and patch series
build_host_tools() {
    key="$MESA_VER $(cat /recipe/patches/*.patch | sha256sum | cut -d' ' -f1)"
    if [ "$(cat "$HOST_TOOLS/.key" 2> /dev/null)" = "$key" ]; then
        return
    fi

    echo "=== mesa: building Mesa's build-time tools ==="
    rm -rf "$HOST_TOOLS" /work/build/host-tools
    run_logged /work/host-tools.log meson setup /work/build/host-tools "$MESA_SRC" \
        --prefix="$HOST_TOOLS" --buildtype=release \
        -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dllvm=enabled \
        -Dvulkan-drivers= -Dgallium-drivers= -Dplatforms= -Dopengl=false -Degl=disabled \
        -Dglx=disabled -Dgbm=disabled -Dzstd=disabled
    run_logged /work/host-tools.log ninja -C /work/build/host-tools -j"$JOBS" install
    rm -rf /work/build/host-tools
    echo "$key" > "$HOST_TOOLS/.key"
}

# NVK's build-id: a hash of the Mesa release, the patch series and the package release
nvk_build_id() {
    { echo "$MESA_VER"; cat /recipe/patches/*.patch; echo "$MESA_PKG_REL"; } | sha1sum | cut -d' ' -f1
}

build_nvk() {
    echo "=== mesa: building NVK for $ARCH ==="
    run_logged "$BUILD/mesa.log" env PATH="$HOST_TOOLS/bin:$PATH" meson setup "$BUILD/mesa" "$MESA_SRC" \
        --cross-file "$CROSS_FILE" --prefix=/usr --buildtype=release -Ddefault_library=static \
        -Dvulkan-drivers=nouveau -Dgallium-drivers= -Dplatforms=stellux -Dnouveau-stellux=true \
        -Dnvk-build-id="$(nvk_build_id)" -Dmesa-clc=system -Dllvm=disabled -Dopengl=false \
        -Degl=disabled -Dglx=disabled -Dgbm=disabled -Dxmlconfig=disabled -Dexpat=disabled \
        -Dshader-cache=disabled -Dzstd=disabled -Dlibunwind=disabled -Dvalgrind=disabled
    run_logged "$BUILD/mesa.log" env PATH="$HOST_TOOLS/bin:$PATH" ninja -C "$BUILD/mesa" -j"$JOBS"
}

# Meson's link of libvulkan_nouveau, which the patches leave in build.ninja unbuilt
nvk_link_command() {
    ninja -C "$BUILD/mesa" -t commands src/nouveau/vulkan/libvulkan_nouveau.so | tail -1
}

# NVK in one object whose only global symbols are its ICD entry points, so neither Mesa's symbols
# nor Rust's standard library can clash with a program's own
prelink_nvk() {
    archives="$(nvk_link_command | tr ' ' '\n' | grep '\.a$' | grep -v '^src/nouveau/vulkan/libnvk\.a$')"

    # Mesa builds some of these archives only for the shared library
    # shellcheck disable=SC2046
    run_logged "$BUILD/prelink.log" env PATH="$HOST_TOOLS/bin:$PATH" ninja -C "$BUILD/mesa" -j"$JOBS" \
        $(echo "$archives" | grep -v '^/')

    roots=""
    kept=""
    for symbol in $ICD_ENTRY_POINTS; do
        roots="$roots -u $symbol"
        kept="$kept --keep-global-symbol=$symbol"
    done

    cd "$BUILD/mesa"
    # shellcheck disable=SC2086
    run_logged "$BUILD/prelink.log" ld.lld -r -o "$BUILD/nvk.o" $roots \
        --whole-archive src/nouveau/vulkan/libnvk.a --no-whole-archive $archives
    # shellcheck disable=SC2086
    llvm-objcopy $kept "$BUILD/nvk.o"

    # A program's link would discard NVK's COMDAT copy of any inline function the program also defines
    groups="$(llvm-readelf --section-groups "$BUILD/nvk.o")"
    case "$groups" in
    *COMDAT*)
        echo "mesa: NVK's object holds COMDAT groups, which a program's link could discard" >&2
        exit 1
        ;;
    esac
}

# The Vulkan library a program links: NVK and the loader shim
build_vulkan_library() {
    echo "=== mesa: linking the $ARCH Vulkan library ==="
    prelink_nvk
    run_logged "$BUILD/shim.log" "$TOOLS/cc" -O2 -Wall -Wextra -Werror -I"$MESA_SRC/include" \
        -c /recipe/loader/stlx_vulkan.c -o "$BUILD/stlx_vulkan.o"
    rm -f "$BUILD/libvulkan.a"
    llvm-ar rcsD "$BUILD/libvulkan.a" "$BUILD/nvk.o" "$BUILD/stlx_vulkan.o"
}

# What a program links besides the library: the wraps, and the libraries Rust's standard library
# names, which a static library of Rust code leaves to the program's link
library_link_flags() {
    : > "$BUILD/empty.rs"
    native_libs="$(rustc --crate-type staticlib --target "$RUST_TARGET" --print native-static-libs \
        -o "$BUILD/empty.a" "$BUILD/empty.rs" 2>&1 | sed -n 's/^note: native-static-libs: //p')"
    [ -n "$native_libs" ] || { echo "mesa: rustc named no native libraries for $RUST_TARGET" >&2; exit 1; }
    echo "$LOADER_WRAPS $native_libs"
}

# Prints the positive decimal value header $1 gives the macro $2, or stops the build with an error
# naming $3, the header's path in the tree
header_version() {
    version="$(awk -v name="$2" '$1 == "#define" && $2 == name { v = $3 } END { print v + 0 }' "$1")"
    [ "$version" -gt 0 ] || {
        echo "mesa: $3 defines no positive decimal $2" >&2
        exit 1
    }
    echo "$version"
}

# Vulkan's headers, the library, and share/stellux/vulkan.mk, which records the GPU_ABI_VERSION and
# STLXWIN_GPU_ABI_VERSION the library was compiled against for userland/mk/vulkan.mk to check
install_kit() {
    rm -rf "$KIT"
    mkdir -p "$PREFIX/include/vulkan" "$PREFIX/include/vk_video" "$PREFIX/lib" "$PREFIX/share/stellux"
    cp "$MESA_SRC"/include/vulkan/*.h "$PREFIX/include/vulkan/"
    cp "$MESA_SRC"/include/vk_video/*.h "$PREFIX/include/vk_video/"
    cp "$BUILD/libvulkan.a" "$PREFIX/lib/"

    gpu_abi_version="$(header_version "$SYSROOT/include/stlx/gpu_abi.h" GPU_ABI_VERSION \
        kernel/gpu/gpu_abi.h)"
    stlxwin_gpu_abi_version="$(header_version "$SYSROOT/include/stlxwin/stlxwin.h" STLXWIN_GPU_ABI_VERSION \
        userland/lib/libstlxwin/include/stlxwin/stlxwin.h)"

    LINK_FLAGS="$(library_link_flags)"
    cat > "$PREFIX/share/stellux/vulkan.mk" << EOF
# Generated by the mesa recipe: what a program using Vulkan from this kit compiles and links
# with, besides libstlxwin, which the library calls. The includer sets VULKAN_DIR to the kit's directory.
VULKAN_LIBRARY := \$(VULKAN_DIR)/lib/libvulkan.a
VULKAN_CPPFLAGS := -isystem \$(VULKAN_DIR)/include -DVK_USE_PLATFORM_STELLUX_STLX
VULKAN_LIBS := \$(VULKAN_LIBRARY) $LINK_FLAGS
# The GPU_ABI_VERSION of the kernel/gpu/gpu_abi.h the library was compiled against
VULKAN_GPU_ABI_VERSION := $gpu_abi_version
# The STLXWIN_GPU_ABI_VERSION of the stlxwin.h the library was compiled against
VULKAN_STLXWIN_GPU_ABI_VERSION := $stlxwin_gpu_abi_version
EOF
}

# libstlxwin for the probe's link, compiled from the tree's sources with the -std, -O2 and stack
# flags its Makefile passes
build_stlxwin() {
    rm -rf "$BUILD/stlxwin"
    mkdir -p "$BUILD/stlxwin"
    tar -C "$BUILD/stlxwin" -xf /in/tree.tar --strip-components=3 userland/lib/libstlxwin
    for source in "$BUILD"/stlxwin/src/*.c; do
        run_logged "$BUILD/stlxwin.log" "$TOOLS/cc" -std=c11 -O2 -fno-stack-protector \
            -I"$BUILD/stlxwin/include" -c "$source" -o "${source%.c}.o"
    done
    rm -f "$BUILD/libstlxwin.a"
    llvm-ar crs "$BUILD/libstlxwin.a" "$BUILD"/stlxwin/src/*.o
}

# The relocatable link reports no unresolved symbol, so a program calling into the library links
# with the kit's flags and libstlxwin, as cxxapp.mk links one, before the kit is packed
check_kit() {
    build_stlxwin
    cat > "$BUILD/probe.cpp" << 'EOF'
#include <vulkan/vulkan.h>

int main() {
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    VkInstance instance;
    return vkCreateInstance(&info, nullptr, &instance);
}
EOF
    # shellcheck disable=SC2086
    run_logged "$BUILD/probe.log" "$TOOLS/c++" -std=c++20 -O2 -Wall -Wextra -Werror \
        -isystem "$PREFIX/include" -DVK_USE_PLATFORM_STELLUX_STLX "$BUILD/probe.cpp" -o "$BUILD/probe" \
        "$PREFIX/lib/libvulkan.a" "$BUILD/libstlxwin.a" $LINK_FLAGS
}

# Stops the build unless a notice holds a copyright line and then the MIT license's grant, so a
# changed libdrm pin cannot leave NOTICES without them
check_mit_notice() {
    case "$2" in
    *Copyright*"Permission is hereby granted"*) ;;
    *)
        echo "mesa: $1 lacks a copyright line or the MIT license" >&2
        exit 1
        ;;
    esac
}

# The license texts of Mesa and of every project besides Stellux whose code the library holds
collect_licenses() {
    licenses="$PREFIX/LICENSES"
    mkdir -p "$licenses/mesa" "$licenses/libdrm" "$licenses/zlib" "$licenses/elfutils" "$licenses/rust"
    cp -R "$MESA_SRC/licenses/." "$licenses/mesa/"
    cp "$SRC/zlib-$ZLIB_VER/LICENSE" "$licenses/zlib/"
    elfutils="$SRC/elfutils-$ELFUTILS_VER"
    cp "$elfutils/COPYING" "$elfutils/COPYING-LGPLV3" "$elfutils/COPYING-GPLV2" "$licenses/elfutils/"

    # libdrm's notices open its sources, the five its static library is built from
    for source in "$SRC/libdrm-$LIBDRM_VER"/xf86drm*.c; do
        notice="$(awk '/^#/ { exit } { print }' "$source")"
        check_mit_notice "$source" "$notice"
        printf '== %s\n%s\n' "$(basename "$source")" "$notice" >> "$licenses/libdrm/NOTICES"
    done

    rust_docs="$(rustc --print sysroot)/share/doc/rust"
    cp -R "$rust_docs/COPYRIGHT-library.html" "$rust_docs/licenses" "$licenses/rust/"

    # The crates NAK and NIL link, as Meson's link of libvulkan_nouveau names them
    crates="$(nvk_link_command | tr ' ' '\n' | sed -n 's|^subprojects/\([^/]*\)/.*\.rlib$|\1|p' | sort -u)"
    for crate in $crates; do
        mkdir -p "$licenses/$crate"
        cp "$MESA_SRC/subprojects/$crate"/LICENSE* "$licenses/$crate/"
    done

    cat > "$licenses/README" << 'EOF'
The license texts of the projects besides Stellux whose code the Vulkan library holds, a directory
for each. Mesa's sources give their licenses in their opening comments, in full or by SPDX
identifier, and libelf may be used under the LGPLv3, which builds on the GPLv3, or under the GPLv2.
EOF
}

package_kit() {
    archive="/out/mesa-$MESA_VER-$MESA_PKG_REL-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$KIT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T"$JOBS" -q --no-progress -f -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

build_sysroot
install_gpu_abi
install_stlxwin_header
write_toolchain
build_deps
prepare_mesa
build_host_tools
build_nvk
build_vulkan_library
install_kit
check_kit
collect_licenses
package_kit
