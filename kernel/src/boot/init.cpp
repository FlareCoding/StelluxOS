#include <core/klog.h>
#include <serial/serial.h>
#include <boot/multiboot2.h>
#include <arch/arch_init.h>
#include <interrupts/irq.h>
#include <memory/memory.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <acpi/acpi.h>
#include <time/time.h>
#include <sched/sched.h>
#include <process/process.h>
#include <process/elf/elf64_loader.h>
#include <smp/smp.h>
#include <dynpriv/dynpriv.h>
#include <modules/graphics/gfx_framebuffer_module.h>
#include <modules/pci/pci_manager_module.h>
#include <modules/module_manager.h>
#include <fs/ram_filesystem.h>
#include <fs/vfs.h>
#include <fs/cpio/cpio.h>
#include <input/system_input_manager.h>
#include <gdb/gdb_stub.h>

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
void* g_mbi_fallback_mmap = nullptr;

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
            case MULTIBOOT_TAG_TYPE_MMAP: {
                g_mbi_fallback_mmap = reinterpret_cast<multiboot_tag_mmap*>(tag);
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
    paging::init_physical_allocator(g_mbi_efi_mmap, g_mbi_fallback_mmap, mbi_start_addr, mbi_size);
    paging::init_virtual_allocator();

    // Initialize kernel logging subsystem
    klog::logger::init(8);

    // Perform arch-specific initialzation that require VMM
    arch::arch_late_stage_init();

    // Discover ACPI tables
    acpi::enumerate_acpi_tables(g_mbi_acpi_rsdp);

    // Connect to the GDB session if the gdb server stub is enabled
    kstl::string cmdline_args = kstl::string(g_mbi_kernel_cmdline);
    if (cmdline_args.find("enable-gdb-stub") != kstl::string::npos) {
        // Connect to the GDB stub
        serial::printf("[*] Waiting for the GDB stub to connect...\n");
        gdb_stub::perform_initial_trap();
    }

    // Load the initrd if it's available
    load_initrd();

    // Calibrate architecture-specific CPU timer to a tickrate of 4ms
    kernel_timer::calibrate_cpu_timer(4);

    // Start CPU timer in order to receive timer IRQs
    kernel_timer::start_cpu_periodic_timer();

    // Initialize the scheduler
    sched::scheduler::get().init();

    // Initialize SMP and bring up application processors
    if (cmdline_args.find("nosmp") == kstl::string::npos) {
        smp::smp_init();   
    }

#ifdef BUILD_UNIT_TESTS
    // Run unit tests
    execute_unit_tests();

    // Shutdown the machine after running the unit tests
    vmshutdown();
#endif // BUILD_UNIT_TESTS

    auto task = sched::create_unpriv_kernel_task(module_manager_init, nullptr);
    memcpy(task->name, "module_manager_init", 19);

    sched::scheduler::get().add_task(task);

    // Idle loop
    while (true) {
        asm volatile ("hlt");
    }
}

void module_manager_init(void*) {
    // Initializes the system-wide input kernel subsystem
    input::system_input_manager::get().init();

    // After input queues have initialized, setup IRQ handling for
    // COM1 input to be processed as a proper system input source.
    RUN_ELEVATED({
        arch::setup_com1_irq();
    });

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
    kstl::shared_ptr<modules::module_base> gfx_module =
        kstl::make_shared<modules::gfx_framebuffer_module>(
            gop_framebuffer_address,
            framebuffer_info
        );

    // Register and start the gfx module
    auto& module_manager = modules::module_manager::get();
    module_manager.register_module(gfx_module);
    module_manager.start_module(gfx_module.get());

    // Create and start the PCI manager module
    kstl::shared_ptr<modules::module_base> pci_mngr =
        kstl::make_shared<modules::pci_manager_module>();

    module_manager.register_module(pci_mngr);
    module_manager.start_module(pci_mngr.get());

    // Load and start the init process
    RUN_ELEVATED({
        task_control_block* task = elf::elf64_loader::load_from_file("/initrd/bin/init");
        if (!task) {
            return;
        }

        // Allow the process to elevate privileges
        dynpriv::whitelist_asid(task->mm_ctx.root_page_table);
        sched::scheduler::get().add_task(task);
    });

    sched::exit_thread();
}
