# Compiler settings
CXX       := g++

# Where the kernel's build artifacts live (relative to userland/ directory)
KERNEL_ELF		:= ../build/stellux
KERNEL_INCLUDES := -I../kernel/include -I../kernel/include/core

# Architecture
TARGET_ARCH := X86_64

# Pre-processor Defines
PREPROCESSOR_DEFINES := -DARCH_$(TARGET_ARCH)

CXXFLAGS := -m64 -g -O0 $(PREPROCESSOR_DEFINES) \
			 -ffreestanding -Wall -Werror -nostdlib -fno-rtti -std=c++17 -mno-red-zone \
			 $(KERNEL_INCLUDES) $(PREPROCESSOR_DEFINES) -fno-pie -fno-pic -mcmodel=large 

LDFLAGS  := -T userland.ld -nostdlib -z max-page-size=0x1000 -no-pie  \
			--just-symbols=$(KERNEL_ELF)

# The final output binary
TARGET := hello_world

# Sources
SRCS   := hello_world.cpp
OBJS   := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "[LD] Linking $@"
	$(LD) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	@echo "[CXX] Compiling $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
