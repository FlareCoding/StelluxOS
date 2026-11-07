#
# Stellux Userland Vulkan Build Rules
#
# Included by cxxapp.mk for an app built on Vulkan. The kit is the mesa build
# package the userland build unpacks into the SDK: the Vulkan library, Vulkan's
# headers and the flags its own vulkan.mk names.
#

VULKAN_DIR := $(USERLAND_ROOT)/toolchain/sdk/$(ARCH)/mesa

# Cleaning needs no kit
ifeq ($(filter clean,$(MAKECMDGOALS)),)

ifeq ($(wildcard $(VULKAN_DIR)/share/stellux/vulkan.mk),)
  $(error Vulkan kit not found in $(VULKAN_DIR). The userland build unpacks it, build from the top-level directory)
endif

include $(VULKAN_DIR)/share/stellux/vulkan.mk

VULKAN_KIT_REBUILD := With the header committed, bump MESA_PKG_REL in packages/mesa/versions.sh if \
    packages.lock pins the release versions.sh names, then run make packages-build PACKAGES="none mesa", and for \
    CI publish that release and pin it

# The decimal value of macro $(1) in header $(2), or 0 when the header does not define it
vulkan_header_version = $(shell awk '$$1 == "\043define" && $$2 == "$(1)" { v = $$3 } END { print v + 0 }' $(2))

# A kit must record the GPU_ABI_VERSION of kernel/gpu/gpu_abi.h its library compiled against, equal to the tree's
VULKAN_TREE_GPU_ABI_VERSION := $(call vulkan_header_version,GPU_ABI_VERSION, \
    $(USERLAND_ROOT)/../kernel/gpu/gpu_abi.h)
ifneq ($(VULKAN_GPU_ABI_VERSION),$(VULKAN_TREE_GPU_ABI_VERSION))
  $(error The Vulkan kit was not built against GPU_ABI_VERSION $(VULKAN_TREE_GPU_ABI_VERSION) of \
    kernel/gpu/gpu_abi.h. $(VULKAN_KIT_REBUILD))
endif

# A kit must also record the STLXWIN_GPU_ABI_VERSION of the stlxwin.h its library compiled against, equal to the tree's
VULKAN_TREE_STLXWIN_GPU_ABI_VERSION := $(call vulkan_header_version,STLXWIN_GPU_ABI_VERSION, \
    $(USERLAND_ROOT)/lib/libstlxwin/include/stlxwin/stlxwin.h)
ifneq ($(VULKAN_STLXWIN_GPU_ABI_VERSION),$(VULKAN_TREE_STLXWIN_GPU_ABI_VERSION))
  $(error The Vulkan kit was not built against STLXWIN_GPU_ABI_VERSION $(VULKAN_TREE_STLXWIN_GPU_ABI_VERSION) of \
    userland/lib/libstlxwin/include/stlxwin/stlxwin.h. $(VULKAN_KIT_REBUILD))
endif

endif
