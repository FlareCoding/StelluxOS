# =========================
# Userland Libraries Makefile
# =========================
# Uses exported variables from parent Makefile

# Sub-libraries
STLIBC_DIR := stlibc
LIBSTLXGFX_DIR := libstlxgfx

# Default target - build all libraries
all: stlibc libstlxgfx

# Build stlibc
stlibc:
	@echo "Building stlibc..."
	@$(MAKE) -C $(STLIBC_DIR)

# Build libstlxgfx
libstlxgfx:
	@echo "Building libstlxgfx..."
	@$(MAKE) -C $(LIBSTLXGFX_DIR)

# Clean all libraries
clean:
	@echo "Cleaning libraries..."
	@$(MAKE) -C $(STLIBC_DIR) clean
	@$(MAKE) -C $(LIBSTLXGFX_DIR) clean

.PHONY: all stlibc libstlxgfx clean
