#!/bin/bash
# Runs inside the Chromium build container, one step per argument: fetch,
# prepare, configure, build, install. The recipe is bind mounted at /stellux,
# the tree lives on the /work volume.
set -euo pipefail

RECIPE=/stellux/packages/chromium
. "$RECIPE/versions.sh"

ARCH="$(uname -m)"
DL=/work/dl
SRC="/work/src/chromium-$CHROMIUM_VER"
OUT="out/stellux-$ARCH"
PACKAGES_CACHE=/stellux/userland/toolchain/packages

case "$ARCH" in
aarch64) GN_CPU=arm64 ;;
x86_64)  GN_CPU=x64 ;;
*) echo "chromium: unsupported build machine $ARCH" >&2; exit 1 ;;
esac

# Downloads $2 to $1 unless a copy with the pinned sha512 is already there
fetch_verified() {
    local dest="$1" url="$2" sha="$3"
    if [ ! -f "$dest" ] || ! echo "$sha  $dest" | sha512sum -c -s; then
        curl -fL --retry 5 -o "$dest.part" "$url"
        mv "$dest.part" "$dest"
    fi
    echo "$sha  $dest" | sha512sum -c -s || {
        echo "chromium: $dest does not match its pinned sha512" >&2
        exit 1
    }
}

step_fetch() {
    mkdir -p "$DL"
    fetch_verified "$DL/chromium-$CHROMIUM_VER-linux.tar.xz" "$CHROMIUM_URL" "$CHROMIUM_SHA512"
    fetch_verified "$DL/rollup-wasm-$ROLLUP_VER.tgz" "$ROLLUP_URL" "$ROLLUP_SHA512"
}

# Copies the Stellux-only sources and the stlxwin and libstlx sources they build
# on into the tree, so an already prepared tree catches up
sync_tree() {
    cd "$SRC"
    rsync -a "$RECIPE/overlay/" ./

    local stlxwin=/stellux/userland/lib/libstlxwin
    rsync -a --delete "$stlxwin/include/" third_party/stlxwin/include/
    rsync -a --delete "$stlxwin/src/" third_party/stlxwin/src/

    local libstlx=/stellux/userland/lib/libstlx
    mkdir -p third_party/libstlx/include/stlx third_party/libstlx/src
    cp "$libstlx/include/stlx/proc.h" "$libstlx/include/stlx/syscall_nums.h" third_party/libstlx/include/stlx/
    cp "$libstlx/src/proc.c" third_party/libstlx/src/
}

step_prepare() {
    if [ -f "$SRC/.stellux-prepared" ]; then
        echo "chromium: $SRC is already prepared"
        return
    fi

    if [ ! -d "$SRC" ]; then
        mkdir -p /work/src
        xz -T"$BUILD_JOBS" -dc "$DL/chromium-$CHROMIUM_VER-linux.tar.xz" | tar -x -C /work/src
    fi

    cd "$SRC"

    grep -vE '^\s*(#|$)' "$RECIPE/series" | while read -r patch_name; do
        echo "chromium: applying $patch_name"
        patch -p1 --forward --no-backup-if-mismatch -i "$RECIPE/patches/$patch_name"
    done

    # The tarball carries prebuilt x86_64 tools that cannot run here
    scanelf -RA -F "%F" . 2>/dev/null | while read -r elf; do rm -f "$elf"; done

    # Alpine's tool wiring, minus its system library unbundling
    touch chrome/test/data/webui/i18n_process_css_test.html
    mkdir -p third_party/node/linux/node-linux-x64/bin
    ln -sf /usr/bin/node third_party/node/linux/node-linux-x64/bin/node

    rm -rf third_party/devtools-frontend/src/node_modules/rollup
    mkdir third_party/devtools-frontend/src/node_modules/rollup
    tar xf "$DL/rollup-wasm-$ROLLUP_VER.tgz" --strip-components=1 \
        -C third_party/devtools-frontend/src/node_modules/rollup
    rm -f third_party/devtools-frontend/src/third_party/esbuild/esbuild
    ln -s /usr/bin/esbuild third_party/devtools-frontend/src/third_party/esbuild/esbuild
    rm -rf third_party/devtools-frontend/src/node_modules/esbuild
    ln -s /usr/lib/node_modules/esbuild third_party/devtools-frontend/src/node_modules/esbuild

    mkdir -p third_party/gperf/cipd/bin
    ln -sf /usr/bin/gperf third_party/gperf/cipd/bin/

    for goarch in amd64 arm64 arm ""; do
        mkdir -p "third_party/dawn/tools/golang/linux-$goarch/bin"
        ln -sf /usr/bin/go "third_party/dawn/tools/golang/linux-$goarch/bin/go"
    done

    python3 third_party/libaddressinput/chromium/tools/update-strings.py
    sed -i 's,^update_readme$,#update_readme,' third_party/libvpx/generate_gni.sh
    echo "$ARCH-$RUST_TARGET_SUFFIX" >> build/rust/known-target-triples.txt

    touch .stellux-prepared
}

# gn reads the compiler from the environment whenever it runs, including when
# ninja reruns it, so every step that can lead to gn exports the same one
export_toolchain() {
    export CC="$LLVM_ROOT/bin/clang" CXX="$LLVM_ROOT/bin/clang++"
    export AR="$LLVM_ROOT/bin/llvm-ar" NM="$LLVM_ROOT/bin/llvm-nm"
    export CFLAGS="-O2 -Wno-unknown-warning-option -Wno-builtin-macro-redefined -Wno-deprecated-declarations -Wno-shift-count-overflow -Wno-ignored-attributes"
    export CXXFLAGS="-O2 -Wno-unknown-warning-option -Wno-builtin-macro-redefined -Wno-deprecated-declarations -Wno-invalid-constexpr"
    export CPPFLAGS="-D__DATE__= -D__TIME__= -D__TIMESTAMP__="
    export LDFLAGS=""
    export RUSTC_BOOTSTRAP=1
}

step_configure() {
    sync_tree
    export_toolchain

    mkdir -p "$OUT"
    { cat "$RECIPE/args.gn"; echo "target_cpu = \"$GN_CPU\""; } > "$OUT/args.gn"
    gn gen "$OUT"
}

step_build() {
    cd "$SRC"
    export_toolchain
    ulimit -n 65536
    # shellcheck disable=SC2086
    ninja -C "$OUT" -j "$BUILD_JOBS" ${NINJA_FLAGS:-} "$PROGRAM"
}

# The unstripped binary stays in $OUT next to its split debug info, which is
# what llvm-symbolizer needs to turn a Stellux backtrace into source lines
step_install() {
    local root=/work/pkg/rootfs
    local archive="$PACKAGES_CACHE/chromium-$CHROMIUM_VER-$CHROMIUM_PKG_REL-$ARCH.tar.zst"

    rm -rf /work/pkg
    mkdir -p "$root/usr/bin" "$PACKAGES_CACHE"
    "$LLVM_ROOT/bin/llvm-strip" -o "$root/usr/bin/$PROGRAM" "$SRC/$OUT/$PROGRAM"

    # Chromium looks for its resources next to its own binary
    for resource in $RESOURCE_FILES; do
        mkdir -p "$root/usr/bin/$(dirname "$resource")"
        cp "$SRC/$OUT/$resource" "$root/usr/bin/$resource"
    done

    # Blink cannot lay out text without at least one font
    mkdir -p "$root/usr/share/fonts/dejavu" "$root/etc/fonts"
    for font in $FONT_FILES; do
        cp "/usr/share/fonts/dejavu/$font" "$root/usr/share/fonts/dejavu/"
    done
    cp "$RECIPE/fonts.conf" "$root/etc/fonts/fonts.conf"

    mkdir -p "$root/usr/share/licenses/chromium" "$root/usr/share/doc/chromium"
    cp "$SRC/LICENSE" "$root/usr/share/licenses/chromium/LICENSE"
    cp "$RECIPE"/licenses/* "$root/usr/share/licenses/chromium/"

    # The toolchain comes from Alpine edge, which moves, so the package records what built it
    {
        echo "chromium $CHROMIUM_VER-$CHROMIUM_PKG_REL aports $APORTS_COMMIT copium $COPIUM_TAG"
        echo "image $ALPINE_IMAGE"
        apk info -v clang22 lld22 llvm22 rust cargo gn samurai nodejs go
    } > "$root/usr/share/doc/chromium/BUILDINFO"

    # The desktop picks the launcher up from the drop-in directory
    mkdir -p "$root/etc/stlxdm/conf.d" "$root/etc/res/icons"
    cp "$RECIPE/browser.conf" "$root/etc/stlxdm/conf.d/browser.conf"
    cp "$RECIPE/icon_browser_32x32.bmp" "$root/etc/res/icons/"

    tar -C "$root" --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 \
        --pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime \
        -cf - . | zstd -19 -T0 -q --no-progress -f -o "$archive"

    echo "chromium: packaged $(basename "$archive")"
}

for step in "$@"; do
    "step_$step"
done
