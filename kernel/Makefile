KERNEL_INCLUDES = -I./src -I./src/core

CFLAGS = -m64 -g -ffreestanding -O0 -Wall -Werror -Wextra -mno-red-zone -fno-allow-store-data-races -fno-reorder-blocks -fno-ipa-cp-clone -fno-partial-inlining -Wframe-larger-than=2048 -Wimplicit-fallthrough=5 -Wno-unused-but-set-variable -Wno-unused-const-variable -fno-omit-frame-pointer -fno-optimize-sibling-calls -ftrivial-auto-var-init=zero -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang -fno-stack-clash-protection -Wno-stringop-truncation -Wno-zero-length-bounds -Wno-array-bounds -Wno-stringop-overflow -Wno-restrict -Wno-maybe-uninitialized -fno-strict-overflow -fno-stack-check -fconserve-stack $(KERNEL_INCLUDES)
CXXFLAGS = $(CFLAGS) -std=c++17
ASMC	 = as
ASMFLAGS = -g --64 -I./src/arch/x86/asm
LDFLAGS = -nostdlib -T kstellux.ld -o kernel.elf

SRCDIR	:=src
OBJDIR	:=obj

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

CPPSOURCES 	:= $(call rwildcard,$(SRCDIR),*.cpp)
CSOURCES    := $(call rwildcard,$(SRCDIR),*.c)
ASMSOURCES 	:= $(call rwildcard,$(SRCDIR),*.s)  
OBJECTS 	:= $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(CPPSOURCES))
OBJECTS 	+= $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(CSOURCES))
OBJECTS 	+= $(patsubst $(SRCDIR)/%.s, $(OBJDIR)/%_asm.o, $(ASMSOURCES))

all: kernel.elf

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $^ -o $@

$(OBJDIR)/%_asm.o: $(SRCDIR)/%.s
	@mkdir -p $(@D)
	$(ASMC) $(ASMFLAGS) $^ -o $@

kernel.elf: $(OBJECTS)
	@mkdir -p bin
	$(LD) $(LDFLAGS) $^ -o bin/$@

clean:
	rm -rf ./bin
	rm -rf ./obj
	rm -f *.o

.PHONY: all clean
