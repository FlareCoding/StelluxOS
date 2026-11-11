APP_NAME := stlxpkg
APP_LIBS := stlxhttp bearssl zstd
include ../../mk/app.mk
include $(USERLAND_ROOT)/../packages/packages.mk

# The release host comes from packages.lock and the build tier from packages.conf, so each is stated once
CFLAGS_COMMON += \
	-DSTLXPKG_RELEASES_URL='"$(PACKAGES_SOURCE)"' \
	-DSTLXPKG_LATEST_URL='"$(patsubst %/download,%/latest/download,$(PACKAGES_SOURCE))"' \
	-DSTLXPKG_BUILD_PACKAGES='"$(PACKAGES_BUILD)"'

$(OBJECTS): $(PACKAGES_LOCK) $(PACKAGES_REGISTRY)
