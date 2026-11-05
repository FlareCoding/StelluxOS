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

# A kit must record the GPU_ABI_VERSION of kernel/gpu/gpu_abi.h its library compiled against, equal to the tree's
VULKAN_TREE_GPU_ABI_VERSION := $(shell awk \
    '$$1 == "\043define" && $$2 == "GPU_ABI_VERSION" { v = $$3 } END { print v + 0 }' \
    $(USERLAND_ROOT)/../kernel/gpu/gpu_abi.h)
ifneq ($(VULKAN_GPU_ABI_VERSION),$(VULKAN_TREE_GPU_ABI_VERSION))
  $(error The Vulkan kit was not built against GPU_ABI_VERSION $(VULKAN_TREE_GPU_ABI_VERSION) of \
    kernel/gpu/gpu_abi.h. With the header committed, bump MESA_PKG_REL in packages/mesa/versions.sh if \
    packages.lock pins the release versions.sh names, then run make packages-build PACKAGES="none mesa", and for \
    CI publish that release and pin it)
endif

endif
