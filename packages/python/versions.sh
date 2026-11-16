# Version and source pins for the python recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version.

# CPython, cross-built with the container's python3 as the build interpreter, which must share its major.minor version
PYTHON_VER="3.12.8"
PYTHON_URL="https://www.python.org/ftp/python/$PYTHON_VER/Python-$PYTHON_VER.tar.xz"
PYTHON_SHA256="c909157bb25ec114e5869124cc2a9c4a4d4c1e957ca4ff553f1edc692101154e"

# zlib, built static for CPython's zlib and binascii modules, checked against the SHA-256 zlib.net publishes
ZLIB_VER="1.3.2"
ZLIB_URL="https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz"
ZLIB_SHA256="bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16"

# The tree's make targets build the sysroot from these, which build.sh holds to the
# Makefile's versions. The checksums verify downloads the Makefile leaves unverified.
MUSL_VER="1.2.5"
MUSL_SHA256="a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
LLVM_VER="20.1.8"
LLVM_SHA256="6898f963c8e938981e6c4a302e83ec5beb4630147c7311183cf61069af16333d"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"

# Package release number, bumped when the recipe changes without CPython's version changing
PYTHON_PKG_REL="2"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    python) echo "$PYTHON_VER-$PYTHON_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest. It is the CI image's distribution, since the tree's sysroot targets expect an
# x86_64 Debian host, and its python3 is 3.12.
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="build-essential clang lld llvm cmake python3 linux-libc-dev-arm64-cross xz-utils zstd"

# The Apache License 2.0 text the image's base-files ships, which the package carries for HACL*'s KaRaMeL headers.
# It equals the text apache.org publishes.
APACHE_LICENSE_SHA256="cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
