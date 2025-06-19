#!/usr/bin/env bash
set -e

# Source environment variables
. "$(dirname "$0")/env.sh"

# Version variables (must match fetch-sources.sh)
BINUTILS=binutils-2.43
GCC=gcc-14.3.0
MUSL=musl-1.2.5

# Create necessary directories
mkdir -p "$PREFIX"
mkdir -p "$SYSROOT"

# Function to extract archives
extract() {
    local archive=$1
    if [[ $archive == *.tar.xz ]]; then
        tar xf "$archive"
    elif [[ $archive == *.tar.gz ]]; then
        tar xf "$archive"
    fi
}

# Build binutils
build_binutils() {
    echo "Building binutils..."
    cd "$(dirname "$0")/../src"
    extract "${BINUTILS}.tar.xz"
    cd "$BINUTILS"
    mkdir -p build
    cd build
    ../configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT --disable-nls --disable-werror
    make $make_flags
    make install
    cd ../..
}

# Build GCC (first stage)
build_gcc_stage1() {
    echo "Building GCC (stage 1)..."
    cd "$(dirname "$0")/../src"
    extract "${GCC}.tar.xz"
    cd "$GCC"
    ./contrib/download_prerequisites
    mkdir -p build
    cd build
    ../configure --target=$TARGET --prefix=$PREFIX --with-sysroot=$SYSROOT \
        --disable-nls --disable-werror --disable-multilib \
        --disable-libssp --disable-libgomp --disable-libmudflap \
        --disable-libstdcxx-pch --disable-libsanitizer \
        --disable-shared --enable-static \
        --enable-languages=c,c++ --with-newlib \
        --without-headers --with-system-zlib
    make $make_flags all-gcc
    make install-gcc
    cd ../..
}

# Build musl
build_musl() {
    echo "Building musl..."
    cd "$(dirname "$0")/../src"
    extract "${MUSL}.tar.gz"
    cd "$MUSL"
    ./configure --prefix=$SYSROOT/usr --target=$TARGET \
        --disable-shared --enable-static \
        --disable-stack-protector
    make $make_flags
    make install
    cd ..
}

# Build GCC (final stage)
build_gcc_final() {
    echo "Building GCC (final stage)..."
    cd "$(dirname "$0")/../src/$GCC/build"
    make $make_flags all-target-libgcc
    make install-target-libgcc
    cd ../..
}

# Main build process
echo "Starting toolchain build process..."
build_binutils
build_gcc_stage1
build_musl
build_gcc_final

echo "Toolchain build complete!"
echo "Your toolchain is installed in: $PREFIX"
echo "Add $PREFIX/bin to your PATH to use the new toolchain" 