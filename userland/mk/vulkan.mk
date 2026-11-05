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

endif
