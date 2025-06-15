#include <syscall/syscalls.h>
#include <process/process.h>
#include <core/klog.h>
#include <dynpriv/dynpriv.h>
#include <memory/paging.h>
#include <memory/memory.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <memory/vmm.h>
#include <process/vma.h>
#include <kstl/vector.h>
#include <interrupts/irq.h>
#include <fs/vfs.h>
#include <process/elf/elf64_loader.h>

// Error codes
#define EINVAL 22  // Invalid argument
#define EFAULT 14  // Bad address
#define ENOMEM 12  // Out of memory

// Protection flags
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4

// Mapping flags
#define MAP_PRIVATE  0x1
#define MAP_SHARED   0x2
#define MAP_ANONYMOUS 0x4
#define MAP_FIXED    0x8

EXTERN_C
__PRIVILEGED_CODE
long __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    // Make sure the elevation status patch happens uninterrupted
    disable_interrupts();

    // After faking out being elevated, original elevation
    // privileged should be restored, except in the case
    // when the scheduler switched context into a new task.
    int original_elevate_status = current->get_core()->hw_state.elevated;
    process* original_task = current;

    current->get_core()->hw_state.elevated = 1;

    // Re-enable interrupts after the elevated status is patched
    enable_interrupts();

    // Different syscall cases will set the
    // return value to be returned at the end.
    long return_val = 0;

    switch (syscallnum) {
    case SYSCALL_SYS_WRITE: {
        // Handle write syscall
        kprint(reinterpret_cast<const char*>(arg2));
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_EXIT: {
        sched::exit_process();
        break;
    }
    case SYSCALL_SYS_MMAP: {
        // arg1 = addr (optional)
        // arg2 = length
        // arg3 = prot_flags
        // arg4 = flags
        // arg5 = offset
        void* addr = reinterpret_cast<void*>(arg1);
        size_t length = arg2;
        uint64_t prot_flags = arg3;
        uint64_t flags = arg4;
        uint64_t offset = arg5;

        // Validate length
        if (length == 0 || length % PAGE_SIZE != 0) {
            return_val = -EINVAL;
            break;
        }

        // Get current process's memory context
        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        if (!mm_ctx) {
            return_val = -EFAULT;
            break;
        }

        // Convert protection flags to VMA flags
        uint64_t vma_flags = 0;
        if (prot_flags & PROT_READ) {
            vma_flags |= VMA_PROT_READ;
        }
        if (prot_flags & PROT_WRITE) {
            vma_flags |= VMA_PROT_WRITE;
        }
        if (prot_flags & PROT_EXEC) {
            vma_flags |= VMA_PROT_EXEC;
        }

        // Convert mapping flags to VMA type
        uint64_t vma_type = 0;
        if (flags & MAP_PRIVATE) {
            vma_type |= VMA_TYPE_PRIVATE;
        }
        if (flags & MAP_SHARED) {
            vma_type |= VMA_TYPE_SHARED;
        }
        if (flags & MAP_ANONYMOUS) {
            vma_type |= VMA_TYPE_ANONYMOUS;
        }

        // If addr is NULL or MAP_FIXED is not set, find a suitable address
        if (!addr || !(flags & MAP_FIXED)) {
            addr = reinterpret_cast<void*>(find_free_vma_range(mm_ctx, length, vma_flags, 
                (flags & MAP_FIXED) ? reinterpret_cast<uintptr_t>(addr) : 0));
            if (!addr) {
                return_val = -ENOMEM;
                break;
            }
        } else {
            // Check if the requested address range overlaps with existing VMAs
            vma_area* existing_vma = find_vma(mm_ctx, reinterpret_cast<uintptr_t>(addr));
            if (existing_vma) {
                return_val = -EINVAL;
                break;
            }
        }

        // Create VMA entry
        vma_area* vma = create_vma(mm_ctx, reinterpret_cast<uintptr_t>(addr), length, 
            vma_flags, vma_type, nullptr, offset);
        if (!vma) {
            return_val = -ENOMEM;
            break;
        }

        // Track allocated physical pages
        struct allocated_page {
            void* addr;
            bool is_large;
        };
        kstl::vector<allocated_page> allocated_pages;
        allocated_pages.reserve(length / PAGE_SIZE);

        // Calculate number of large pages and remainder
        size_t large_pages_needed = length / LARGE_PAGE_SIZE;
        size_t remaining_bytes = length % LARGE_PAGE_SIZE;
        size_t small_pages_needed = remaining_bytes / PAGE_SIZE;

        // Set up page flags
        uint64_t page_flags = PTE_PRESENT | PTE_US;
        if (vma_flags & VMA_PROT_WRITE) {
            page_flags |= PTE_RW;
        }
        if (!(vma_flags & VMA_PROT_EXEC)) {
            page_flags |= PTE_NX;
        }

        // First map large pages
        for (size_t i = 0; i < large_pages_needed; i++) {
            void* page_addr = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) + (i * LARGE_PAGE_SIZE)
            );

            // Allocate physical large page
            void* phys_page = allocators::page_bitmap_allocator::get_physical_allocator().alloc_large_page();
            if (!phys_page) {
                // Clean up already allocated pages
                for (const auto& page : allocated_pages) {
                    if (page.is_large) {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_large_page(page.addr);
                    } else {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_page(page.addr);
                    }
                }
                remove_vma(mm_ctx, vma);
                return_val = -ENOMEM;
                break;
            }
            allocated_pages.push_back({phys_page, true});

            // Map the large page
            paging::map_large_page(
                reinterpret_cast<uintptr_t>(page_addr),
                reinterpret_cast<uintptr_t>(phys_page),
                page_flags,
                reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table)
            );

            if (!paging::get_physical_address(page_addr)) {
                // Clean up already allocated pages
                for (const auto& page : allocated_pages) {
                    if (page.is_large) {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_large_page(page.addr);
                    } else {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_page(page.addr);
                    }
                }
                remove_vma(mm_ctx, vma);
                return_val = -EFAULT;
                break;
            }
        }

        // Then map remaining small pages
        for (size_t i = 0; i < small_pages_needed; i++) {
            void* page_addr = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) + (large_pages_needed * LARGE_PAGE_SIZE) + (i * PAGE_SIZE)
            );

            // Allocate physical page
            void* phys_page = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
            if (!phys_page) {
                // Clean up already allocated pages
                for (const auto& page : allocated_pages) {
                    if (page.is_large) {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_large_page(page.addr);
                    } else {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_page(page.addr);
                    }
                }
                remove_vma(mm_ctx, vma);
                return_val = -ENOMEM;
                break;
            }
            allocated_pages.push_back({phys_page, false});

            // Map the page
            paging::map_page(
                reinterpret_cast<uintptr_t>(page_addr),
                reinterpret_cast<uintptr_t>(phys_page),
                page_flags,
                reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table)
            );

            if (!paging::get_physical_address(page_addr)) {
                // Clean up already allocated pages
                for (const auto& page : allocated_pages) {
                    if (page.is_large) {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_large_page(page.addr);
                    } else {
                        allocators::page_bitmap_allocator::get_physical_allocator().free_page(page.addr);
                    }
                }
                remove_vma(mm_ctx, vma);
                return_val = -EFAULT;
                break;
            }
        }

        // Set return value to the allocated address
        return_val = reinterpret_cast<long>(addr);
        break;
    }
    case SYSCALL_SYS_MUNMAP: {
        /*
         * TODO: Improve munmap to properly handle large pages (2MB) vs normal pages (4KB)
         * Current issues:
         * 1. No tracking of whether a page was allocated as a large page or normal page
         * 2. Assumes all pages are 4KB when unmapping, which could lead to:
         *    - Incorrect unmapping of large pages (only unmapping first 4KB)
         *    - Memory leaks if large pages aren't properly freed
         *    - Potential corruption if trying to unmap parts of large pages
         * 
         * Proposed solution:
         * 1. Add page type tracking during mmap (large vs normal)
         * 2. Store this information in the VMA or a separate tracking structure
         * 3. Use this information during munmap to:
         *    - Properly unmap entire large pages when needed
         *    - Handle mixed regions of large and normal pages
         *    - Ensure correct physical page freeing
         */

        // arg1 = addr
        // arg2 = length
        uintptr_t addr = static_cast<uintptr_t>(arg1);
        size_t length = arg2;

        // Validate length
        if (length == 0 || length % PAGE_SIZE != 0) {
            kprint("[MUNMAP] Invalid length: %llu (must be non-zero and page-aligned)\n", length);
            return_val = -EINVAL;
            break;
        }

        // Get current process's memory context
        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        if (!mm_ctx) {
            kprint("[MUNMAP] Invalid memory context\n");
            return_val = -EFAULT;
            break;
        }

        // Find the VMA containing this address
        vma_area* vma = find_vma(mm_ctx, addr);
        if (!vma) {
            kprint("[MUNMAP] No VMA found at address 0x%llx\n", addr);
            return_val = -EINVAL;  // No VMA found at this address
            break;
        }

        // Verify the entire range is within the VMA
        if (addr + length > vma->end) {
            kprint("[MUNMAP] Range extends beyond VMA: addr+len=0x%llx, vma->end=0x%llx\n",
                addr + length, vma->end);
            return_val = -EINVAL;  // Range extends beyond VMA
            break;
        }

        // Calculate number of pages to unmap
        size_t num_pages = length / PAGE_SIZE;

        // Unmap pages and free physical memory
        for (size_t i = 0; i < num_pages; i++) {
            uintptr_t page_addr = addr + (i * PAGE_SIZE);
            void* phys_addr = reinterpret_cast<void*>(paging::get_physical_address(reinterpret_cast<void*>(page_addr)));
            
            if (phys_addr) {
                // Unmap the page
                paging::unmap_page(page_addr, reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table));
                // Free the physical page
                allocators::page_bitmap_allocator::get_physical_allocator().free_page(phys_addr);
            }
        }

        // If we're unmapping the entire VMA, remove it
        if (addr == vma->start && length == (vma->end - vma->start)) {
            remove_vma(mm_ctx, vma);
        } else {
            // Otherwise split the VMA
            if (addr > vma->start) {
                // Split at start of unmapped region
                split_vma(mm_ctx, vma, addr);
            }
            if (addr + length < vma->end) {
                // Split at end of unmapped region
                vma_area* remaining = find_vma(mm_ctx, addr);
                if (remaining) {
                    split_vma(mm_ctx, remaining, addr + length);
                }
            }
        }

        return_val = 0;
        break;
    }
    case SYSCALL_SYS_GETPID: {
        return_val = static_cast<long>(current->get_core()->identity.pid);
        break;
    }
    case SYSCALL_SYS_PROC_CREATE: {
        // arg1 = path to executable
        // arg2 = process creation flags
        const char* path = reinterpret_cast<const char*>(arg1);
        uint64_t flags = arg2;
        __unused flags;

        if (!path) {
            return_val = -EINVAL;
            break;
        }

        // Load the ELF file and create a process core
        process_core* core = elf::elf64_loader::load_from_file(path);
        if (!core) {
            return_val = -ENOMEM;
            break;
        }

        // Create a new process with the loaded core
        process* new_proc = new process();
        if (!new_proc) {
            sched::destroy_process_core(core);
            return_val = -ENOMEM;
            break;
        }

        // Initialize the process with the loaded core
        if (!new_proc->init_with_core(core, process_creation_flags::SCHEDULE_NOW, true)) {
            new_proc->cleanup(); // Will call `destroy_process_core`
            delete new_proc;
            return_val = -ENOMEM;
            break;
        }

        // Return the new process's PID
        return_val = static_cast<long>(core->identity.pid);
        break;
    }
    case SYSCALL_SYS_PROC_WAIT: {
        // arg1 = pid to wait for
        // arg2 = pointer to store exit code
        pid_t pid = static_cast<pid_t>(arg1);
        int* exit_code = reinterpret_cast<int*>(arg2);

        if (pid <= 0) {
            return_val = -EINVAL;
            break;
        }

        // TODO: Implement process waiting
        kprint("SYSCALL_SYS_PROC_WAIT unimplemented!\n");
        if (exit_code) {
            *exit_code = 0;
        }
        return_val = -ENOPRIV;
        break;
    }
    case SYSCALL_SYS_ELEVATE: {
        // Make sure that the thread is allowed to elevate
        if (!dynpriv::is_asid_allowed()) {
            kprint("[*] Unauthorized elevation attempt\n");
            return_val = -ENOPRIV;
            break;
        }

        if (original_elevate_status) {
            kprint("[*] Already elevated\n");
        } else {
            current->get_core()->hw_state.elevated = 1;
        }
        break;
    }
    default: {
        kprint("Unknown syscall number %llu\n", syscallnum);
        return_val = -ENOSYS;
        break;
    }
    }

    if (syscallnum != SYSCALL_SYS_ELEVATE) {
        // Restore the original elevate status
        original_task->get_core()->hw_state.elevated = original_elevate_status;
    }

    return return_val;
}

EXTERN_C
long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscall_number), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8"
    );

    return static_cast<long>(ret);
}

