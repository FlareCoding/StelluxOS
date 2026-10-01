# Fetches the packages PACKAGES selects and unpacks them into the rootfs overlay.
# Archives are cached in userland/toolchain/packages, see packages/README.md.

include $(USERLAND_ROOT)/../packages/packages.mk

PACKAGES_STATE := $(USERLAND_ROOT)/build/$(ARCH)/packages
PACKAGES_ROOTFS := $(USERLAND_ROOT)/build/$(ARCH)/rootfs

pkg_archive = $(PACKAGES_CACHE)/$(call pkg_file,$(1))
pkg_stamp   = $(PACKAGES_STATE)/$(1)-$(call pkg_version,$(1)).stamp
pkg_list    = $(PACKAGES_STATE)/$(1).files

# Removes what a staged package installed, then the directories it left empty, and
# every stamp of the package, since a switch to another version must stage again
define pkg_unstage
if [ -f $(call pkg_list,$(1)) ]; then \
	(cd $(PACKAGES_ROOTFS) && \
		grep -v '/$$' $(call pkg_list,$(1)) | xargs rm -f && \
		grep '/$$' $(call pkg_list,$(1)) | sort -r | xargs rmdir 2>/dev/null; true); \
	rm -f $(call pkg_list,$(1)) $(PACKAGES_STATE)/$(1)-*.stamp; \
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
		echo "packages: could not download $(call pkg_file,$(1)), PACKAGES=none builds without packages"; exit 1; }
	$(UQ)echo "$(call pkg_sha256,$(1))  $$@.tmp" | shasum -a 256 -c - > /dev/null || \
		{ rm -f $$@.tmp; echo "packages: downloaded $(call pkg_file,$(1)) does not match packages.lock"; exit 1; }
	$(UQ)mv $$@.tmp $$@
endef

# Unpack into the overlay after removing what an earlier version installed. A pinned
# archive must match the lock, a local build is taken as is and said so.
define package_stage_rule
$(call pkg_stamp,$(1)): $(call pkg_archive,$(1))
	$(UQ)$(if $(call pkg_is_local,$(1)),\
		echo "[PKG] $(1) $(call pkg_version,$(1)) ($(ARCH)) is an unpublished local build",\
		echo "$(call pkg_sha256,$(1))  $$<" | shasum -a 256 -c - > /dev/null || \
		{ echo "packages: cached $(call pkg_file,$(1)) does not match packages.lock, delete it to refetch"; exit 1; })
	$(UQ)mkdir -p $(PACKAGES_STATE) $(PACKAGES_ROOTFS)
	$(UQ)$$(call pkg_unstage,$(1))
	$(UQ)zstd -dc $$< | tar -xf - -C $(PACKAGES_ROOTFS)
	$(UQ)zstd -dc $$< | tar -tf - > $(call pkg_list,$(1))
	$(UQ)touch $$@
	@echo "[PKG] $(1) $(call pkg_version,$(1)) ($(ARCH))"
endef

$(foreach p,$(PACKAGES_SELECTED),$(eval $(call check_package,$(p))))
$(foreach p,$(PACKAGES_SELECTED),$(eval $(call package_fetch_rule,$(p))))
$(foreach p,$(PACKAGES_SELECTED),$(eval $(call package_stage_rule,$(p))))

# The selection alone decides what is staged, so packages left by an earlier
# build that this one does not select are removed from the overlay again
packages: $(foreach p,$(PACKAGES_SELECTED),$(call pkg_stamp,$(p)))
	$(UQ)for list in $(PACKAGES_STATE)/*.files; do \
		[ -f "$$list" ] || continue; \
		name=$$(basename $$list .files); \
		case " $(PACKAGES_SELECTED) " in *" $$name "*) continue ;; esac; \
		$(call pkg_unstage,$$name); \
		echo "[PKG] removed $$name ($(ARCH))"; \
	done

.PHONY: packages
