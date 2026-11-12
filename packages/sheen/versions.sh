# Version and source pins for the sheen recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version.

# Sheen, built from userland/apps/sheen, raised whenever a change there should reach the package
SHEEN_VER="1.2"

# Scintilla, the editing component, and Lexilla, its lexers, compiled unmodified from their release tarballs
SCINTILLA_VER="5.6.9"
SCINTILLA_URL="https://www.scintilla.org/scintilla569.tgz"
SCINTILLA_SHA256="1622546ce1d3804b589292f1adae98ad1d87fa8c43bb401edc098adae12dcacb"
LEXILLA_VER="5.5.5"
LEXILLA_URL="https://www.scintilla.org/lexilla555.tgz"
LEXILLA_SHA256="fa4093ad5c48acf49a6f3eca1b75143a5c824ef1a6f5b0ee59e8c3d6a970f229"

# The tree's make targets build the sysroot from these, which build.sh holds to the
# Makefile's versions. The checksums verify downloads the Makefile leaves unverified.
MUSL_VER="1.2.5"
MUSL_SHA256="a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
LLVM_VER="20.1.8"
LLVM_SHA256="6898f963c8e938981e6c4a302e83ec5beb4630147c7311183cf61069af16333d"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"

# Package release number, bumped when the recipe, a pin or the Qt kit changes without Sheen's version changing
SHEEN_PKG_REL="1"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    sheen) echo "$SHEEN_VER-$SHEEN_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest. It is the CI image's distribution and
# package set, since the tree's sysroot targets expect an x86_64 Debian host.
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="build-essential clang lld llvm libclang-rt-dev gcc-aarch64-linux-gnu linux-libc-dev-arm64-cross cmake ninja-build python3 perl curl ca-certificates xz-utils zstd git file"
