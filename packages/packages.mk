# Resolves the packages PACKAGES selects against the registry, and their pins
# from packages.lock for the target ARCH. Included by both Makefiles, see README.md.

PACKAGES ?=

PACKAGES_DIR      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PACKAGES_REGISTRY := $(PACKAGES_DIR)/packages.conf
PACKAGES_LOCK     := $(PACKAGES_DIR)/packages.lock
PACKAGES_CACHE    := $(abspath $(PACKAGES_DIR)/../userland/toolchain/packages)

PACKAGES_ALL     := $(shell awk '$$1 !~ /^\043/ && NF { print $$1 }' $(PACKAGES_REGISTRY))
PACKAGES_DEFAULT := $(shell awk '$$1 !~ /^\043/ && $$2 == "default" { print $$1 }' $(PACKAGES_REGISTRY))

# PACKAGES is a selection over the default tier: a name adds a package, -name
# removes one, and `none` or `all` replaces the default tier as the base
packages_base    := $(if $(filter all,$(PACKAGES)),$(PACKAGES_ALL),$(PACKAGES_DEFAULT))
packages_base    := $(if $(filter none,$(PACKAGES)),,$(packages_base))
packages_added   := $(filter-out none all -%,$(PACKAGES))
packages_removed := $(patsubst -%,%,$(filter -%,$(PACKAGES)))
packages_unknown := $(filter-out $(PACKAGES_ALL),$(packages_added) $(packages_removed))

$(if $(and $(filter none,$(PACKAGES)),$(filter all,$(PACKAGES))),$(error PACKAGES cannot select both none and all))
$(if $(packages_unknown),$(error unknown package(s) '$(packages_unknown)', $(PACKAGES_REGISTRY) knows: $(PACKAGES_ALL)))

PACKAGES_SELECTED := $(filter-out $(packages_removed),$(sort $(packages_base) $(packages_added)))

PACKAGES_SOURCE  := $(shell awk '$$1 == "source" { print $$2 }' $(PACKAGES_LOCK))
PACKAGES_RELEASE := $(shell awk '$$1 == "release" { print $$2 }' $(PACKAGES_LOCK))

# pkg_field(name, column) reads one column of the package's entry for ARCH
pkg_field   = $(shell awk -v n=$(1) -v a=$(ARCH) '$$1 == n && $$3 == a { print $$$(2) }' $(PACKAGES_LOCK))
pkg_tier    = $(shell awk -v n=$(1) '$$1 == n { print $$2 }' $(PACKAGES_REGISTRY))
pkg_recipe  = $(shell awk -v n=$(1) '$$1 == n { print $$3 }' $(PACKAGES_REGISTRY))

# A package is taken from the pin, unless its recipe now names a version the lock
# does not pin and the cache holds that build: an unpublished local build, used as is
define resolve_package
pkg_pinned_$(1)  := $$(call pkg_field,$(1),2)
pkg_sha256_$(1)  := $$(call pkg_field,$(1),4)
pkg_current_$(1) := $$(shell . $(PACKAGES_DIR)/$$(call pkg_recipe,$(1))/versions.sh && package_version $(1))
pkg_built_$(1)   := $$(wildcard $(PACKAGES_CACHE)/$(1)-$$(pkg_current_$(1))-$(ARCH).tar.zst)
pkg_local_$(1)   := $$(if $$(filter-out $$(pkg_pinned_$(1)),$$(pkg_current_$(1))),$$(pkg_built_$(1)))
endef
$(foreach p,$(PACKAGES_SELECTED),$(eval $(call resolve_package,$(p))))

pkg_is_local = $(pkg_local_$(1))
pkg_version  = $(if $(call pkg_is_local,$(1)),$(pkg_current_$(1)),$(pkg_pinned_$(1)))
pkg_sha256   = $(pkg_sha256_$(1))
pkg_file     = $(1)-$(call pkg_version,$(1))-$(ARCH).tar.zst
pkg_url      = $(PACKAGES_SOURCE)/$(PACKAGES_RELEASE)/$(call pkg_file,$(1))
pkg_origin   = $(if $(call pkg_is_local,$(1)),unpublished local build,$(call pkg_url,$(1)))

# Fails the build early when a selected package has neither a pin for ARCH nor a local build
define check_package
$(if $(call pkg_version,$(1)),,$(error package '$(1)' has no $(ARCH) entry in $(PACKAGES_LOCK) \
	and no build in $(PACKAGES_CACHE)))
endef
