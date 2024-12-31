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
#include <modules/graphics/gfx_framebuffer_driver.h>
#include <modules/module_manager.h>
#include <fs/ram_filesystem.h>
#include <fs/vfs.h>

#ifdef BUILD_UNIT_TESTS
#include <acpi/shutdown.h>
#include <unit_tests/unit_tests.h>
#endif // BUILD_UNIT_TESTS

__PRIVILEGED_DATA
char* g_mbi_kernel_cmdline;

__PRIVILEGED_DATA
multiboot_tag_framebuffer* g_mbi_framebuffer;

__PRIVILEGED_DATA
void* g_mbi_efi_mmap;

__PRIVILEGED_DATA
void* g_mbi_acpi_rsdp;

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

    auto task = sched::create_unpriv_kernel_task(module_manager_init, nullptr);
    sched::scheduler::get().add_task(task);

    // Idle loop
    while (true) {
        asm volatile ("hlt");
    }
}

// void ls(fs::filesystem& fs, const kstl::string& path) {
//     kstl::vector<fs::direntry> entries;

//     // List the directory
//     fs::fs_error error = fs.list_directory(path, entries);
//     if (error != fs::fs_error::success) {
//         serial::printf("Error: Unable to list '%s' - %s\n", path.c_str(), fs::error_to_string(error));
//         return;
//     }

//     serial::printf("Listing directory: %s\n", path.c_str());
//     serial::printf("Permissions  Size       Name\n");
//     serial::printf("-----------------------------------\n");

//     for (const auto& entry : entries) {
//         const auto& inode = entry.target_inode();

//         // Null pointer check
//         if (!inode) {
//             serial::printf("Error: Null inode for '%s'\n", entry.name().c_str());
//             continue;
//         }

//         // Permissions
//         char permissions[11] = {'-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '\0'};
//         if (inode->is_directory()) permissions[0] = 'd';
//         if (inode->perms() & 0b100000000) permissions[1] = 'r';
//         if (inode->perms() & 0b010000000) permissions[2] = 'w';
//         if (inode->perms() & 0b001000000) permissions[3] = 'x';
//         if (inode->perms() & 0b000100000) permissions[4] = 'r';
//         if (inode->perms() & 0b000010000) permissions[5] = 'w';
//         if (inode->perms() & 0b000001000) permissions[6] = 'x';
//         if (inode->perms() & 0b000000100) permissions[7] = 'r';
//         if (inode->perms() & 0b000000010) permissions[8] = 'w';
//         if (inode->perms() & 0b000000001) permissions[9] = 'x';

//         // Ensure name is valid
//         if (entry.name().empty()) {
//             serial::printf("Error: Empty name for inode\n");
//             continue;
//         }

//         const auto& name = entry.name();

//         // Print the entry in Linux-like format
//         serial::printf("%s  %u %04s %s\n", permissions, inode->size(), " ", name.c_str());
//     }
// }

void test_ramfs() {
    auto mockfs = kstl::make_shared<fs::ram_filesystem>();

    auto& vfs = fs::virtual_filesystem::get();
    vfs.mount("/", mockfs);

    kstl::string path = "/";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    path = "/home";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    auto status = vfs.create(path, fs::vfs_node_type::directory);
    if (status != fs::fs_error::success) {
        serial::printf("Failed to create '%s': %s\n", path.c_str(), fs::error_to_string(status));
        return;
    }

    path = "/home/subdir/";
    status = vfs.create(path, fs::vfs_node_type::directory);
    if (status != fs::fs_error::success) {
        serial::printf("Failed to create '%s': %s\n", path.c_str(), fs::error_to_string(status));
        return;
    }

    path = "/home/subdir/test_file.txt";
    status = vfs.create(path, fs::vfs_node_type::file);
    if (status != fs::fs_error::success) {
        serial::printf("Failed to create '%s': %s\n", path.c_str(), fs::error_to_string(status));
        return;
    }

    path = "/home";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    path = "/home/var";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    path = "/home/subdir";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));
    
    path = "/home/subdir/test";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    path = "/home/subdir/test_file.txt";
    serial::printf("vfs.path_exists(\"%s\") --> %i\n", path.c_str(), vfs.path_exists(path));

    serial::printf("Reading file '%s'...\n", path.c_str());
    char buf[16] = { 0 };
    ssize_t bytes_read = vfs.read(path, buf, sizeof(buf) - 1, 0);
    if (bytes_read < 0) {
        serial::printf("Error: %s\n", fs::error_to_string(bytes_read));
    } else {
        serial::printf("Bytes read: %lli, buffer: '%s'\n", bytes_read, buf);
    }

    serial::printf("Writing to file '%s'...\n", path.c_str());
    char write_buf[] = "Hello File!";
    ssize_t bytes_written = vfs.write(path, write_buf, sizeof(write_buf), 0);
    if (bytes_written < 0) {
        serial::printf("Error: %s\n", fs::error_to_string(bytes_written));
    } else {
        serial::printf("Bytes written: %lli\n", bytes_written);
    }

    serial::printf("Reading file '%s' again...\n", path.c_str());
    zeromem(buf, 16);
    bytes_read = vfs.read(path, buf, sizeof(buf) - 1, 0);
    if (bytes_read < 0) {
        serial::printf("Error: %s\n", fs::error_to_string(bytes_read));
    } else {
        serial::printf("Bytes read: %lli, buffer: '%s'\n", bytes_read, buf);
    }

    serial::printf("Reading file '%s' again (x2)...\n", path.c_str());
    zeromem(buf, 16);
    bytes_read = vfs.read(path, buf, sizeof(buf) - 1, 0);
    if (bytes_read < 0) {
        serial::printf("Error: %s\n", fs::error_to_string(bytes_read));
    } else {
        serial::printf("Bytes read: %lli, buffer: '%s'\n", bytes_read, buf);
    }

    path = "/";
    kstl::vector<kstl::string> entries;
    status = fs::virtual_filesystem::get().listdir(path, entries);

    if (status == fs::fs_error::success) {
        serial::printf("Contents of '%s':\n", path.c_str());
        for (const auto& name : entries) {
            serial::printf("- %s\n", name.c_str());
        }
    } else {
        serial::printf("Failed to list directory: %s\n", fs::error_to_string(status));
    }

    path = "/home";
    entries.clear();
    status = fs::virtual_filesystem::get().listdir(path, entries);

    if (status == fs::fs_error::success) {
        serial::printf("Contents of '%s':\n", path.c_str());
        for (const auto& name : entries) {
            serial::printf("- %s\n", name.c_str());
        }
    } else {
        serial::printf("Failed to list directory: %s\n", fs::error_to_string(status));
    }

    path = "/home/subdir";
    entries.clear();
    status = fs::virtual_filesystem::get().listdir(path, entries);

    if (status == fs::fs_error::success) {
        serial::printf("Contents of '%s':\n", path.c_str());
        for (const auto& name : entries) {
            serial::printf("- %s\n", name.c_str());
        }
    } else {
        serial::printf("Failed to list directory: %s\n", fs::error_to_string(status));
    }

    status = vfs.remove(path);
    if (status != fs::fs_error::success) {
        serial::printf("Failed to remove '%s': %s\n", path.c_str(), fs::error_to_string(status));
        return;
    }

    path = "/var/subdir";
    entries.clear();
    status = fs::virtual_filesystem::get().listdir(path, entries);

    if (status == fs::fs_error::success) {
        serial::printf("Contents of '%s':\n", path.c_str());
        for (const auto& name : entries) {
            serial::printf("- %s\n", name.c_str());
        }
    } else {
        serial::printf("Failed to list directory: %s\n", fs::error_to_string(status));
    }
}

void module_manager_init(void*) {
    // First create a graphics module to allow rendering to the screen,
    // and for that we need to create a framebuffer information struct.
    modules::gfx_framebuffer_driver::framebuffer_t framebuffer_info;
    zeromem(&framebuffer_info, sizeof(modules::gfx_framebuffer_driver::framebuffer_t));

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
        kstl::make_shared<modules::gfx_framebuffer_driver>(
            gop_framebuffer_address,
            framebuffer_info
        );
    
    // Register and start the gfx module
    auto& module_manager = modules::module_manager::get();
    module_manager.register_module(gfx_driver);
    module_manager.start_module(gfx_driver->name());

    // Iterate over discovered PCI devices and attempt to find driver modules
    //module_manager.start_pci_device_modules();

    test_ramfs();

    sched::exit_thread();
}
