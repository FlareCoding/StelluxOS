# Version and source pins for the developer packages.
# Sourced by build.sh on the host and by container/build.sh inside.

# musl-cross-make provides the musl targeting patches and the build
# system for every component below.
MCM_REPO="https://github.com/richfelker/musl-cross-make.git"
MCM_COMMIT="227df8b99103f9c59f6570babf892978e293082f"

# GCC 14.3.0 is the newest release with a full patch set in the pinned
# musl-cross-make commit. musl matches the version in the repo sysroot.
# binutils 2.45 is newer than the pinned default, so hashes/ carries its
# checksum for the download.
GCC_VER="14.3.0"
BINUTILS_VER="2.45"
MUSL_VER="1.2.5"

# Package release numbers, bumped when a recipe changes without its
# upstream version changing
GCC_PKG_REL="1"
BINUTILS_PKG_REL="1"

# Downloads come from the kernel.org mirror, since the GNU primary
# intermittently serves errors long enough to outlast curl retries.
GNU_SITE="https://mirrors.kernel.org/gnu"

# Build container, pinned by digest so every host builds from the same
# image, and the packages the build needs inside it
ALPINE_IMAGE="alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce"
ALPINE_PKGS="build-base curl xz zstd tar patch linux-headers texinfo bash git rsync"
