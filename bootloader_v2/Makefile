EFIINC          := -I /usr/include/efi -I /usr/include/efi/x86_64
EFI_LDS         := /usr/lib/elf_$(ARCH)_efi.lds
EFILIBS			:= -lgnuefi -lefi

CFLAGS    		:= $(EFIINC) -g -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -DHAVE_USE_MS_ABI

ifeq ($(ARCH), x86_64)
    CFLAGS += -mno-red-zone -maccumulate-outgoing-args
endif

LDFLAGS   		:= -shared -nostdlib -znocombreloc -T $(EFI_LDS) -Bsymbolic -L /usr/lib /usr/lib/crt0-efi-x86_64.o $(EFILIBS)

all: bootloader.efi

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

SOURCES := $(wildcard *.c)
OBJECTS := $(patsubst %.c, %.o, $(SOURCES))

bootloader.efi: bootloader.so
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym  -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc --target efi-app-$(ARCH) --subsystem=10 $^ $@

bootloader.so: $(OBJECTS)
	$(LD) $(LDFLAGS) $^ -o $@ $(EFILIBS)

clean:
	rm -f *.o *.efi *.img *.so

.PHONY: clean
