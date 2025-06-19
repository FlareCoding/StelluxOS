#!/usr/bin/env bash
export TARGET=x86_64-linux-musl
export PREFIX=$(pwd)/../dist
export SYSROOT=$PREFIX/sysroot
export PATH="$PREFIX/bin:$PATH"
export STELLUX_CFLAGS="-mcmodel=large -ffreestanding -static -fno-stack-protector -fno-builtin -fno-strict-aliasing -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables -fno-ident -fno-exceptions -fno-rtti"
export STELLUX_CXXFLAGS="$STELLUX_CFLAGS"
export STELLUX_LDFLAGS="-static -nostdlib -z nodefaultlib -z noexecstack"
make_flags="-j$(nproc)"
