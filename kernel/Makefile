KERNEL_INCLUDES = -I./src -I./src/core

CFLAGS = -m64 -g -ffreestanding -O0 -Wall -Werror -Wextra $(KERNEL_INCLUDES)
CXXFLAGS = $(CFLAGS) -std=c++17
ASMC	 = nasm
ASMFLAGS = -g
LDFLAGS = -nostdlib -T kstellux.ld -o kernel.elf

SRCDIR	:=src
OBJDIR	:=obj

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

CPPSOURCES 	:= $(call rwildcard,$(SRCDIR),*.cpp)
CSOURCES    := $(call rwildcard,$(SRCDIR),*.c)
ASMSOURCES 	:= $(call rwildcard,$(SRCDIR),*.asm)  
OBJECTS 	:= $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(CPPSOURCES))
OBJECTS 	+= $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(CSOURCES))
OBJECTS 	+= $(patsubst $(SRCDIR)/%.asm, $(OBJDIR)/%_asm.o, $(ASMSOURCES))

all: kernel.elf

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%_asm.o: $(SRCDIR)/%.asm
	@mkdir -p $(@D)
	$(ASMC) $(ASMFLAGS) $^ -f elf64 -o $@

kernel.elf: $(OBJECTS)
	@mkdir -p bin
	$(LD) $(LDFLAGS) $^ -o bin/$@

clean:
	rm -rf ./bin
	rm -rf ./obj
	rm -f *.o

.PHONY: all clean
