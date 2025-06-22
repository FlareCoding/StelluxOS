# ==========================
# Shell Application Makefile
# ==========================
# Uses exported variables from parent Makefiles

# Application configuration
APP_NAME := shell
SRC_DIR := src
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# Source files and objects
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Final binary (goes to shared bin directory)
TARGET := $(USERLAND_BIN_DIR)/$(APP_NAME)

# Use exported flags from parent
CFLAGS := $(CFLAGS_COMMON)
LDFLAGS := $(LDFLAGS_EXE)
LIBS := $(LIBS_COMMON)

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