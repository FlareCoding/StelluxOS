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

# Stage 2: static native toolchain, runs on the target
cat > /work/mcm-stage2/config.mak <<EOF
TARGET = $TRIPLE
HOST = $TRIPLE
OUTPUT = /work/out
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
BINUTILS_CONFIG += --disable-lto
EOF

echo "=== stage 2: static native toolchain ($TRIPLE) ==="
export PATH="/work/stage1/bin:$PATH"
make -C /work/mcm-stage2 -j"$JOBS"
make -C /work/mcm-stage2 install

# Documentation and fixinclude tooling never run on the target
rm -rf /work/out/share
rm -rf /work/out/libexec/gcc/$TRIPLE/$GCC_VER/install-tools

echo "=== container build done ($TRIPLE) ==="
