# =========================
# Userland Libraries Makefile
# =========================
# Uses exported variables from parent Makefile

# Sub-libraries
STLIBC_DIR := stlibc

# Default target - build all libraries
all: stlibc

# Build stlibc
stlibc:
	@echo "Building stlibc..."
	@$(MAKE) -C $(STLIBC_DIR)

# Clean all libraries
clean:
	@echo "Cleaning libraries..."
	@$(MAKE) -C $(STLIBC_DIR) clean

.PHONY: all stlibc clean
