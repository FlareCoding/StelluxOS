#include <serial/serial.h>
#include <boot/multiboot2.h>
#include <arch/arch_init.h>
#include <interrupts/irq.h>
#include <memory/memory.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <boot/efimem.h>
#include <acpi/acpi.h>
#include <time/time.h>
#include <sched/sched.h>
#include <process/process.h>
#include <smp/smp.h>
#include <dynpriv/dynpriv.h>
#include <modules/graphics/gfx_framebuffer_module.h>
#include <modules/module_manager.h>
#include <fs/ram_filesystem.h>
#include <fs/vfs.h>
#include <fs/cpio/cpio.h>

#ifdef BUILD_UNIT_TESTS
#include <acpi/shutdown.h>
#include <unit_tests/unit_tests.h>
#endif // BUILD_UNIT_TESTS

__PRIVILEGED_DATA
char* g_mbi_kernel_cmdline = nullptr;

__PRIVILEGED_DATA
multiboot_tag_framebuffer* g_mbi_framebuffer = nullptr;

__PRIVILEGED_DATA
void* g_mbi_efi_mmap = nullptr;

__PRIVILEGED_DATA
void* g_mbi_acpi_rsdp = nullptr;

__PRIVILEGED_DATA
multiboot_tag_module* g_initrd_mod = nullptr;

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
            case MULTIBOOT_TAG_TYPE_MODULE: {
                multiboot_tag_module* module_tag = reinterpret_cast<multiboot_tag_module*>(tag);
#if 0
                serial::printf("GRUB_MOD:\n");
                serial::printf("  start   : 0x%llx\n", module_tag->mod_start);
                serial::printf("  end     : 0x%llx\n", module_tag->mod_end);
                serial::printf("  cmdline : '%s'\n", module_tag->cmdline);
#endif

                if (memcmp(module_tag->cmdline, "initrd", 7) == 0) {
                    g_initrd_mod = module_tag;
                }
                break;
            }
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
                g_mbi_framebuffer = reinterpret_cast<multiboot_tag_framebuffer*>(tag);
                break;
            }
            case MULTIBOOT_TAG_TYPE_EFI_MMAP: { // New case for EFI Memory Map
                g_mbi_efi_mmap = reinterpret_cast<void*>(tag);
                break;
            }
            case MULTIBOOT_TAG_TYPE_ACPI_NEW: {
                g_mbi_acpi_rsdp = reinterpret_cast<multiboot_tag_new_acpi*>(tag)->rsdp;
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

__PRIVILEGED_CODE
void load_initrd() {
    if (!g_initrd_mod) {
        return;
    }

    size_t mod_size = g_initrd_mod->mod_end - g_initrd_mod->mod_start;
    size_t mod_page_count = (mod_size + PAGE_SIZE - 1) / PAGE_SIZE;

    void* vaddr = vmm::map_contiguous_physical_pages(g_initrd_mod->mod_start, mod_page_count, DEFAULT_PRIV_PAGE_FLAGS);
    if (!vaddr) {
        serial::printf("[!] Failed to map initrd into kernel's address space\n");
        return;
    }

    // Create a temporary root ("/") mount point
    auto& vfs = fs::virtual_filesystem::get();
    vfs.mount("/", kstl::make_shared<fs::ram_filesystem>());

    fs::load_cpio_initrd(reinterpret_cast<const uint8_t*>(vaddr), mod_size, "/initrd");
}

struct Elf64_Ehdr {
    uint8_t e_ident[16];     // ELF Identification bytes
    uint16_t e_type;         // Object file type
    uint16_t e_machine;      // Machine type
    uint32_t e_version;      // Object file version
    uint64_t e_entry;        // Entry point address
    uint64_t e_phoff;        // Program header offset
    uint64_t e_shoff;        // Section header offset
    uint32_t e_flags;        // Processor-specific flags
    uint16_t e_ehsize;       // ELF header size
    uint16_t e_phentsize;    // Size of a program header entry
    uint16_t e_phnum;        // Number of program header entries
    uint16_t e_shentsize;    // Size of a section header entry
    uint16_t e_shnum;        // Number of section header entries
    uint16_t e_shstrndx;     // Section name string table index
};

struct Elf64_Phdr {
    uint32_t p_type;         // Type of segment
    uint32_t p_flags;        // Segment attributes
    uint64_t p_offset;       // Offset in file
    uint64_t p_vaddr;        // Virtual address in memory
    uint64_t p_paddr;        // Physical address (unused on many platforms)
    uint64_t p_filesz;       // Size of segment in file
    uint64_t p_memsz;        // Size of segment in memory
    uint64_t p_align;        // Alignment of segment
};

struct Elf64_Shdr {
    uint32_t sh_name;        // Section name (string table index)
    uint32_t sh_type;        // Section type
    uint64_t sh_flags;       // Section attributes
    uint64_t sh_addr;        // Virtual address in memory
    uint64_t sh_offset;      // Offset in file
    uint64_t sh_size;        // Size of section
    uint32_t sh_link;        // Link to other section
    uint32_t sh_info;        // Miscellaneous information
    uint64_t sh_addralign;   // Address alignment boundary
    uint64_t sh_entsize;     // Size of entries, if section has a table
};

const char* program_header_type_to_string(uint32_t type) {
    switch (type) {
        case 0x00000000: return "PT_NULL";
        case 0x00000001: return "PT_LOAD";
        case 0x00000002: return "PT_DYNAMIC";
        case 0x00000003: return "PT_INTERP";
        case 0x00000004: return "PT_NOTE";
        case 0x00000005: return "PT_SHLIB";
        case 0x00000006: return "PT_PHDR";
        case 0x6474e551: return "PT_GNU_STACK";
        default: return "UNKNOWN";
    }
}

__PRIVILEGED_CODE
void parse_elf64_file(const uint8_t* file_buffer) {
    constexpr uint32_t ELF_MAGIC = 0x464c457f; // Little endian "\x7FELF"
    
    // Check ELF magic number
    const uint32_t magic = *reinterpret_cast<const uint32_t*>(file_buffer);
    if (magic != ELF_MAGIC) {
        serial::printf("Invalid ELF file: magic = 0x%x\n", magic);
        return;
    }

    paging::page_table* new_pt = paging::create_higher_class_userland_page_table();

    // Whitelist the new page table's ASID
    dynpriv::whitelist_asid(paging::get_physical_address(new_pt));

    // ELF Header
    const Elf64_Ehdr* elf_header = reinterpret_cast<const Elf64_Ehdr*>(file_buffer);
    serial::printf("ELF Entry Point: 0x%llx\n", elf_header->e_entry);
    serial::printf("Program Header Offset: 0x%llx\n", elf_header->e_phoff);
    serial::printf("Section Header Offset: 0x%llx\n", elf_header->e_shoff);
    serial::printf("Number of Program Headers: %d\n", elf_header->e_phnum);
    serial::printf("Number of Section Headers: %d\n", elf_header->e_shnum);
    serial::printf("Section Header String Table Index: %d\n", elf_header->e_shstrndx);

    // Locate Section Header String Table (SHSTRTAB)
    // const auto* section_headers = reinterpret_cast<const Elf64_Shdr*>(file_buffer + elf_header->e_shoff);
    // const auto& shstrtab_header = section_headers[elf_header->e_shstrndx];
    // const char* shstrtab = reinterpret_cast<const char*>(file_buffer + shstrtab_header.sh_offset);

    // Parse Program Headers
    const Elf64_Phdr* program_header = reinterpret_cast<const Elf64_Phdr*>(file_buffer + elf_header->e_phoff);
    for (int i = 0; i < elf_header->e_phnum; ++i) {
        // const char* type_str = program_header_type_to_string(program_header[i].p_type);

        // serial::printf("Program Header [%d]:\n", i);
        // serial::printf("  Type: %s (0x%x)\n", type_str, program_header[i].p_type);
        // serial::printf("  Virtual Address: 0x%llx\n", program_header[i].p_vaddr);
        // serial::printf("  Physical Address: 0x%llx\n", program_header[i].p_paddr);
        // serial::printf("  File Offset: 0x%llx\n", program_header[i].p_offset);
        // serial::printf("  File Size: 0x%llx\n", program_header[i].p_filesz);
        // serial::printf("  Memory Size: 0x%llx\n", program_header[i].p_memsz);
        // serial::printf("  Flags: 0x%x\n", program_header[i].p_flags);
        // serial::printf("  Align: 0x%llx\n", program_header[i].p_align);

        const auto& phdr = program_header[i];
        if (phdr.p_type != 0x00000001) {
            continue; // Only load PT_LOAD segments
        }

        uint64_t segment_vaddr = phdr.p_vaddr;
        uint64_t segment_offset = phdr.p_offset;
        uint64_t segment_filesz = phdr.p_filesz;
        uint64_t segment_memsz = phdr.p_memsz;

        // Align the virtual address and size to page boundaries
        uint64_t aligned_vaddr_start = PAGE_ALIGN_DOWN(segment_vaddr);
        uint64_t aligned_vaddr_end = PAGE_ALIGN_UP(segment_vaddr + segment_memsz);
        uint64_t aligned_size = aligned_vaddr_end - aligned_vaddr_start;

        // Allocate physical pages
        size_t num_pages = aligned_size / PAGE_SIZE;
        auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
        void* phys_memory = physalloc.alloc_pages(num_pages);

        if (!phys_memory) {
            serial::printf("Failed to allocate physical pages for segment [%d]\n", i);
            return;
        }

        // Map the pages to the virtual address space
        paging::map_pages(
            aligned_vaddr_start,
            reinterpret_cast<uintptr_t>(phys_memory),
            num_pages,
            DEFAULT_UNPRIV_PAGE_FLAGS,
            new_pt
        );

        // Temporarily map the segment's physical address into the current address space
        void* phys_memory_mapped_vaddr = paging::phys_to_virt_linear(phys_memory);
        serial::printf("physical address: 0x%llx\n", phys_memory);

        // Copy the file data to the allocated memory
        void* dest_memory = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(phys_memory_mapped_vaddr) + (segment_vaddr % PAGE_SIZE)
        );
        const void* src_memory = reinterpret_cast<const void*>(file_buffer + segment_offset);
        memcpy(dest_memory, src_memory, segment_filesz);

        // Zero out the rest of the memory (if memsz > filesz)
        if (segment_memsz > segment_filesz) {
            void* bss_start = reinterpret_cast<void*>(aligned_vaddr_start + segment_filesz);
            size_t bss_size = segment_memsz - segment_filesz;
            memset(bss_start, 0, bss_size);
        }

        serial::printf(
            "Loaded segment [%d]: vaddr=0x%llx, paddr=0x%llx, filesz=0x%llx, memsz=0x%llx\n",
            i, segment_vaddr, reinterpret_cast<uintptr_t>(phys_memory), segment_filesz, segment_memsz
        );
    }

    // Parse Section Headers
    // for (int i = 0; i < elf_header->e_shnum; ++i) {
    //     const auto& shdr = section_headers[i];
    //     const char* section_name = shstrtab + shdr.sh_name;
    //     if (kstl::string(section_name).starts_with(".debug")) {
    //         continue;
    //     }

    //     serial::printf("Section [%d]:\n", i);
    //     serial::printf("  Name: %s\n", section_name);
    //     serial::printf("  Type: 0x%x\n", shdr.sh_type);
    //     serial::printf("  Address: 0x%llx\n", shdr.sh_addr);
    //     serial::printf("  Offset: 0x%llx\n", shdr.sh_offset);
    //     serial::printf("  Size: 0x%llx\n", shdr.sh_size);
    //     serial::printf("  Flags: 0x%llx\n", shdr.sh_flags);
    // }

    uintptr_t user_stack_address_top = 0x00007fffffffffff;
    const size_t user_stack_pages = 8;
    const size_t user_stack_size = PAGE_SIZE * user_stack_pages;
    const uintptr_t user_stack_start_page = PAGE_ALIGN(user_stack_address_top - user_stack_size);

    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
    void* phys_stack_start_page = physalloc.alloc_pages(user_stack_pages);
    paging::map_pages(
        user_stack_start_page,
        reinterpret_cast<uintptr_t>(phys_stack_start_page),
        user_stack_pages,
        DEFAULT_UNPRIV_PAGE_FLAGS,
        new_pt
    );

    user_stack_address_top -= 0x100;

    task_control_block* task = sched::create_upper_class_userland_task(
        elf_header->e_entry,
        user_stack_address_top,
        new_pt
    );

    sched::scheduler::get().add_task(task, BSP_CPU_ID);
}

__PRIVILEGED_CODE
void test_load_hello_world_from_initrd() {
    auto& vfs = fs::virtual_filesystem::get();
    fs::vfs_stat_struct stat;
    vfs.stat("/initrd/bin/hello_world", stat);

    uint8_t* file_buffer = reinterpret_cast<uint8_t*>(zmalloc(stat.size));
    serial::printf("stat.size: %llu bytes (%llu KB)\n", stat.size, stat.size / 1024);

    vfs.read("/initrd/bin/hello_world", file_buffer, stat.size, 0);
    serial::printf("file_buffer: 0x%llx\n", file_buffer);

    parse_elf64_file(file_buffer);
}

// Since the scheduler will prioritize any other task to the idle task,
// the module manager that will start scheduling future tasks has to get
// started in a thread of its own to avoid getting forever descheduled
// when the first module task gets scheduled.
void module_manager_init(void*);

EXTERN_C
__PRIVILEGED_CODE
void init(unsigned int magic, void* mbi) {
    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        while (true) { asm volatile ("hlt"); }
    }

    // Initialize early stage serial output
    serial::init_port(SERIAL_PORT_BASE_COM1);

    // Architecture-specific initialization sequences
    arch::arch_init();

    // Process and store multiboot provided information
    walk_mbi(mbi);

    // Read the total size of the multiboot information structure
    uint32_t mbi_size = *reinterpret_cast<uint32_t*>(mbi);
    uintptr_t mbi_start_addr = reinterpret_cast<uintptr_t>(mbi);

    // Initialize memory allocators
    paging::init_physical_allocator(g_mbi_efi_mmap, mbi_start_addr, mbi_size);
    paging::init_virtual_allocator();

    // Perform arch-specific initialzation that require VMM
    arch::arch_late_stage_init();

    // Discover ACPI tables
    acpi::enumerate_acpi_tables(g_mbi_acpi_rsdp);

    // Load the initrd if it's available
    load_initrd();

    // Calibrate architecture-specific CPU timer to a tickrate of 4ms
    kernel_timer::calibrate_cpu_timer(4);

    // Start CPU timer in order to receive timer IRQs
    kernel_timer::start_cpu_periodic_timer();

    // Initialize the scheduler
    sched::scheduler::get().init();

    // Initialize SMP and bring up application processors
    smp::smp_init();

#ifdef BUILD_UNIT_TESTS
    // Run unit tests
    execute_unit_tests();

    // Shutdown the machine after running the unit tests
    vmshutdown();
#endif // BUILD_UNIT_TESTS

    test_load_hello_world_from_initrd();

    auto task = sched::create_unpriv_kernel_task(module_manager_init, nullptr);
    sched::scheduler::get().add_task(task);

    // Idle loop
    while (true) {
        asm volatile ("hlt");
    }
}

void module_manager_init(void*) {
    // First create a graphics module to allow rendering to the screen,
    // and for that we need to create a framebuffer information struct.
    modules::gfx_framebuffer_module::framebuffer_t framebuffer_info;
    zeromem(&framebuffer_info, sizeof(modules::gfx_framebuffer_module::framebuffer_t));

    uintptr_t gop_framebuffer_address = 0;
    
    // Since we are now lowered/unprivileged, we have
    // to elevate to access the privileged MBI data.
    RUN_ELEVATED({
        framebuffer_info.width = g_mbi_framebuffer->common.framebuffer_width;
        framebuffer_info.height = g_mbi_framebuffer->common.framebuffer_height;
        framebuffer_info.pitch = g_mbi_framebuffer->common.framebuffer_pitch;
        framebuffer_info.bpp = g_mbi_framebuffer->common.framebuffer_bpp;

        gop_framebuffer_address = g_mbi_framebuffer->common.framebuffer_addr;
    });

    // Create the gfx driver module
    kstl::shared_ptr<modules::module_base> gfx_driver =
        kstl::make_shared<modules::gfx_framebuffer_module>(
            gop_framebuffer_address,
            framebuffer_info
        );

    // Register and start the gfx module
    auto& module_manager = modules::module_manager::get();
    module_manager.register_module(gfx_driver);
    module_manager.start_module(gfx_driver->name());

    // Iterate over discovered PCI devices and attempt to find driver modules
    //module_manager.start_pci_device_modules();

    sched::exit_thread();
}
