# ================================
# Stellux Display Manager Makefile
# ================================
# Uses exported variables from parent Makefiles

# Application configuration
APP_NAME := stlxdm
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# Source files and objects
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Final binary (goes to shared bin directory)
TARGET := $(USERLAND_BIN_DIR)/$(APP_NAME)

# Use exported flags from parent with local include directory
CFLAGS := $(CFLAGS_COMMON) -I$(INC_DIR) -I$(LIBSTLXGFX_INCLUDE_DIR)
LDFLAGS := $(LDFLAGS_EXE)
LIBS := $(LIBS_STLXGFX) $(LIBS_COMMON)

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	@echo "[LD] Linking $@"
	@$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@
	@echo "Built $(APP_NAME) successfully"

# Compile C source files to local obj directory
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning $(APP_NAME)..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(TARGET)

.PHONY: all clean
