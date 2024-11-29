#include <serial/serial.h>
#include <boot/multiboot2.h>
#include <arch/arch_init.h>

EXTERN_C void init(unsigned int magic, void* mbi) {
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

                for (uint32_t i = 0; i < num_entries; ++i) {
                    multiboot_memory_map_t* entry = reinterpret_cast<multiboot_memory_map_t*>(
                        reinterpret_cast<uint8_t*>(mmap_tag->entries) + i * entry_size
                    );

                    uint64_t base_addr = (static_cast<uint64_t>(entry->base_addr_high) << 32) | entry->base_addr_low;
                    uint64_t length = (static_cast<uint64_t>(entry->length_high) << 32) | entry->length_low;

                    if (entry->type != MULTIBOOT_MEMORY_TYPE_AVAILABLE) {
                        continue;
                    }

                    serial::com1_printf("    type:%u size:%uMB\n    phys:%llx-%llx virt:%llx-%llx\n",
                        entry->type, length / 1024 / 1024,
                        base_addr, base_addr + length,
                        base_addr + 0xffffffff80000000, base_addr + 0xffffffff80000000 + length);
                }
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
