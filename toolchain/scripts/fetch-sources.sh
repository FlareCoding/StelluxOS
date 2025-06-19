#!/usr/bin/env bash
set -e
. "$(dirname "$0")/env.sh"
mkdir -p ../src
cd ../src

# URLs (update versions here only)
BINUTILS=binutils-2.43
GCC=gcc-14.3.0
MUSL=musl-1.2.5

download() {
  local url=$1
  [ -f $(basename $url) ] || curl -LO "$url"
}

download "https://ftp.gnu.org/gnu/binutils/${BINUTILS}.tar.xz"
download "https://ftp.gnu.org/gnu/gcc/${GCC}/${GCC}.tar.xz"
download "https://musl.libc.org/releases/${MUSL}.tar.gz"
