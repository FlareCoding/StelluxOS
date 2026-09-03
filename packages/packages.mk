# Resolves Stellux developer package pins from packages.lock for the target
# ARCH. Included by the top-level and userland Makefiles, see README.md.

PACKAGES ?=

PACKAGES_DIR   := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
PACKAGES_LOCK  := $(PACKAGES_DIR)/packages.lock
PACKAGES_CACHE := $(PACKAGES_DIR)/../userland/toolchain/packages

PACKAGES_SOURCE  := $(shell awk '$$1 == "source" { print $$2 }' $(PACKAGES_LOCK))
PACKAGES_RELEASE := $(shell awk '$$1 == "release" { print $$2 }' $(PACKAGES_LOCK))

# pkg_field(name, column) reads one column of the package's entry for ARCH
pkg_field   = $(shell awk -v n=$(1) -v a=$(ARCH) '$$1 == n && $$3 == a { print $$$(2) }' $(PACKAGES_LOCK))
pkg_version = $(call pkg_field,$(1),2)
pkg_sha256  = $(call pkg_field,$(1),4)
pkg_file    = $(1)-$(call pkg_version,$(1))-$(ARCH).tar.zst
pkg_url     = $(PACKAGES_SOURCE)/$(PACKAGES_RELEASE)/$(call pkg_file,$(1))

# Fails the build early when a requested package has no pin for ARCH
define check_package
$(if $(call pkg_version,$(1)),,$(error package '$(1)' has no $(ARCH) entry in $(PACKAGES_LOCK)))
endef
