# Fetches the selected packages into the rootfs overlay and every build package into the
# SDK the tree compiles against. Archives are cached in userland/toolchain/packages.

include $(USERLAND_ROOT)/../packages/packages.mk

comma := ,

PACKAGES_STATE := $(USERLAND_ROOT)/build/$(ARCH)/packages
PACKAGES_ROOTFS := $(USERLAND_ROOT)/build/$(ARCH)/rootfs
PACKAGES_STAGED := $(filter-out $(PACKAGES_BUILD),$(PACKAGES_SELECTED))

# The SDK keeps its state beside it, since make clean leaves it in place like the cache
PACKAGES_SDK := $(USERLAND_ROOT)/toolchain/sdk/$(ARCH)
PACKAGES_SDK_STATE := $(PACKAGES_SDK)/.packages

pkg_archive = $(PACKAGES_CACHE)/$(call pkg_file,$(1))
pkg_stamp   = $(2)/$(1)-$(call pkg_version,$(1)).stamp
pkg_list    = $(2)/$(1).files

# Removes what a package unpacked under root $(2), the directories it left empty, and its
# stamps in state $(3), since a switch to another version must unpack again
define pkg_remove
if [ -f $(call pkg_list,$(1),$(3)) ]; then \
	(cd $(2) && \
		grep -v '/$$' $(call pkg_list,$(1),$(3)) | xargs rm -f && \
		grep '/$$' $(call pkg_list,$(1),$(3)) | sort -r | xargs rmdir 2>/dev/null; true); \
	rm -f $(call pkg_list,$(1),$(3)) $(3)/$(1)-*.stamp; \
fi
endef

# Download once into the cache, keeping only archives that match their pin
define package_fetch_rule
$(call pkg_archive,$(1)):
	$(UQ)[ -n "$(PACKAGES_RELEASE)" ] || \
		{ echo "packages: $(1) is not cached and packages.lock pins no release"; exit 1; }
	$(UQ)mkdir -p $(PACKAGES_CACHE)
	$(UQ)echo "[PKG] fetching $(call pkg_file,$(1))"
	$(UQ)curl -fsSL --retry 3 -o $$@.tmp $(call pkg_url,$(1)) || { rm -f $$@.tmp; \
		echo "packages: could not download $(call pkg_file,$(1))$(if $(filter $(1),$(PACKAGES_BUILD)),,$(comma) PACKAGES=none builds without packages)"; exit 1; }
	$(UQ)echo "$(call pkg_sha256,$(1))  $$@.tmp" | shasum -a 256 -c - > /dev/null || \
		{ rm -f $$@.tmp; echo "packages: downloaded $(call pkg_file,$(1)) does not match packages.lock"; exit 1; }
	$(UQ)mv $$@.tmp $$@
endef

# Unpack under root $(2) after removing what an earlier version installed. A pinned
# archive must match the lock, a local build is taken as is and said so.
define package_unpack_rule
$(call pkg_stamp,$(1),$(3)): $(call pkg_archive,$(1))
	$(UQ)$(if $(call pkg_is_local,$(1)),\
		echo "[PKG] $(1) $(call pkg_version,$(1)) ($(ARCH)) is an unpublished local build",\
		echo "$(call pkg_sha256,$(1))  $$<" | shasum -a 256 -c - > /dev/null || \
		{ echo "packages: cached $(call pkg_file,$(1)) does not match packages.lock, delete it to refetch"; exit 1; })
	$(UQ)mkdir -p $(3) $(2)
	$(UQ)$$(call pkg_remove,$(1),$(2),$(3))
	$(UQ)zstd -dc $$< | tar -xf - -C $(2)
	$(UQ)zstd -dc $$< | tar -tf - > $(call pkg_list,$(1),$(3))
	$(UQ)touch $$@
	@echo "[PKG] $(1) $(call pkg_version,$(1)) ($(ARCH))"
endef

# Removes the packages unpacked under the root $(1) with state in $(2) that the list $(3)
# no longer names, so a package left by an earlier build is not kept
define pkg_remove_unlisted
for list in $(2)/*.files; do \
	[ -f "$$list" ] || continue; \
	name=$$(basename $$list .files); \
	case " $(3) " in *" $$name "*) continue ;; esac; \
	$(call pkg_remove,$$name,$(1),$(2)); \
	echo "[PKG] removed $$name ($(ARCH))"; \
done
endef

$(foreach p,$(PACKAGES_STAGED) $(PACKAGES_BUILD),$(eval $(call check_package,$(p))))
$(foreach p,$(PACKAGES_STAGED) $(PACKAGES_BUILD),$(eval $(call package_fetch_rule,$(p))))
$(foreach p,$(PACKAGES_STAGED),$(eval $(call package_unpack_rule,$(p),$(PACKAGES_ROOTFS),$(PACKAGES_STATE))))
$(foreach p,$(PACKAGES_BUILD),$(eval $(call package_unpack_rule,$(p),$(PACKAGES_SDK),$(PACKAGES_SDK_STATE))))

# The selection alone decides what is staged, so packages left by an earlier
# build that this one does not select are removed from the overlay again
packages: $(foreach p,$(PACKAGES_STAGED),$(call pkg_stamp,$(p),$(PACKAGES_STATE)))
	$(UQ)$(call pkg_remove_unlisted,$(PACKAGES_ROOTFS),$(PACKAGES_STATE),$(PACKAGES_STAGED))

# Every build package whatever PACKAGES selects, since the tree's programs compile against them
sdk: $(foreach p,$(PACKAGES_BUILD),$(call pkg_stamp,$(p),$(PACKAGES_SDK_STATE)))
	$(UQ)$(call pkg_remove_unlisted,$(PACKAGES_SDK),$(PACKAGES_SDK_STATE),$(PACKAGES_BUILD))

.PHONY: packages sdk
