# Fetches the Stellux developer packages named in PACKAGES and unpacks them
# into the rootfs overlay. Archives are cached in userland/toolchain/packages
# and verified against packages.lock, see packages/README.md.

include $(USERLAND_ROOT)/../packages/packages.mk

PACKAGES_STATE := $(USERLAND_ROOT)/build/$(ARCH)/packages
PACKAGES_ROOTFS := $(USERLAND_ROOT)/build/$(ARCH)/rootfs

pkg_archive = $(PACKAGES_CACHE)/$(call pkg_file,$(1))
pkg_stamp   = $(PACKAGES_STATE)/$(1).stamp
pkg_list    = $(PACKAGES_STATE)/$(1).files

# Download once into the cache, keeping only archives that match their pin
define package_fetch_rule
$(call pkg_archive,$(1)):
	$(UQ)[ -n "$(PACKAGES_RELEASE)" ] || \
		{ echo "packages: $(1) is not cached and packages.lock pins no release"; exit 1; }
	$(UQ)mkdir -p $(PACKAGES_CACHE)
	$(UQ)echo "[PKG] fetching $(call pkg_file,$(1))"
	$(UQ)curl -fsSL --retry 3 -o $$@.tmp $(call pkg_url,$(1)) || { rm -f $$@.tmp; exit 1; }
	$(UQ)echo "$(call pkg_sha256,$(1))  $$@.tmp" | shasum -a 256 -c - > /dev/null || \
		{ rm -f $$@.tmp; echo "packages: downloaded $(call pkg_file,$(1)) does not match packages.lock"; exit 1; }
	$(UQ)mv $$@.tmp $$@
endef

# Unpack into the overlay, first removing what the previous version of the
# same package installed so a version bump leaves no stale files behind
define package_stage_rule
$(call pkg_stamp,$(1)): $(call pkg_archive,$(1))
	$(UQ)echo "$(call pkg_sha256,$(1))  $$<" | shasum -a 256 -c - > /dev/null || \
		{ echo "packages: cached $(call pkg_file,$(1)) does not match packages.lock, delete it to refetch"; exit 1; }
	$(UQ)mkdir -p $(PACKAGES_STATE) $(PACKAGES_ROOTFS)
	$(UQ)if [ -f $(call pkg_list,$(1)) ]; then \
		(cd $(PACKAGES_ROOTFS) && xargs rm -f < $(call pkg_list,$(1))); \
	fi
	$(UQ)zstd -dc $$< | tar -xf - -C $(PACKAGES_ROOTFS)
	$(UQ)zstd -dc $$< | tar -tf - | grep -v '/$$$$' > $(call pkg_list,$(1))
	$(UQ)touch $$@
	@echo "[PKG] $(1) $(call pkg_version,$(1)) ($(ARCH))"
endef

$(foreach p,$(PACKAGES),$(eval $(call check_package,$(p))))
$(foreach p,$(PACKAGES),$(eval $(call package_fetch_rule,$(p))))
$(foreach p,$(PACKAGES),$(eval $(call package_stage_rule,$(p))))

packages: $(foreach p,$(PACKAGES),$(call pkg_stamp,$(p)))

.PHONY: packages
