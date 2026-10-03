# Version and source pins for the qt recipe. Sourced by build.sh on the
# host, by container.sh inside, and by the dispatcher for package_version.

# Qt modules the kit holds: qtbase for Core, Gui and Widgets, qtsvg for SVG icons
QT_VER="6.11.2"
QT_URL="https://download.qt.io/official_releases/qt/${QT_VER%.*}/$QT_VER/submodules"
QTBASE_SHA256="5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22"
QTSVG_SHA256="d594337feca84c26fb67fe87b85e6a5c12fda404b611d905f9d138210c311876"

# The tree's make targets build the sysroot from these, which build.sh holds to the
# Makefile's versions. The checksums verify downloads the Makefile leaves unverified.
MUSL_VER="1.2.5"
MUSL_SHA256="a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
LLVM_VER="20.1.8"
LLVM_SHA256="6898f963c8e938981e6c4a302e83ec5beb4630147c7311183cf61069af16333d"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"

# Package release number, bumped when the recipe changes without Qt changing
QT_PKG_REL="1"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    qt) echo "$QT_VER-$QT_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest. It is the CI image's distribution and
# package set, since the tree's sysroot targets expect an x86_64 Debian host.
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="build-essential clang lld llvm libclang-rt-dev gcc-aarch64-linux-gnu linux-libc-dev-arm64-cross cmake ninja-build python3 perl curl ca-certificates xz-utils zstd git file"
