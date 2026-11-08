#!/bin/sh
# Runs inside the vk-gl-cts recipe's container: builds the Stellux sysroot, then VK-GL-CTS's
# deqp-vk for ARCH against the mesa kit, and packs it with its data and case lists into /out.
#
# Usage: container.sh <x86_64|aarch64>, with the tree and the mesa kit in /in and the sources in
# /sources
set -eu

ARCH="${1:?usage: container.sh <x86_64|aarch64>}"
. /recipe/versions.sh

JOBS="${JOBS:-$(nproc)}"
SYSROOT="/work/sysroot/$ARCH"
TOOLCHAIN="/work/toolchain-$ARCH.cmake"
KIT="/work/kit/$ARCH"
TREE_FILES="/work/tree-files/$ARCH"
SRC="/work/src/$ARCH"
CTS="$SRC/VK-GL-CTS-vulkan-cts-$VK_GL_CTS_VER"
BUILD="/work/build/$ARCH"
ROOT="/work/root/$ARCH"

# The test groups that get a case list, which cover what a compositor and toolkits draw with, and
# the most cases a list takes, which bounds how many cases, and so roughly how long, one list runs
CASELIST_GROUPS="api memory synchronization2 compute draw renderpasses pipeline image binding_model
    rasterization glsl ssbo ubo texture"
CASELIST_MAX_CASES=500

# Runs a command with its output in a log, shown only when it fails
run_logged() {
    log="$1"
    step="$(basename "$log" .log)"
    shift
    "$@" > "$log" 2>&1 || {
        tail -40 "$log"
        echo "vk-gl-cts: the $step step failed, its output is in $log inside the stellux-vk-gl-cts-work volume" >&2
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

    echo "=== vk-gl-cts: building the Stellux sysroot ==="
    rm -rf /work/tree /work/sysroot
    mkdir -p /work/tree
    tar -C /work/tree -xf /in/tree.tar
    cp "/sources/musl-$MUSL_VER.tar.gz" "/sources/llvm-project-$LLVM_VER.src.tar.xz" /work/tree/userland/
    run_logged /work/sysroot.log make -C /work/tree NPROC="$JOBS" musl libcxx compiler-rt

    mv /work/tree/userland/sysroot /work/sysroot
    rm -rf /work/tree
    echo "$key" > /work/sysroot/.key
}

# The kit's library and its link flags, read through the tree's userland/mk/vulkan.mk, which
# refuses a kit built against another GPU_ABI_VERSION or STLXWIN_GPU_ABI_VERSION
kit_link_flags() {
    rm -rf "$KIT" "$TREE_FILES"
    mkdir -p "$KIT" "$TREE_FILES"
    zstd -dc /in/mesa.tar.zst | tar -C "$KIT" -xf -
    tar -C "$TREE_FILES" -xf /in/tree.tar userland/mk/vulkan.mk kernel/gpu/gpu_abi.h userland/lib/libstlxwin

    # shellcheck disable=SC2016
    make -s -f "$TREE_FILES/userland/mk/vulkan.mk" USERLAND_ROOT="$TREE_FILES/userland" VULKAN_DIR="$KIT/mesa" \
        ARCH="$ARCH" --eval 'vulkan-libs: ; @echo $(VULKAN_LIBS)' vulkan-libs
}

# libstlxwin, which the kit's library calls, compiled from the tree's sources with the -std, -O2
# and stack flags its Makefile passes
build_stlxwin() {
    stlxwin="$TREE_FILES/userland/lib/libstlxwin"
    mkdir -p "$TREE_FILES/include/stlx"
    cp "$TREE_FILES/kernel/gpu/gpu_abi.h" "$TREE_FILES/include/stlx/"
    for source in "$stlxwin"/src/*.c; do
        run_logged "$TREE_FILES/stlxwin.log" clang --target="$ARCH-linux-musl" --sysroot="$SYSROOT" -nostdlibinc \
            -isystem "$SYSROOT/include" -std=c11 -O2 -fno-stack-protector -I"$stlxwin/include" \
            -I"$TREE_FILES/include" -c "$source" -o "${source%.c}.o"
    done
    llvm-ar crs "$TREE_FILES/libstlxwin.a" "$stlxwin"/src/*.o
}

# Compiles for Stellux the way userland/mk/toolchain.mk does and links the way
# userland/mk/cxxapp.mk does, with the kit and libstlxwin in every C++ link
write_toolchain() {
    builtins="$SYSROOT/lib/libclang_rt.builtins-$ARCH.a"
    vulkan_libs="$(kit_link_flags)"
    build_stlxwin
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
set(CMAKE_CXX_STANDARD_LIBRARIES "$vulkan_libs $TREE_FILES/libstlxwin.a \
-Wl,--start-group -lc++ -lc++abi -lunwind -lc -lm $builtins -Wl,--end-group $SYSROOT/lib/crtn.o")
set(CMAKE_FIND_ROOT_PATH $SYSROOT)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
}

# VK-GL-CTS with the backports applied and each external unpacked where fetch_sources.py puts it
prepare_cts() {
    rm -rf "$SRC"
    mkdir -p "$SRC"
    tar -C "$SRC" -xzf "/sources/vk-gl-cts-$VK_GL_CTS_VER.tar.gz"
    while read -r commit _; do
        [ -z "$commit" ] ||
            patch -s -d "$CTS" -p1 --forward --no-backup-if-mismatch -i "/sources/backports/$commit.patch"
    done << EOF
$VK_GL_CTS_BACKPORTS
EOF
    while read -r name url _; do
        [ -n "$name" ] || continue
        mkdir -p "$CTS/external/$name/src"
        tar -C "$CTS/external/$name/src" --strip-components=1 -xzf "/sources/$name/${url##*/}"
    done << EOF
$VK_GL_CTS_EXTERNALS
EOF

    # libpng builds with the configuration header fetch_sources.py copies into place
    cp "$CTS/external/libpng/src/scripts/pnglibconf.h.prebuilt" "$CTS/external/libpng/src/pnglibconf.h"
}

# The headless target needs no window system and loads Vulkan through dlopen, which the kit's
# loader shim answers. Video tests need sources the recipe leaves out, so they report as unsupported.
build_deqp_vk() {
    echo "=== vk-gl-cts: building deqp-vk for $ARCH ==="
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    run_logged "$BUILD/configure.log" cmake -G Ninja -S "$CTS" -B "$BUILD/cts" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=vulkan_headless \
        -DSELECTED_BUILD_TARGETS=deqp-vk -DDEQP_DISABLE_VK_VIDEO_TESTS=ON
    run_logged "$BUILD/build.log" cmake --build "$BUILD/cts" --parallel "$JOBS"
}

# The license texts of VK-GL-CTS, of the externals the recipe fetches, of musl and of the mesa
# kit's projects
collect_licenses() {
    licenses="$ROOT/usr/share/licenses/vk-gl-cts"
    mkdir -p "$licenses/mesa-kit"
    cp "$CTS/LICENSE" "$CTS/NOTICE" "$licenses/"
    while read -r name _; do
        [ -n "$name" ] || continue
        mkdir -p "$licenses/$name"
        cp -R "$CTS/external/$name/src"/LICENSE* "$licenses/$name/"
    done << EOF
$VK_GL_CTS_EXTERNALS
EOF
    cp -R "$KIT/mesa/LICENSES/." "$licenses/mesa-kit/"
    mkdir -p "$licenses/musl"
    tar -xzf "/sources/musl-$MUSL_VER.tar.gz" -O "musl-$MUSL_VER/COPYRIGHT" > "$licenses/musl/COPYRIGHT"
}

# deqp-vk, the data it reads from its archive directory, its case lists, the license texts, and
# BUILDINFO naming the release, its backports and the kit deqp-vk links
install_package() {
    rm -rf "$ROOT"
    mkdir -p "$ROOT/usr/bin" "$ROOT/usr/share/vk-gl-cts" "$ROOT/usr/share/doc/vk-gl-cts"
    llvm-strip -o "$ROOT/usr/bin/deqp-vk" "$BUILD/cts/external/vulkancts/modules/vulkan/deqp-vk"
    cp -R "$CTS/external/vulkancts/data/vulkan" "$ROOT/usr/share/vk-gl-cts/"
    # shellcheck disable=SC2086
    python3 /recipe/caselists.py "$CTS/external/vulkancts/mustpass/main" \
        "$ROOT/usr/share/vk-gl-cts/caselists" "$CASELIST_MAX_CASES" $CASELIST_GROUPS
    collect_licenses

    {
        echo "vk-gl-cts $(package_version vk-gl-cts) from vulkan-cts-$VK_GL_CTS_VER with backports:"
        echo "$VK_GL_CTS_BACKPORTS" | awk 'NF { print "  " $1 }'
        echo "linked against $MESA_KIT, archive sha256 $MESA_KIT_SHA256"
    } > "$ROOT/usr/share/doc/vk-gl-cts/BUILDINFO"
}

package() {
    archive="/out/vk-gl-cts-$(package_version vk-gl-cts)-$ARCH.tar.zst"
    echo "=== packaging $(basename "$archive") ==="
    tar -C "$ROOT" --sort=name --owner=0 --group=0 --numeric-owner \
        --mtime="@$SOURCE_DATE_EPOCH" \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T"$JOBS" -q --no-progress -f -o "$archive"
    chown "$OUT_OWNER" "$archive"
}

build_sysroot
write_toolchain
prepare_cts
build_deqp_vk
install_package
package
