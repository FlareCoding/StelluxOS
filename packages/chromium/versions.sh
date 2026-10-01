# Version and source pins for the chromium recipe. Sourced by build.sh on the
# host, by container.sh inside, and by the dispatcher for package_version.

CHROMIUM_VER=151.0.7922.137
CHROMIUM_PKG_REL=1 # Counts rebuilds of the same version with a changed recipe
CHROMIUM_URL="https://github.com/chromium-linux-tarballs/chromium-tarballs/releases/download/$CHROMIUM_VER/chromium-$CHROMIUM_VER-linux.tar.xz"
CHROMIUM_SHA512=379dede0ca1ae3137e67776608a1b899ecdfa2c5c566df92841f63f1643dbe09d021df05a6a2d1fd853fb7d0fab95e2fea6a4568c18a94fdb84758e0f668f68b

# content_shell embeds Chromium's web test fonts, which DEPS pins by object
# name in the chromium-fonts bucket
TEST_FONTS_OBJECT=9c07d19d9c5ee1ff94f717e6fb17e0c8c354e6f9
TEST_FONTS_URL="https://storage.googleapis.com/chromium-fonts/$TEST_FONTS_OBJECT"
TEST_FONTS_SHA512=2d3602b7a924d2bb8d538882f13a05428e23bf16ea3a9cf07eb13801fe7f33261ce61f2e5dc12e5c6f61f21b392526024d1969e8a967cf1f5138c647b84025b6

# Alpine's devtools build swaps the prebuilt native rollup for the wasm one
ROLLUP_VER=4.22.4
ROLLUP_URL="https://registry.npmjs.org/@rollup/wasm-node/-/wasm-node-$ROLLUP_VER.tgz"
ROLLUP_SHA512=d805e9353da0b52c866a7812593c26b293a7bb4c153012c2e94ff027c7ece7c261b65032f81de334eb9a37eef1f5b962940a22c0193dd7cef26e1aecee4682a6

# Alpine's musl packaging of this release, which patches/alpine and
# patches/copium are taken from and a version bump refreshes
APORTS_COMMIT=a7265853b9385c74045b02ca5aeac7c1413516bd
COPIUM_TAG=151.2

# Alpine edge carries the LLVM 22 and Rust toolchain the aports recipe expects
ALPINE_IMAGE="alpine:edge@sha256:020dfcbaaf4cc1078bf2d9c7ba31a8466e334061dcd2f248001d68f79e52c000"
ALPINE_PKGS="bash bison flex gperf perl findutils gzip xz tar patch git curl rsync zstd pax-utils file
    clang22 clang22-dev clang22-rtlib clang22-static lld22 llvm22 llvm22-dev llvm22-static
    gn samurai python3 py3-setuptools nodejs esbuild go rust rust-bindgen rustfmt cargo
    musl-dev musl-libintl linux-headers bsd-compat-headers llvm-libunwind-dev llvm-libunwind-static elfutils
    zlib-static util-linux-static font-dejavu"

# Fonts copied into the package from Alpine's font-dejavu
FONT_FILES="DejaVuSans.ttf DejaVuSans-Bold.ttf DejaVuSerif.ttf DejaVuSerif-Bold.ttf
    DejaVuSansMono.ttf DejaVuSansMono-Bold.ttf"

# The program the package ships and the resource pack it loads from beside itself
PROGRAM=content_shell
RESOURCE_PAK=content_shell.pak
LLVM_ROOT=/usr/lib/llvm22
RUST_TARGET_SUFFIX=alpine-linux-musl

# The persistent container and volume, and the resources the build may use
CONTAINER=stlx-chromium
VOLUME=stlx-chromium
BUILD_JOBS=12
BUILD_MEMORY=44g

package_version() {
    [ "$1" = chromium ] && echo "$CHROMIUM_VER-$CHROMIUM_PKG_REL"
}
