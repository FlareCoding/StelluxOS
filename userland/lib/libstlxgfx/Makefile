# ================================
# STLXGFX Graphics Library Makefile
# ================================
# Uses exported variables from parent Makefiles

# Library configuration
LIB_NAME := stlxgfx
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# Source files and objects
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Output library (static)
TARGET := $(USERLAND_LIB_DIR)/lib$(LIB_NAME).a

# Use exported flags from parent with local include directory
CFLAGS := $(CFLAGS_COMMON) -I$(INC_DIR)

# Default target
all: $(TARGET)

# Build the static library
$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	@echo "[AR] Creating static library $@"
	@$(AR) rcs $@ $(OBJECTS)
	@$(RANLIB) $@
	@echo "Built lib$(LIB_NAME).a successfully"

# Compile C source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning lib$(LIB_NAME)..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(TARGET)

# Install headers (copy to a common include location if needed)
install-headers:
	@mkdir -p $(USERLAND_INCLUDE_DIR)
	@cp -r $(INC_DIR)/* $(USERLAND_INCLUDE_DIR)/
	@echo "Installed $(LIB_NAME) headers"

.PHONY: all clean install-headers
