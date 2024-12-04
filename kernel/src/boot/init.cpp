#include <serial/serial.h>
#include <boot/multiboot2.h>
#include <boot/efimem.h>
#include <arch/arch_init.h>
#include <interrupts/irq.h>

__PRIVILEGED_DATA
char* g_mbi_kernel_cmdline;

__PRIVILEGED_DATA
multiboot_tag_framebuffer* g_mbi_framebuffer;

__PRIVILEGED_DATA
multiboot_tag_efi_mmap* g_mbi_efi_mmap;

__PRIVILEGED_CODE
void walk_mbi(void* mbi) {
    // Cast the mbi pointer to a byte pointer for arithmetic
    uint8_t* ptr = static_cast<uint8_t*>(mbi);
    uint32_t total_size = *reinterpret_cast<uint32_t*>(ptr);

    // Move pointer past the initial 8 bytes
    ptr += 8;

    // Calculate the end of the MBI structure
    uint8_t* mbi_end = static_cast<uint8_t*>(mbi) + total_size;

    while (ptr < mbi_end) {
        // Interpret the current position as a multiboot_tag
        multiboot_tag* tag = reinterpret_cast<multiboot_tag*>(ptr);

        // Process the tag based on its type
        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                multiboot_tag_string* cmdline_tag = reinterpret_cast<multiboot_tag_string*>(tag);
                g_mbi_kernel_cmdline = cmdline_tag->string;
                break;
            }
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                g_mbi_framebuffer = reinterpret_cast<multiboot_tag_framebuffer*>(tag);
                break;
            }
            case MULTIBOOT_TAG_TYPE_EFI_MMAP: { // New case for EFI Memory Map
                g_mbi_efi_mmap = reinterpret_cast<multiboot_tag_efi_mmap*>(tag);
                break;
            }
            default: {
                break;
            }
        }

        // Move to the next tag, ensuring 8-byte alignment
        ptr += (tag->size + 7) & ~7;
    }
}

EXTERN_C
__PRIVILEGED_CODE
void init(unsigned int magic, void* mbi) {
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        while (true) { asm volatile ("hlt"); }
    }

    serial::init_port(SERIAL_PORT_BASE_COM1);

    // Architecture-specific initialization sequences
    arch::arch_init();

    // Process and store multiboot provided information
    walk_mbi(mbi);
}
