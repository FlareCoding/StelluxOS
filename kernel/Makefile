# Define tools
AS := gcc
CC := g++
LD := ld

# Directories
SRC_DIR := src
INC_DIR := include
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# Architecture
TARGET_ARCH := X86_64

# Pre-processor Defines
PREPROCESSOR_DEFINES := -DARCH_$(TARGET_ARCH)

# Output files
KERNEL := $(BUILD_DIR)/stellux
LINKER_SCRIPT := stellux.ld

# Function to recursively find files in subdirectories
rwildcard = $(foreach d,$(wildcard $(1)/*), \
             $(call rwildcard,$d,$2)) \
             $(filter $(subst *,%,$2),$d)

# Source and header files
ASM_SRCS := $(call rwildcard, $(SRC_DIR), *.S)
CPP_SRCS := $(call rwildcard, $(SRC_DIR), *.cpp)
HEADERS  := $(call rwildcard, $(SRC_DIR), *.h)

# Object files
ASM_OBJS := $(ASM_SRCS:$(SRC_DIR)/%.S=$(OBJ_DIR)/%.o)
CPP_OBJS := $(CPP_SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
OBJS     := $(ASM_OBJS) $(CPP_OBJS)

# Compiler and linker flags
ASFLAGS   := -m64 -g -ffreestanding -nostdlib -I$(INC_DIR) -I$(INC_DIR)/core $(PREPROCESSOR_DEFINES)
CXXFLAGS  := -m64 -g -I$(INC_DIR) -I$(INC_DIR)/core $(PREPROCESSOR_DEFINES) \
			 -ffreestanding -nostdlib -fno-exceptions -fno-rtti -std=c++17 -mno-red-zone
LDFLAGS   := -m elf_x86_64 -T $(LINKER_SCRIPT) -g

# Default target
all: $(KERNEL)

# Create build directories
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile assembly files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.S $(HEADERS)
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# Compile C++ files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CXXFLAGS) -c $< -o $@

# Link the kernel
$(KERNEL): $(OBJS) $(LINKER_SCRIPT) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Clean up build files
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
