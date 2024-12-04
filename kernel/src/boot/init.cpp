#include <serial/serial.h>
#include <boot/multiboot2.h>
#include <arch/arch_init.h>
#include <interrupts/irq.h>

struct efi_memory_descriptor {
    multiboot_uint32_t type;
    multiboot_uint32_t reserved;
    multiboot_uint64_t physical_start;
    multiboot_uint64_t virtual_start;
    multiboot_uint64_t number_of_pages;
    multiboot_uint64_t attribute;
};

EXTERN_C
__PRIVILEGED_CODE
void init(unsigned int magic, void* mbi) {
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        while (true) { asm volatile ("hlt"); }
    }

    serial::init_port(SERIAL_PORT_BASE_COM1);

    // Architecture-specific initialization sequences
    arch::arch_init();

    serial::com1_printf("Walking MBI tags...\n");

    // Cast the mbi pointer to a byte pointer for arithmetic
    uint8_t* ptr = static_cast<uint8_t*>(mbi);

    // The first 8 bytes are:
    // uint32_t total_size
    // uint32_t reserved
    uint32_t total_size = *reinterpret_cast<uint32_t*>(ptr);
    uint32_t reserved = *reinterpret_cast<uint32_t*>(ptr + 4);

    serial::com1_printf("Total MBI Size: %u bytes\n", total_size);
    serial::com1_printf("Reserved Field: %u\n", reserved);

    // Move pointer past the initial 8 bytes
    ptr += 8;

    // Calculate the end of the MBI structure
    uint8_t* mbi_end = static_cast<uint8_t*>(mbi) + total_size;

    while (ptr < mbi_end) {
        // Interpret the current position as a multiboot_tag
        multiboot_tag* tag = reinterpret_cast<multiboot_tag*>(ptr);

        // Check for the end tag
        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            serial::com1_printf("Reached END tag.\n");
            break;
        }

        // Print tag type and size
        serial::com1_printf("Tag Type: %u, Size: %u bytes\n", tag->type, tag->size);

        // Process the tag based on its type
        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_CMDLINE: {
                multiboot_tag_string* cmdline_tag = reinterpret_cast<multiboot_tag_string*>(tag);
                serial::com1_printf("  Command Line: %s\n", cmdline_tag->string);
                break;
            }
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: {
                multiboot_tag_string* loader_name_tag = reinterpret_cast<multiboot_tag_string*>(tag);
                serial::com1_printf("  Boot Loader Name: %s\n", loader_name_tag->string);
                break;
            }
            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: {
                multiboot_tag_basic_meminfo* meminfo = reinterpret_cast<multiboot_tag_basic_meminfo*>(tag);
                serial::com1_printf("  Basic Memory Info:\n");
                serial::com1_printf("    Lower Memory: %u KB\n", meminfo->mem_lower);
                serial::com1_printf("    Upper Memory: %u KB\n", meminfo->mem_upper);
                break;
            }
            case MULTIBOOT_TAG_TYPE_MMAP: {
                multiboot_tag_mmap* mmap_tag = reinterpret_cast<multiboot_tag_mmap*>(tag);
                serial::com1_printf("  Memory Map:\n");

                // Calculate the number of entries
                uint32_t mmap_size = mmap_tag->size - sizeof(multiboot_tag_mmap);
                uint32_t entry_size = mmap_tag->entry_size;
                uint32_t num_entries = mmap_size / entry_size;

                uint64_t total_system_size_mb = 0;

                for (uint32_t i = 0; i < num_entries; ++i) {
                    multiboot_memory_map_t* entry = reinterpret_cast<multiboot_memory_map_t*>(
                        reinterpret_cast<uint8_t*>(mmap_tag->entries) + i * entry_size
                    );

                    uint64_t base_addr = (static_cast<uint64_t>(entry->base_addr_high) << 32) | entry->base_addr_low;
                    uint64_t length = (static_cast<uint64_t>(entry->length_high) << 32) | entry->length_low;

                    if (entry->type != MULTIBOOT_MEMORY_TYPE_AVAILABLE) {
                        continue;
                    }

                    serial::com1_printf("    type: %u size: %u MB (%u pages)\n    0x%016llx-0x%016llx\n",
                        entry->type, length / 1024 / 1024, length / 4096,
                        base_addr, base_addr + length);

                    total_system_size_mb += length / 1024 / 1024;
                }

                serial::com1_printf("\n    Total System Memory (MBI_MMAP): %llu MB\n", total_system_size_mb);
                break;
            }
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                multiboot_tag_framebuffer* fb_tag = reinterpret_cast<multiboot_tag_framebuffer*>(tag);
                serial::com1_printf("  Framebuffer Information:\n");
                serial::com1_printf("    Address: 0x%llx\n", fb_tag->common.framebuffer_addr);
                serial::com1_printf("    Pitch: %u\n", fb_tag->common.framebuffer_pitch);
                serial::com1_printf("    Width: %u\n", fb_tag->common.framebuffer_width);
                serial::com1_printf("    Height: %u\n", fb_tag->common.framebuffer_height);
                serial::com1_printf("    BPP: %u\n", fb_tag->common.framebuffer_bpp);
                serial::com1_printf("    Type: %u\n", fb_tag->common.framebuffer_type);
                break;
            }
            case MULTIBOOT_TAG_TYPE_EFI_MMAP: { // New case for EFI Memory Map
                multiboot_tag_efi_mmap* efi_mmap_tag = reinterpret_cast<multiboot_tag_efi_mmap*>(tag);
                serial::com1_printf("  EFI Memory Map:\n");

                uint32_t descr_size = efi_mmap_tag->descr_size;

                // Calculate the number of EFI memory descriptors
                uint32_t efi_mmap_size = efi_mmap_tag->size - sizeof(multiboot_tag_efi_mmap);
                uint32_t num_efi_entries = efi_mmap_size / descr_size;

                uint64_t total_system_size_mb = 0;

                for (uint32_t i = 0; i < num_efi_entries; ++i) {
                    uint8_t* desc_ptr = efi_mmap_tag->efi_mmap + i * descr_size;
                    efi_memory_descriptor* desc = reinterpret_cast<efi_memory_descriptor*>(desc_ptr);

                    uint64_t physical_start = desc->physical_start;
                    uint64_t number_of_pages = desc->number_of_pages;
                    uint64_t length = number_of_pages * 4096; // Assuming 4KB pages

                    if (desc->type != 7) {
                        continue;
                    }

                    serial::com1_printf("    Type: %u, Size: %llu MB (%u pages)\n    0x%016llx-0x%016llx  virt: 0x%llx-0x%llx\n",
                        desc->type, length / 1024 / 1024, length / 4096,
                        physical_start, physical_start + length,
                        physical_start + 0xffffffff80000000, physical_start + length + 0xffffffff80000000);

                    total_system_size_mb += length / 1024 / 1024;
                }

                serial::com1_printf("\n    Total System Memory (EFI_MMAP): %llu MB\n", total_system_size_mb);
                break;
            }
            // Add more cases here for other tag types as needed
            default: {
                serial::com1_printf("  [Unhandled Tag Type: %u]\n", tag->type);
                break;
            }
        }

        // Move to the next tag, ensuring 8-byte alignment
        ptr += (tag->size + 7) & ~7;
    }

    serial::com1_printf("Finished walking MBI tags.\n");
}
