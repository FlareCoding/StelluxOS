# Version and source pins for the mesa recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version.

# Mesa, whose Vulkan driver for NVIDIA GPUs the kit holds, checked against the SHA512 its release
# notes publish
MESA_VER="26.2.4"
MESA_URL="https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
MESA_SHA512="42e793561661a567e72c7324d071ab9f82d3a9f1e000827ed989b8d5fcc2cd16e50c02808d43465b3135c25a3cad77649ba0bf9e35d95b848f8fc8d8b9ce327c"

# Libraries Mesa needs that the sysroot lacks, built static. libdrm is the release Mesa's own wrap
# pins, and elfutils' configure needs argp, fts and obstack, three parts of glibc that musl leaves out.
LIBDRM_VER="2.4.133"
LIBDRM_URL="https://dri.freedesktop.org/libdrm/libdrm-$LIBDRM_VER.tar.xz"
LIBDRM_SHA512="2b8016f64d2553c6cec4870b01820c48eb3985cadc4e39846bcc54db6bcec9435f65ba7749fc8271020317489bc05625e991383569cba803cca85c686d1f4568"
ZLIB_VER="1.3.2"
ZLIB_URL="https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz"
ZLIB_SHA512="70963771ea5d763614278a69b474f09b7d237ef8f53b675a10fe31d9923aeef601504b35d7ebd1b1e7f347e9ebb048e6b3b47fffdf137e7bdc7e8d5eb4ec4692"
ELFUTILS_VER="0.195"
ELFUTILS_URL="https://sourceware.org/elfutils/ftp/$ELFUTILS_VER/elfutils-$ELFUTILS_VER.tar.bz2"
ELFUTILS_SHA512="e1e1fdf4f7f72bf520deb0103bb8fd208dc247bab14af100c6fb56d38c0ef6c6d54ff5bd15b84e4f70e2b2ea481e5999fea2842360f8932a861122df79b5fc8f"
ARGP_VER="1.5.0"
ARGP_URL="https://github.com/argp-standalone/argp-standalone/archive/refs/tags/$ARGP_VER.tar.gz"
ARGP_SHA512="fa2eb61ea00f7a13385e5c1e579dd88471d6ba3a13b6353e924fe71914b90b40688b42a9f1789bc246e03417fee1788b1990753cda8c8d4a544e85f26b63f9e2"
FTS_VER="1.2.7"
FTS_URL="https://github.com/void-linux/musl-fts/archive/refs/tags/v$FTS_VER.tar.gz"
FTS_SHA512="949f73b9406b06bd8712c721b4ec89afcb37d4eaef5666cccf3712242d3a57fc0acf3ca994934e0f57c1e92f40521a9370132a21eb6d1957415a83c76bf20feb"
OBSTACK_VER="1.2.3"
OBSTACK_URL="https://github.com/void-linux/musl-obstack/archive/refs/tags/v$OBSTACK_VER.tar.gz"
OBSTACK_SHA512="b2bbed19c4ab2714ca794bdcb1a84fad1af964e884d4f3bbe91c9937ca089d92b8472cb05ebe998a9f5c85cb922b9b458db91eff29077bd099942e1ce18e16cc"

# The tree's make targets build the sysroot from these, which build.sh holds to the
# Makefile's versions. The checksums verify downloads the Makefile leaves unverified.
MUSL_VER="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
MUSL_SHA512="7bb7f7833923cd69c7a1a9b8a5f1784bfd5289663eb6061dcd43d583e45987df8a68a1be05d75cc1c88a3f5b610653d1a70f4a9cff4d8f7fd41ae73ee058c17c"
LLVM_VER="20.1.8"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"
LLVM_SHA512="f330e72e6a1da468569049437cc0ba7a41abb816ccece7367189344f7ebfef730f4788ac7af2bef0aa8a49341c15ab1d31e941ffa782f264d11fe0dc05470773"

# The Rust toolchain and the binding generators NAK and NIL build with, and Meson, at the versions
# Mesa's own CI uses at this release. Mesa's CI takes the current rustup, which this recipe pins.
RUSTUP_VER="1.29.3"
RUSTUP_SHA256="b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55"
RUST_VER="1.96.0"
BINDGEN_VER="0.72.1"
CBINDGEN_VER="0.26.0"
MESON_VER="1.7.0"
MESON_SHA256="ae3f12953045f3c7c60e27f2af1ad862f14dee125b4ed9bcb8a842a5080dbf85"

# Package release number, bumped when the recipe, GPU_ABI_VERSION or STLXWIN_GPU_ABI_VERSION changes but Mesa does not
MESA_PKG_REL="8"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    mesa) echo "$MESA_VER-$MESA_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest: the CI image's distribution, since the sysroot targets expect an x86_64
# Debian host. It adds the SPIR-V libraries Mesa's OpenCL compiler links and autotools for argp, fts and obstack.
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="build-essential clang lld llvm libclang-rt-dev gcc-aarch64-linux-gnu linux-libc-dev-arm64-cross cmake ninja-build python3 perl curl ca-certificates xz-utils bzip2 zstd git file pkgconf python3-pip python3-mako python3-yaml python3-packaging flex bison llvm-dev libclang-dev libclang-cpp-dev libllvmspirvlib-18-dev spirv-tools zlib1g-dev autoconf automake libtool m4 gawk"
