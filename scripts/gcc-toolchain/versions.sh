# Version and source pins for the on-target GCC toolchain build.
# Sourced by build.sh and container-build.sh.

# musl-cross-make provides the musl targeting patches and the build
# system for every component below.
MCM_REPO="https://github.com/richfelker/musl-cross-make.git"
MCM_COMMIT="227df8b99103f9c59f6570babf892978e293082f"

# GCC 14.3.0 is the newest release with a full patch set in the pinned
# musl-cross-make commit. musl matches the version in the repo sysroot.
GCC_VER="14.3.0"
MUSL_VER="1.2.5"

# Downloads come from the kernel.org mirror, since the GNU primary
# intermittently serves errors long enough to outlast curl retries.
GNU_SITE="https://mirrors.kernel.org/gnu"

# Build container and the packages the build needs inside it
ALPINE_IMAGE="alpine:3.22"
ALPINE_PKGS="build-base curl xz patch linux-headers texinfo bash git rsync zstd"
