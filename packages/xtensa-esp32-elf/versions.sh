# Version and source pins for the xtensa-esp32-elf recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version and RECIPE_TREE.

# Espressif's GCC and binutils for its chips, at the tags of the toolchain release ESP_RELEASE names,
# whose version the package takes
ESP_RELEASE="esp-14.2.0_20260121"
XTENSA_ESP32_ELF_VER="${ESP_RELEASE#esp-}"
GCC_VER="14.2.0"
GCC_URL="https://github.com/espressif/gcc/archive/refs/tags/$ESP_RELEASE.tar.gz"
GCC_SHA256="a1047d04f01cc21e6fe372d2e82092dab8bb89516e071ab2abae7a7f7202edd7"
BINUTILS_TAG="esp-2.43.1_20260121"
BINUTILS_URL="https://github.com/espressif/binutils-gdb/archive/refs/tags/$BINUTILS_TAG.tar.gz"
BINUTILS_SHA256="a2ecd29ce6f80ab36981883b3106279984cda6767caa636bf487cf5e64c04758"

# The source of Espressif's crosstool-NG for the same release, whose overlays/xtensa_esp32 holds the
# ESP32's core configuration for GCC and binutils
ESP_DOWNLOADS="https://github.com/espressif/crosstool-NG/releases/download/$ESP_RELEASE"
CTNG_URL="$ESP_DOWNLOADS/crosstool-NG-$ESP_RELEASE-src.tar.xz"
CTNG_SHA256="ac107a1a7aa941ab1e35e17dc08bde3ef2b468172ca0eb9ff788a1193b80dbb5"

# Espressif's Linux build of the release, which supplies the ESP32-side libraries: newlib, libgcc
# and libstdc++ in their esp32 variants
RELEASE_URL="$ESP_DOWNLOADS/xtensa-esp-elf-$XTENSA_ESP32_ELF_VER-x86_64-linux-gnu.tar.xz"
RELEASE_SHA256="da31f36d79d4e99f24e55a90a71e65d5694714f16199960bf7885724b706a48c"

# The libraries GCC's own programs need, at the versions its contrib/download_prerequisites names
GNU_SITE="https://mirrors.kernel.org/gnu"
GMP_FILE="gmp-6.2.1.tar.bz2"
GMP_SHA256="eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c"
MPFR_FILE="mpfr-4.1.0.tar.bz2"
MPFR_SHA256="feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926"
MPC_FILE="mpc-1.2.1.tar.gz"
MPC_SHA256="17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459"

# The compiler for Stellux that builds the package's programs: musl-cross-make's newest GCC 14, the
# series of the GCC it builds, with the musl version the tree builds, which build.sh checks
MCM_COMMIT="227df8b99103f9c59f6570babf892978e293082f"
MCM_URL="https://github.com/richfelker/musl-cross-make/archive/$MCM_COMMIT.tar.gz"
MCM_SHA256="bb3fc7851088e1e5e1274ee56a0ab6ae176043d160fdf0b71027934b091f208a"
CROSS_GCC_VER="14.3.0"
MUSL_VER="1.2.5"

# The tree paths the package builds from, which the recipe's hash covers: the musl patches and each
# libstlx header they include, which reaches the container only when named here
STLX_HEADERS="userland/lib/libstlx/include"
RECIPE_TREE="userland/musl/patches $STLX_HEADERS/stlx/proc.h $STLX_HEADERS/stlx/syscall_nums.h"

# Where the toolchain installs, and a link for Arduino builds to find it through, named by the
# packager, tool name and version that Arduino-ESP32's package index gives this toolchain
TARGET="xtensa-esp32-elf"
PREFIX="/usr/$TARGET"
ARDUINO_TOOL="/usr/arduino/tools/esp32/esp-x32/2601"

# Package release number, bumped when the recipe or the tree paths it builds from change without the
# toolchain release changing
XTENSA_ESP32_ELF_PKG_REL="1"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    xtensa-esp32-elf) echo "$XTENSA_ESP32_ELF_VER-$XTENSA_ESP32_ELF_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest, and the packages the builds need inside it
ALPINE_IMAGE="alpine:3.22@sha256:14358309a308569c32bdc37e2e0e9694be33a9d99e68afb0f5ff33cc1f695dce"
ALPINE_PKGS="build-base curl xz zstd tar patch linux-headers bison flex texinfo"
