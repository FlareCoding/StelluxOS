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

# Conditionally exclude .test.cpp files if BUILD_UNIT_TESTS is not set
ifndef BUILD_UNIT_TESTS
CPP_SRCS := $(filter-out %.test.cpp, $(CPP_SRCS))
endif

# Object files
ASM_OBJS := $(ASM_SRCS:$(SRC_DIR)/%.S=$(OBJ_DIR)/%.o)
CPP_OBJS := $(CPP_SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
OBJS     := $(ASM_OBJS) $(CPP_OBJS)

# Optimization level [0, 1, 2, or 3]
COMPILER_OPTIMIZATION_LEVEL := 2

# Compiler and linker flags
ASFLAGS   := -m64 -g -ffreestanding -nostdlib -I$(INC_DIR) -I$(INC_DIR)/core $(PREPROCESSOR_DEFINES)
CXXFLAGS  := -m64 -g -O$(COMPILER_OPTIMIZATION_LEVEL) -I$(INC_DIR) -I$(INC_DIR)/core $(PREPROCESSOR_DEFINES) \
			 -ffreestanding -Wall -Werror -Wextra -nostdlib -fno-exceptions -fno-rtti -std=c++17 -mno-red-zone \
			 -fno-allow-store-data-races -fno-reorder-blocks -fno-ipa-cp-clone -fno-partial-inlining -Wframe-larger-than=2048 \
			 -Wimplicit-fallthrough=5 -Wno-unused-but-set-variable -Wno-unused-const-variable -fno-omit-frame-pointer -fno-optimize-sibling-calls \
			 -fno-stack-clash-protection -Wno-stringop-truncation -Wno-zero-length-bounds -Wno-array-bounds -Wno-stringop-overflow -Wno-restrict \
			 -Wno-maybe-uninitialized -fno-strict-overflow -fno-stack-check -fconserve-stack -fno-stack-protector -mno-sse -mno-mmx
LDFLAGS   := -m elf_x86_64 -T $(LINKER_SCRIPT) -g

ifdef BUILD_UNIT_TESTS
CXXFLAGS += -DBUILD_UNIT_TESTS
ASFLAGS  += -DBUILD_UNIT_TESTS
endif

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
	@echo "[AS ] Compiling $<"
	@$(AS) $(ASFLAGS) -c $< -o $@

# Compile C++ files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[CXX] Compiling $<"
	@$(CC) $(CXXFLAGS) -c $< -o $@

# Link the kernel
$(KERNEL): $(OBJS) $(LINKER_SCRIPT) | $(BUILD_DIR)
	@echo "[LD ] Linking $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Clean up build files
clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
