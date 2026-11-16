# Version and source pins for the esptool recipe. Sourced by build.sh on the host, by
# container.sh inside, and by the dispatcher for package_version.

PYPI_FILES_URL="https://files.pythonhosted.org/packages"

# esptool, from its source release, since it publishes no wheel
ESPTOOL_VER="5.6.0"
ESPTOOL_URL="$PYPI_FILES_URL/source/e/esptool/esptool-$ESPTOOL_VER.tar.gz"
ESPTOOL_SHA256="86c7249e9cc3a824e032ede9ba20abad057a2c49e231418cc4f02b177bb36f42"

# pyserial's version, shared by its wheel below and its source release, which holds the license text the wheel lacks
PYSERIAL_VER="3.5"
PYSERIAL_URL="$PYPI_FILES_URL/source/p/pyserial/pyserial-$PYSERIAL_VER.tar.gz"
PYSERIAL_SHA256="3c77e014170dfffbd816e6ffc205e9842efb10be9f58ec16d3e8675b4925cddb"

# Wheels of the modules esptool imports, each URL followed by its SHA-256. esptool's other requirements serve espefuse,
# espsecure and IDE integration, and rich's serve Markdown and syntax highlighting, none of which esptool uses.
WHEELS="
$PYPI_FILES_URL/py3/e/esp_pylib/esp_pylib-1.2.1-py3-none-any.whl fb05017676200e38dc182013c994e62d9088d377081c88562e14a2f5aaae6456
$PYPI_FILES_URL/py2.py3/p/pyserial/pyserial-$PYSERIAL_VER-py2.py3-none-any.whl c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0
$PYPI_FILES_URL/py3/c/click/click-8.5.1-py3-none-any.whl 8bb5376d2006950386f8c6427613334a2acadc5f559cb80e638d00b4df957e37
$PYPI_FILES_URL/py3/r/rich_click/rich_click-1.9.9-py3-none-any.whl 365e7a9d0adb42e41ea832a0a12e02c44c079a26536dee688125eb9814f97274
$PYPI_FILES_URL/py3/r/rich/rich-15.0.0-py3-none-any.whl 33bd4ef74232fb73fe9279a257718407f169c09b78a87ad3d296f548e27de0bb
$PYPI_FILES_URL/py2.py3/i/intelhex/intelhex-2.3.0-py2.py3-none-any.whl 87cc5225657524ec6361354be928adfd56bcf2a3dcc646c40f8f094c39c07db4
"

# The directory under /usr/lib of the python package's Python, which build.sh holds to packages/python's version
PYTHON_LIB="python3.12"

# packages/python's pins, which this recipe's hash covers, so a change there marks the published archive stale until
# the release is bumped and build.sh checks PYTHON_LIB again
RECIPE_TREE="packages/python/versions.sh"

# Package release number, bumped when the recipe, a module or python's pins change without esptool's version changing
ESPTOOL_PKG_REL="1"

# The version-release of each package this recipe emits, as the archive names it
package_version() {
    case "$1" in
    esptool) echo "$ESPTOOL_VER-$ESPTOOL_PKG_REL" ;;
    *) return 1 ;;
    esac
}

# Build container, pinned by digest: Python unpacks the wheels and zstd compresses the archive
UBUNTU_IMAGE="ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60"
UBUNTU_PKGS="python3 zstd"
