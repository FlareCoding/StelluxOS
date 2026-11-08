# Version and source pins for the vk-gl-cts recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version.

# VK-GL-CTS, whose dEQP-VK the package holds: the vulkan-cts release Mesa 26.2.4's CI builds for NVK
VK_GL_CTS_VER="1.4.6.1"
VK_GL_CTS_URL="https://github.com/KhronosGroup/VK-GL-CTS/archive/refs/tags/vulkan-cts-$VK_GL_CTS_VER.tar.gz"
VK_GL_CTS_SHA512="2c46643f56a7a0ddb25487b7617d7511654be5714ff575d339a606c2d8c9090572fa12ef8d7ef5de67580a42ecd9294bb72b3ee8dd2c95fd32299ab90de7a77d"

# The commits Mesa's CI backports onto the release, applied in this order from GitHub's patch of
# each: <commit> <sha512 of the patch>
VK_GL_CTS_COMMIT_URL="https://github.com/KhronosGroup/VK-GL-CTS/commit"
VK_GL_CTS_BACKPORTS="
6368ee8503dd9ca46eabfa2df293075d9034a214 9c1d6be7a7913111e6dd266aa326a210fb3d0fdc9ff1d1b24d7e85e512c965c2e11f6afc121a7417ce233afab70b2a5df9a6d56e7320fae414b428545a5655dc
541ed0874565d642069c59fe3b31fc42f495a470 4dd843914429053ea17f7022fe775970129e272fced4389f2d4e1bb9b8be2e8b82ed25c5b2fedfbb82020b870de5b15ed68579ec652482f7971d3f8809cc4998
629745a58d31bc5c810f014c0975e176e2791ae0 89d2c971e652f2ab46f5416d7c993ba64444ea2e9b20d5fa9d77e50cced13afb7d0f1516c93b6662168812fe7183ac5bb2c14c58a5c2e8e063b868f3d97006d7
ded32883bf36e5bdf7ac6b0512d5314adc0849d4 8ee323fbc42c79deb4a5a68e4ec06977be1eaeef10cbb240e392268cdb641a78b99470feec67edfd65307b4284702e320559b7c06b250d1ddc7595528613e372
"

# The sources dEQP-VK builds from, at the archives and commits the release's external/fetch_sources.py
# pins, each unpacked into external/<name>/src as the script does: <name> <url> <sha512>
VK_GL_CTS_EXTERNALS="
zlib https://github.com/madler/zlib/releases/download/v1.2.13/zlib-1.2.13.tar.gz 99f0e843f52290e6950cc328820c0f322a4d934a504f66c7caa76bd0cc17ece4bf0546424fc95135de85a2656fed5115abb835fd8d8a390d60ffaf946c8887ad
libpng https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz 34c806e0dda960b480ce2f5ea13e2e55a9540f07c51948be25d312b901c431bc814f730f9322a2e3b6f88d4104a0c49bde9e616762b342d07db44e2c7fd5f2dc
spirv-tools https://github.com/KhronosGroup/SPIRV-Tools/archive/2d14d2e76aa7de72404b17078eda15c20a6a0389.tar.gz dfca6dba8b5e2ad37df314e77be66f4cd1d01cebfee36973c39dbf81e07f6655dd32ca36f8f8c5e4d4dea09b386acb440cf7d2c8c62e3cbb373a04279ca707c5
glslang https://github.com/KhronosGroup/glslang/archive/715c8500e7cd67f2eba9e60e98852a1ed49d2f15.tar.gz 08010ca38d9aaf8488ca96021b77fe5ec2d87ec345978c5d1f83fcbdc6129993e7279b2b7074f918c6fa3ad618628d437701c6db20bde217f0eebffa07f0b8da
spirv-headers https://github.com/KhronosGroup/SPIRV-Headers/archive/6dd7ba990830f7c15ac1345ff3b43ef6ffdad216.tar.gz 4f6ae3af7e75e8a9d045024aac378395d08f42a8fe3c2a35142c23a40e3e5f4d6fdc46c537251d6174ee89581daf1ad63746c3c48a7a9d2a690271e6d94f08fc
amber https://github.com/google/amber/archive/53a4c8934bf7335d27c694e9fdac9ae1b180c0d2.tar.gz 2d8d5bf3a68a3b836aa8e8a65436ebbf26ffe2ee5adfbe27e235abfcf693997c889d675d6c006288c640cbd7728454ac66a56026ef016cc6e7967005d730eb64
jsoncpp https://github.com/open-source-parsers/jsoncpp/archive/9059f5cad030ba11d37818847443a53918c327b1.tar.gz 8062c83cad9dc453f1eb4886e63e054570e0f29dcd6594330d3b3628ba994915e26e08690cd28118805a766be200ac99ad4fbc131db3af895122a8d1bd87ef31
"

# The tree's make targets build the sysroot from these, which build.sh holds to the
# Makefile's versions. The checksums verify downloads the Makefile leaves unverified.
MUSL_VER="1.2.5"
MUSL_URL="https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz"
MUSL_SHA512="7bb7f7833923cd69c7a1a9b8a5f1784bfd5289663eb6061dcd43d583e45987df8a68a1be05d75cc1c88a3f5b610653d1a70f4a9cff4d8f7fd41ae73ee058c17c"
LLVM_VER="20.1.8"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/llvm-project-$LLVM_VER.src.tar.xz"
LLVM_SHA512="f330e72e6a1da468569049437cc0ba7a41abb816ccece7367189344f7ebfef730f4788ac7af2bef0aa8a49341c15ab1d31e941ffa782f264d11fe0dc05470773"

# Package release number, bumped when the recipe or the pinned mesa kit changes without VK-GL-CTS changing
VK_GL_CTS_PKG_REL="2"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    vk-gl-cts) echo "$VK_GL_CTS_VER-$VK_GL_CTS_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest: the CI image's distribution, since the tree's sysroot targets
# expect an x86_64 Debian host. The CTS's Linux platform requires pkgconf, unused by the headless target.
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="build-essential clang lld llvm libclang-rt-dev gcc-aarch64-linux-gnu linux-libc-dev-arm64-cross cmake ninja-build python3 perl curl ca-certificates xz-utils zstd git file pkgconf"
