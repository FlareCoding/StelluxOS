# =========================
# StelluxOS Standard C Library (stlibc)
# =========================
# Uses exported variables from parent Makefiles

# Source and build directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# Source files
C_SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(C_SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Library output
LIBRARY := $(USERLAND_LIB_DIR)/libstlibc.a

# Use exported flags from parent with any library-specific additions
CFLAGS := $(CFLAGS_COMMON)

# Default target
all: $(LIBRARY)

# Create the static library
$(LIBRARY): $(OBJECTS)
	@mkdir -p $(dir $@)
	@echo "[AR] Creating $@"
	@$(AR) rcs $@ $^
	@$(RANLIB) $@

# Compile C source files to local obj directory
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning stlibc..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(LIBRARY)

# Install headers (if needed)
install-headers:
	@echo "Headers are already in the include directory structure"

.PHONY: all clean install-headers
