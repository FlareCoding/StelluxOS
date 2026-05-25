#
# Python Interpreter - Stellux Build
#
# Builds a static CPython interpreter against the Stellux musl sysroot
# and libstlx. Stages python + stdlib into userland/build/$(ARCH)/rootfs.
#

APP_NAME := python
ARCH ?= x86_64
USERLAND_ROOT ?= ../..
override USERLAND_ROOT := $(abspath $(USERLAND_ROOT))

PY_VER := 3.12.8
PY_MAJOR_MINOR := 3.12
PY_TARBALL := Python-$(PY_VER).tar.xz
PY_URL := https://www.python.org/ftp/python/$(PY_VER)/$(PY_TARBALL)

SYSROOT := $(USERLAND_ROOT)/sysroot/$(ARCH)
TARGET_TRIPLE := $(ARCH)-linux-musl

DL_DIR := dl
SRC_ROOT := src
SRC_DIR := $(SRC_ROOT)/Python-$(PY_VER)
BUILD_DIR := build/$(ARCH)
INSTALL_DIR := install/$(ARCH)

ROOTFS_DIR := $(USERLAND_ROOT)/build/$(ARCH)/rootfs

CC := clang
HOST_PYTHON := python3

# Runtime builtins discovery: prefer the sysroot-bundled compiler-rt (built by
# 'make compiler-rt' at repo root), then the host's system compiler-rt, then
# the target GCC libgcc archive. Mirrors userland/mk/toolchain.mk so that
# every userland app sees the same builtins.
SYSROOT_BUILTINS     := $(SYSROOT)/lib/libclang_rt.builtins-$(ARCH).a
COMPILER_RT_BUILTINS := $(shell $(CC) --target=$(TARGET_TRIPLE) --rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null)
GCC_BUILTINS         := $(shell $(ARCH)-linux-gnu-gcc -print-libgcc-file-name 2>/dev/null)

ifneq ($(wildcard $(SYSROOT_BUILTINS)),)
  BUILTINS_LIB := $(SYSROOT_BUILTINS)
else ifneq ($(wildcard $(COMPILER_RT_BUILTINS)),)
  BUILTINS_LIB := $(COMPILER_RT_BUILTINS)
else ifneq ($(wildcard $(GCC_BUILTINS)),)
  BUILTINS_LIB := $(GCC_BUILTINS)
else
  $(error Missing runtime builtins for ARCH=$(ARCH). Run 'make compiler-rt' or install dependencies with 'make deps')
endif

COMMON_CFLAGS := \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	-nostdlibinc \
	-isystem $(SYSROOT)/include \
	-std=c11 -O2 -g \
	-D_GNU_SOURCE \
	-D__STELLUX__ \
	-fno-stack-protector

COMMON_LDFLAGS := \
	-nostdlib \
	-fuse-ld=lld \
	--target=$(TARGET_TRIPLE) \
	--sysroot=$(SYSROOT) \
	--rtlib=compiler-rt \
	-static

STELLUX_LIBS := \
	$(SYSROOT)/lib/crt1.o \
	$(SYSROOT)/lib/crti.o \
	-L$(SYSROOT)/lib \
	-Wl,--start-group \
	-lstlx \
	-lc \
	-lm \
	$(BUILTINS_LIB) \
	-Wl,--end-group \
	$(SYSROOT)/lib/crtn.o

# Timestamp-files so make can use mtime-based incremental rebuilds.
EXTRACT_STAMP   := $(SRC_DIR)/.stellux-extracted
PATCH_STAMP     := $(SRC_DIR)/.stellux-patched
CONFIGURE_STAMP := $(BUILD_DIR)/.configured
BUILD_STAMP     := $(BUILD_DIR)/.built

# The staged interpreter in the rootfs is the install "stamp"
ROOTFS_PYTHON := $(ROOTFS_DIR)/usr/bin/python$(PY_MAJOR_MINOR)

.PHONY: all fetch extract patch configure build install verify clean distclean print-vars

all: install

print-vars:
	@echo "APP_NAME=$(APP_NAME)"
	@echo "ARCH=$(ARCH)"
	@echo "USERLAND_ROOT=$(USERLAND_ROOT)"
	@echo "SYSROOT=$(SYSROOT)"
	@echo "TARGET_TRIPLE=$(TARGET_TRIPLE)"
	@echo "SRC_DIR=$(SRC_DIR)"
	@echo "BUILD_DIR=$(BUILD_DIR)"
	@echo "INSTALL_DIR=$(INSTALL_DIR)"
	@echo "ROOTFS_DIR=$(ROOTFS_DIR)"
	@echo "BUILTINS_LIB=$(BUILTINS_LIB)"

fetch:     $(DL_DIR)/$(PY_TARBALL)
extract:   $(EXTRACT_STAMP)
patch:     $(PATCH_STAMP)
configure: $(CONFIGURE_STAMP)
build:     $(BUILD_STAMP)
install:   $(ROOTFS_PYTHON)

$(DL_DIR)/$(PY_TARBALL):
	@mkdir -p $(DL_DIR)
	@echo "[FETCH] CPython $(PY_VER)"
	@curl -L "$(PY_URL)" -o "$@"

$(EXTRACT_STAMP): $(DL_DIR)/$(PY_TARBALL)
	@mkdir -p $(SRC_ROOT)
	@if [ ! -d "$(SRC_DIR)" ]; then \
		echo "[EXTRACT] $(PY_TARBALL)"; \
		tar -C $(SRC_ROOT) -xf "$(DL_DIR)/$(PY_TARBALL)"; \
	fi
	@touch $@

$(PATCH_STAMP): | $(EXTRACT_STAMP)
	@echo "[PATCH] CPython for Stellux"
	@cd $(SRC_DIR) && for p in ../../patches/*.patch; do \
		if [ -f "$$p" ]; then \
			echo "  applying $$p"; \
			patch --forward --batch -p1 < "$$p"; \
		fi; \
	done
	@cp Modules.Setup.local $(SRC_DIR)/Modules/Setup.local
	@touch $@

$(CONFIGURE_STAMP): $(PATCH_STAMP) config.site Modules.Setup.local
	@mkdir -p $(BUILD_DIR)
	@echo "[CONF] CPython $(PY_VER) ($(ARCH))"
	@echo "  SYSROOT=$(SYSROOT)"
	@echo "  BUILTINS_LIB=$(BUILTINS_LIB)"
	@cd $(BUILD_DIR) && \
	CONFIG_SITE=$(CURDIR)/config.site \
	CC="$(CC)" \
	CFLAGS="$(COMMON_CFLAGS)" \
	LDFLAGS="$(COMMON_LDFLAGS)" \
	LIBS="$(STELLUX_LIBS)" \
	PKG_CONFIG=false \
	ac_cv_file__dev_ptmx=no \
	ac_cv_file__dev_ptc=no \
	../../$(SRC_DIR)/configure \
		--build=$$(../../$(SRC_DIR)/config.guess) \
		--host=$(TARGET_TRIPLE) \
		--prefix=/usr \
		--disable-shared \
		--disable-test-modules \
		--disable-ipv6 \
		--without-ensurepip \
		--with-build-python=$$(command -v $(HOST_PYTHON))
	@cp Modules.Setup.local $(BUILD_DIR)/Modules/Setup.local
	@touch $@

$(BUILD_STAMP): $(CONFIGURE_STAMP)
	@echo "[MAKE] CPython $(PY_VER) ($(ARCH))"
	@$(MAKE) -C $(BUILD_DIR) LINKFORSHARED=""
	@touch $@

$(ROOTFS_PYTHON): $(BUILD_STAMP)
	@echo "[INSTALL] CPython $(PY_VER)"
	@rm -rf $(INSTALL_DIR)

	@if ! $(MAKE) -C $(BUILD_DIR) install DESTDIR=$(CURDIR)/$(INSTALL_DIR); then \
		echo "[WARN] full install failed; retrying static-friendly install targets"; \
		rm -rf $(INSTALL_DIR); \
		$(MAKE) -C $(BUILD_DIR) altbininstall DESTDIR=$(CURDIR)/$(INSTALL_DIR); \
		$(MAKE) -C $(BUILD_DIR) libinstall DESTDIR=$(CURDIR)/$(INSTALL_DIR); \
		$(MAKE) -C $(BUILD_DIR) inclinstall DESTDIR=$(CURDIR)/$(INSTALL_DIR); \
	fi

	# Stellux has no dynamic loader.
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/lib-dynload
	@find $(INSTALL_DIR) -name '*.so' -delete

	# Remove build/development artifacts not needed at runtime.
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/config-*
	@rm -rf $(INSTALL_DIR)/usr/include
	@rm -rf $(INSTALL_DIR)/usr/lib/pkgconfig
	@rm -rf $(INSTALL_DIR)/usr/share/man
	@rm -f  $(INSTALL_DIR)/usr/lib/libpython*.a
	@rm -f  $(INSTALL_DIR)/usr/lib/libpython*.so*
	@rm -f  $(INSTALL_DIR)/usr/lib/python*.a

	# Trim bulky/unneeded stdlib content for early bring-up.
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/test
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/idlelib
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/tkinter
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/turtledemo
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/ensurepip
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/venv
	@rm -rf $(INSTALL_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/lib2to3

	# Remove helper scripts not needed in Stellux initrd.
	@rm -f $(INSTALL_DIR)/usr/bin/idle*
	@rm -f $(INSTALL_DIR)/usr/bin/pydoc*
	@rm -f $(INSTALL_DIR)/usr/bin/2to3*
	@rm -f $(INSTALL_DIR)/usr/bin/python*-config

	# Remove bytecode/cache files.
	@find $(INSTALL_DIR) -name '__pycache__' -type d -prune -exec rm -rf {} +
	@find $(INSTALL_DIR) -name '*.pyc' -delete
	@find $(INSTALL_DIR) -name '*.pyo' -delete
	@find $(INSTALL_DIR) -name '*.a' -delete
	@find $(INSTALL_DIR) -name '*.o' -delete

	# Optional: strip the runtime interpreter to reduce initrd size.
	@llvm-strip $(INSTALL_DIR)/usr/bin/python$(PY_MAJOR_MINOR) 2>/dev/null || \
		strip $(INSTALL_DIR)/usr/bin/python$(PY_MAJOR_MINOR) 2>/dev/null || true

	# Stage as a rootfs overlay. Remove stale Python artifacts first.
	@rm -rf $(ROOTFS_DIR)/usr/bin/python*
	@rm -rf $(ROOTFS_DIR)/usr/lib/python$(PY_MAJOR_MINOR)
	@rm -f  $(ROOTFS_DIR)/usr/lib/libpython*
	@rm -rf $(ROOTFS_DIR)/usr/include/python*
	@rm -rf $(ROOTFS_DIR)/usr/lib/pkgconfig/python*
	@rm -f  $(ROOTFS_DIR)/bin/python3

	@mkdir -p $(ROOTFS_DIR)
	@cp -a $(INSTALL_DIR)/usr $(ROOTFS_DIR)/

	# Convenience command path.
	@mkdir -p $(ROOTFS_DIR)/bin
	@ln -sf /usr/bin/python$(PY_MAJOR_MINOR) $(ROOTFS_DIR)/bin/python3

	# `cp -a` preserves source mtimes, which can be older than $(BUILD_STAMP).
	# Bump $(ROOTFS_PYTHON) so make sees this stamp as up-to-date next run.
	@touch $@

	@echo "[OK] Staged Python $(PY_VER) rootfs overlay for Stellux"
	@$(MAKE) --no-print-directory verify

verify:
	@echo "[VERIFY] built interpreter"
	@if [ -x "$(BUILD_DIR)/python" ]; then \
		file "$(BUILD_DIR)/python"; \
		readelf -l "$(BUILD_DIR)/python" | grep INTERP || echo "no PT_INTERP"; \
	else \
		echo "missing $(BUILD_DIR)/python"; \
		exit 1; \
	fi

	@echo "[VERIFY] staged Python files"
	@test -x "$(ROOTFS_DIR)/usr/bin/python$(PY_MAJOR_MINOR)"
	@test -d "$(ROOTFS_DIR)/usr/lib/python$(PY_MAJOR_MINOR)/encodings"
	@test -L "$(ROOTFS_DIR)/bin/python3"

	@echo "[VERIFY] no dynamic/shared/dev artifacts"
	@found="$$(find $(ROOTFS_DIR) \( -name '*.so' -o -name 'libpython*.a' -o -name 'libpython*.so*' \) -print 2>/dev/null)"; \
	if [ -n "$$found" ]; then \
		echo "$$found"; \
		echo "[FAIL] bad Python artifacts in rootfs staging"; \
		exit 1; \
	else \
		echo "ok"; \
	fi

clean:
	@rm -rf build install
	@rm -rf $(ROOTFS_DIR)/usr/bin/python*
	@rm -rf $(ROOTFS_DIR)/usr/lib/python$(PY_MAJOR_MINOR)
	@rm -f  $(ROOTFS_DIR)/usr/lib/libpython*
	@rm -f  $(ROOTFS_DIR)/bin/python3

distclean: clean
	@rm -rf src dl

