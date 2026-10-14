# Version and source pins for the nvidia-firmware recipe. Sourced by build.sh on the
# host, by container.sh inside, and by the dispatcher for package_version.

# The firmware version, and NVIDIA's driver package holding its GSP image and NVIDIA's license
# agreement
NVIDIA_FW_VER="535.183.01"
DRIVER_URL="https://download.nvidia.com/XFree86/Linux-x86_64/$NVIDIA_FW_VER/NVIDIA-Linux-x86_64-$NVIDIA_FW_VER.run"
DRIVER_SHA256="f6707afbdda9407e3cbc2e5128e60bcbcdbf02fae29958c72fafb5d405e8b883"

# NVIDIA's license agreement in the driver package, pinned so a version with another agreement
# stops the build and its text is read before the pin moves
LICENSE_SHA256="5ac626496a6388d37cdd53e0300525267a4b8c89fb5980da060783afd561faf7"

# The commit NVIDIA's open kernel modules tag with the firmware version, and the files taken from
# it: NVIDIA's extraction tool and the generated files holding GA102's bootloader and booters
OPEN_MODULES_COMMIT="4459285b60d72ef69ec1876551fcf5c0fef6b9dd"
OPEN_MODULES_URL="https://raw.githubusercontent.com/NVIDIA/open-gpu-kernel-modules/$OPEN_MODULES_COMMIT"
TOOL="nouveau/extract-firmware-nouveau.py"
TOOL_SHA256="56016c9d89d38a9f6b927ba255abb0775204e45ab284b83106604ae42c1fb19d"
BOOTLOADER_SRC="src/nvidia/generated/g_bindata_kgspGetBinArchiveGspRmBoot_GA102.c"
BOOTLOADER_SRC_SHA256="122591d38ae19f6d4bfd9bbc841abae2c9396032e0961416f865e50dea891dcb"
BOOTER_LOAD_SRC="src/nvidia/generated/g_bindata_kgspGetBinArchiveBooterLoadUcode_GA102.c"
BOOTER_LOAD_SRC_SHA256="fe1dff288a8e3cfa92e3c125e66032fcb3dd8c04abe4e878751f32f72ea94030"
BOOTER_UNLOAD_SRC="src/nvidia/generated/g_bindata_kgspGetBinArchiveBooterUnloadUcode_GA102.c"
BOOTER_UNLOAD_SRC_SHA256="4c9c3642f41c437000ebced34e406a9bb7ca207537e002da1c412d4574edac18"

# Package release number, bumped when the recipe changes without the firmware changing
NVIDIA_FW_PKG_REL="1"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    nvidia-firmware) echo "$NVIDIA_FW_VER-$NVIDIA_FW_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest: Python runs the extraction tool, and zstd unpacks the
# driver package and compresses the archive
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="python3 zstd"
