#!/bin/sh
# Runs inside the Alpine build container. Builds one architecture's
# static native GCC toolchain in two stages:
#   stage 1: a cross toolchain targeting <arch>-linux-musl
#   stage 2: that cross toolchain rebuilds everything as static
#            native binaries that run on the target itself
# Expects /work prepared by build.sh: mcm-stage1/, mcm-stage2/, and
# versions.sh. Installs into /work/stage1 and /work/out.
set -eu

ARCH="${1:?usage: container-build.sh <x86_64|aarch64>}"
TRIPLE="$ARCH-linux-musl"
. /work/versions.sh

apk add -q $ALPINE_PKGS

JOBS="$(nproc)"

# Stage 1: cross toolchain, runs on the build host
cat > /work/mcm-stage1/config.mak <<EOF
TARGET = $TRIPLE
OUTPUT = /work/stage1
GCC_VER = $GCC_VER
MUSL_VER = $MUSL_VER
GNU_SITE = $GNU_SITE
DL_CMD = curl -fL --retry 10 --retry-delay 3 -C - -o
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --enable-languages=c,c++
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-multilib
EOF

echo "=== stage 1: cross toolchain ($TRIPLE) ==="
make -C /work/mcm-stage1 -j"$JOBS"
make -C /work/mcm-stage1 install

# Stage 2: static native toolchain, runs on the target from PREFIX
PREFIX=/usr/gcc

cat > /work/mcm-stage2/config.mak <<EOF
TARGET = $TRIPLE
HOST = $TRIPLE
OUTPUT = /work/out
PREFIX = $PREFIX
GCC_VER = $GCC_VER
MUSL_VER = $MUSL_VER
GNU_SITE = $GNU_SITE
DL_CMD = curl -fL --retry 10 --retry-delay 3 -C - -o
COMMON_CONFIG += CC="$TRIPLE-gcc -static --static" CXX="$TRIPLE-g++ -static --static"
COMMON_CONFIG += CFLAGS="-g0 -Os" CXXFLAGS="-g0 -Os" LDFLAGS="-s -static --static"
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --enable-languages=c,c++
GCC_CONFIG += --disable-libquadmath --disable-decimal-float
GCC_CONFIG += --disable-multilib --disable-lto --disable-host-shared
GCC_CONFIG += --disable-shared
GCC_CONFIG += --disable-fixincludes
GCC_CONFIG += --with-native-system-header-dir=/include
BINUTILS_CONFIG += --disable-lto
MUSL_CONFIG += --disable-shared
EOF

echo "=== stage 2: static native toolchain ($TRIPLE) ==="
export PATH="/work/stage1/bin:$PATH"
make -C /work/mcm-stage2 -j"$JOBS"
make -C /work/mcm-stage2 install

# Documentation and fixinclude tooling never run on the target, and
# the kernel interface headers describe a different kernel entirely
rm -rf "/work/out$PREFIX/share"
rm -rf "/work/out$PREFIX/libexec/gcc/$TRIPLE/$GCC_VER/install-tools"
rm -rf "/work/out$PREFIX/include/linux" \
       "/work/out$PREFIX/include/asm" \
       "/work/out$PREFIX/include/asm-generic"

# The target has no dynamic loader, so the driver defaults to static
# linking through a specs file it loads from its own lib directory
cat > "/work/out$PREFIX/lib/gcc/$TRIPLE/$GCC_VER/specs" <<'SPECS'
*self_spec:
+ %{!static:%{!shared:%{!r:-static}}}
SPECS

echo "=== container build done ($TRIPLE) ==="
