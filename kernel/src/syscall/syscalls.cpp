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
    long return_val = 0;
    __unused arg1;
    __unused arg2;
    __unused arg3;
    __unused arg4;
    __unused arg5;

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
        sched::exit_thread();
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
        int prot_flags = arg3;
        int flags = arg4;
        long offset = arg5;

        // Validate length
        if (length == 0 || length % PAGE_SIZE != 0) {
            return_val = -EINVAL;
            break;
        }

        // Get current process's memory context
        mm_context* mm_ctx = &current->mm_ctx;
        if (!mm_ctx) {
            return_val = -EFAULT;
            break;
        }

        // Calculate number of pages needed
        size_t num_pages = length / PAGE_SIZE;

        // Convert protection flags to VMA flags
        uint64_t vma_flags = 0;
        if (prot_flags & PROT_READ) vma_flags |= VMA_PROT_READ;
        if (prot_flags & PROT_WRITE) vma_flags |= VMA_PROT_WRITE;
        if (prot_flags & PROT_EXEC) vma_flags |= VMA_PROT_EXEC;

        // Convert mapping flags to VMA type
        uint64_t vma_type = 0;
        if (flags & MAP_PRIVATE) vma_type |= VMA_TYPE_PRIVATE;
        if (flags & MAP_SHARED) vma_type |= VMA_TYPE_SHARED;
        if (flags & MAP_ANONYMOUS) vma_type |= VMA_TYPE_ANONYMOUS;

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
        kstl::vector<void*> allocated_pages;
        allocated_pages.reserve(num_pages);

        // Map the pages
        for (size_t i = 0; i < num_pages; i++) {
            void* page_addr = reinterpret_cast<void*>(
                reinterpret_cast<uintptr_t>(addr) + (i * PAGE_SIZE)
            );

            // Allocate physical page
            void* phys_page = allocators::page_bitmap_allocator::get_physical_allocator().alloc_page();
            if (!phys_page) {
                // Clean up already allocated pages
                for (void* page : allocated_pages) {
                    allocators::page_bitmap_allocator::get_physical_allocator().free_page(page);
                }
                remove_vma(mm_ctx, vma);
                return_val = -ENOMEM;
                break;
            }
            allocated_pages.push_back(phys_page);

            // Map the page with appropriate permissions
            uint64_t page_flags = PTE_PRESENT | PTE_US;
            if (vma_flags & VMA_PROT_WRITE) page_flags |= PTE_RW;
            if (!(vma_flags & VMA_PROT_EXEC)) page_flags |= PTE_NX;

            paging::map_page(
                reinterpret_cast<uintptr_t>(page_addr),
                reinterpret_cast<uintptr_t>(phys_page),
                page_flags,
                reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table)
            );

            if (!paging::get_physical_address(page_addr)) {
                // Clean up already allocated pages
                for (void* page : allocated_pages) {
                    allocators::page_bitmap_allocator::get_physical_allocator().free_page(page);
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
        mm_context* mm_ctx = &current->mm_ctx;
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
    case SYSCALL_SYS_ELEVATE: {
        // Make sure that the thread is allowed to elevate
        if (!dynpriv::is_asid_allowed()) {
            kprint("[*] Unauthorized elevation attempt\n");
            return_val = -ENOPRIV;
            break;
        }

        if (current->elevated) {
            kprint("[*] Already elevated\n");
        } else {
            current->elevated = 1;
        }
        break;
    }
    default: {
        kprint("Unknown syscall number %llu\n", syscallnum);
        return_val = -ENOSYS;
        break;
    }
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
    uint64_t arg5,
    uint64_t arg6
) {
    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "mov %7, %%r9\n"   // arg6
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscall_number), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5), "r"(arg6)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );

    return static_cast<long>(ret);
}

