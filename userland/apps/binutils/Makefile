#
# Stellux binutils stager
#
# Stages the prebuilt static binutils tools into the rootfs overlay so
# the userland install step carries them onto the initrd at /usr/bin.
# The tools themselves come from 'make binutils' at the top level,
# which downloads a pinned release, applies patches/, and cross-builds
# both architectures into userland/toolchain/.
#

USERLAND_ROOT ?= $(shell cd ../.. && pwd)
include ../../mk/toolchain.mk

TOOLCHAIN_BIN := $(USERLAND_ROOT)/toolchain/$(ARCH)/bin
ROOTFS_DIR    := $(USERLAND_ROOT)/build/$(ARCH)/rootfs
STAGE_BIN     := $(ROOTFS_DIR)/usr/bin

# ld.bfd duplicates ld and gprof needs profiling support, so neither ships
TOOLS := as ld ar ranlib nm objcopy strip readelf objdump strings size \
	addr2line c++filt

all:
	$(UQ)if [ ! -x $(TOOLCHAIN_BIN)/as ]; then \
		echo "binutils: tools for $(ARCH) missing, run 'make binutils' at the top level"; \
		exit 1; \
	fi
	$(UQ)mkdir -p $(STAGE_BIN)
	$(UQ)for t in $(TOOLS); do cp $(TOOLCHAIN_BIN)/$$t $(STAGE_BIN)/$$t; done
	@echo "[STG] binutils ($(ARCH))"

clean:
	$(UQ)rm -f $(addprefix $(STAGE_BIN)/,$(TOOLS))

.PHONY: all clean
