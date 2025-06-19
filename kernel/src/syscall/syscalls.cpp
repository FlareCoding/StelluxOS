#include <syscall/syscalls.h>
#include <process/process.h>
#include <core/klog.h>
#include <dynpriv/dynpriv.h>
#include <memory/paging.h>
#include <memory/memory.h>
#include <memory/allocators/page_bitmap_allocator.h>
#include <memory/vmm.h>
#include <process/vma.h>
#include <process/mm.h>
#include <kstl/vector.h>
#include <interrupts/irq.h>
#include <fs/vfs.h>
#include <sched/sched.h>
#include <process/elf/elf64_loader.h>
#include <arch/x86/msr.h>

#define STELLUX_STRACE_ENABLED

// Userland process creation flags
typedef enum {
    PROC_NONE           = 0 << 0,  // Invalid / empty flags
    PROC_SHARE_ENV      = 1 << 0,  // Share environment with parent
    PROC_COPY_ENV       = 1 << 1,  // Copy parent's environment
    PROC_NEW_ENV        = 1 << 2,  // Create new environment
    PROC_CAN_ELEVATE    = 1 << 3,  // Create new environment
} userland_proc_flags_t;

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

EXTERN_C
__PRIVILEGED_CODE
long __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
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
        int fd = static_cast<int>(arg1);
        __unused fd;

        const char* buf = reinterpret_cast<const char*>(arg2);
        size_t count = static_cast<size_t>(arg3);
        
        // Write to serial port
        serial::write(serial::g_kernel_uart_port, buf, count);
        
#ifdef STELLUX_STRACE_ENABLED
        kprint("write(%i, \"0x%llx\", %llu) = %llu\n", fd, reinterpret_cast<uint64_t>(buf), count, count);
#endif
        return_val = static_cast<long>(count);
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_EXIT_GROUP:
    case SYSCALL_SYS_EXIT: {
        current->get_core()->exit_code = arg1;
#ifdef STELLUX_STRACE_ENABLED
        kprint("exit_group(%llu) = ?\n", arg1);
#endif
        sched::exit_process();
        break;
    }
    case SYSCALL_SYS_MMAP: {
        uintptr_t addr = static_cast<uintptr_t>(arg1);
        size_t length = static_cast<size_t>(arg2);
        int prot = static_cast<int>(arg3);
        int flags = static_cast<int>(arg4);
        int fd = static_cast<int>(arg5);
        size_t offset = static_cast<size_t>(arg6);

        __unused fd;
        
#ifdef STELLUX_STRACE_ENABLED
        kprint("mmap(0x%llx, %llu, %i, %i, %i, %lli) = ", addr, length, prot, flags, fd, offset);
#endif

        // Parameter validation
        
        // Validate length
        if (length == 0) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (length is 0)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Check for length overflow when page-aligned
        if (length > SIZE_MAX - PAGE_SIZE + 1) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-ENOMEM (length too large)\n");
#endif
            return_val = -ENOMEM;
            break;
        }
        
        // Validate protection flags
        if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (invalid prot flags)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Validate mapping flags
        const int valid_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS;
        if (flags & ~valid_flags) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (invalid map flags)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Must specify either MAP_SHARED or MAP_PRIVATE, but not both
        if (!(flags & (MAP_SHARED | MAP_PRIVATE)) || 
            ((flags & MAP_SHARED) && (flags & MAP_PRIVATE))) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (must specify exactly one of MAP_SHARED or MAP_PRIVATE)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // For now, only support anonymous mappings
        if (!(flags & MAP_ANONYMOUS)) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-ENOSYS (file-backed mappings not yet supported)\n");
#endif
            return_val = -ENOSYS;
            break;
        }
        
        // Validate address alignment if specified
        if (addr && (addr & (PAGE_SIZE - 1))) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (address not page-aligned)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // For anonymous mappings, offset should be 0
        if ((flags & MAP_ANONYMOUS) && offset != 0) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (offset must be 0 for anonymous mappings)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Page-align length (round up)
        length = PAGE_ALIGN_UP(length);
        
        // Find virtual address
        uintptr_t target_addr;
        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        
        if (flags & MAP_FIXED) {
            // User demands specific address
            if (!addr) {
#ifdef STELLUX_STRACE_ENABLED
                kprint("-EINVAL (MAP_FIXED requires non-null address)\n");
#endif
                return_val = -EINVAL;
                break;
            }
            target_addr = addr;
            
            // For MAP_FIXED, Linux behavior: automatically unmap any existing mappings in this range
            // Find all VMAs that overlap with [target_addr, target_addr + length)
            uintptr_t range_end = target_addr + length;
            vma_area* vma = mm_ctx->vma_list;
            
            while (vma) {
                vma_area* next_vma = vma->next;  // Save next before potential deletion
                
                // Check if this VMA overlaps with our target range
                if (vma->start < range_end && vma->end > target_addr) {
                    uintptr_t overlap_start = vma->start > target_addr ? vma->start : target_addr;
                    uintptr_t overlap_end = vma->end < range_end ? vma->end : range_end;
                    
                    // Unmap physical pages in the overlapping region
                    paging::page_table* pt = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
                    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
                    
                    for (uintptr_t page_addr = PAGE_ALIGN_DOWN(overlap_start); 
                         page_addr < PAGE_ALIGN_UP(overlap_end); 
                         page_addr += PAGE_SIZE) {
                        
                        void* phys_addr = reinterpret_cast<void*>(
                            paging::get_physical_address(reinterpret_cast<void*>(page_addr), pt)
                        );
                        if (phys_addr) {
                            paging::unmap_page(page_addr, pt);
                            physalloc.free_page(phys_addr);
                        }
                    }
                    
                    // Handle VMA splitting/removal based on overlap type
                    if (vma->start >= target_addr && vma->end <= range_end) {
                        // VMA is completely within target range - remove it entirely
                        remove_vma(mm_ctx, vma);
                    } else if (vma->start < target_addr && vma->end > range_end) {
                        // VMA completely contains target range - split into two parts
                        // First, split at range_end to create the second part
                        vma_area* second_part = split_vma(mm_ctx, vma, range_end);
                        if (second_part) {
                            // Now split the first part at target_addr
                            split_vma(mm_ctx, vma, target_addr);
                            // The middle part (between target_addr and range_end) is now a separate VMA
                            // Find and remove it
                            vma_area* middle_part = find_vma(mm_ctx, target_addr);
                            if (middle_part) {
                                remove_vma(mm_ctx, middle_part);
                            }
                        }
                    } else if (vma->start < target_addr && vma->end > target_addr) {
                        // VMA overlaps start of target range - truncate it
                        vma->end = target_addr;
                    } else if (vma->start < range_end && vma->end > range_end) {
                        // VMA overlaps end of target range - truncate from start
                        vma->start = range_end;
                    }
                }
                
                vma = next_vma;
            }
        } else {
            // Kernel chooses address
            target_addr = find_free_vma_range(mm_ctx, length, 0, addr);
            if (!target_addr) {
#ifdef STELLUX_STRACE_ENABLED
                kprint("-ENOMEM (no free address range found)\n");
#endif
                return_val = -ENOMEM;
                break;
            }
        }
        
        // Create the VMA
        uint64_t vma_prot = prot;
        
        uint64_t vma_type = VMA_TYPE_ANONYMOUS;
        if (flags & MAP_SHARED) {
            vma_type |= VMA_TYPE_SHARED;
        } else {
            vma_type |= VMA_TYPE_PRIVATE;
        }
        
        vma_area* vma = create_vma(mm_ctx, target_addr, length, vma_prot, vma_type);
        if (!vma) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-ENOMEM (failed to create VMA)\n");
#endif
            return_val = -ENOMEM;
            break;
        }
        
        // Allocate and map physical memory
        // For now, use eager allocation (allocate all pages immediately)
        paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
        auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
        
        size_t num_pages = length / PAGE_SIZE;
        
        // Convert protection flags to page table flags
        uint64_t page_flags = PTE_PRESENT | PTE_US;  // User accessible
        if (prot & PROT_WRITE) {
            page_flags |= PTE_RW;
        }
        if (!(prot & PROT_EXEC)) {
            page_flags |= PTE_NX;
        }
        
        // Allocate and map pages one by one to avoid needing contiguous physical memory
        bool allocation_failed = false;
        size_t pages_allocated = 0;
        
        for (size_t i = 0; i < num_pages; i++) {
            uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
            
            void* phys_page = physalloc.alloc_page();
            if (!phys_page) {
                allocation_failed = true;
                break;
            }
            
            // Map the page
            paging::map_page(virt_addr, reinterpret_cast<uintptr_t>(phys_page), page_flags, page_table);
            
            // Zero out the page for security
            void* virt_page = paging::phys_to_virt_linear(phys_page);
            if (virt_page) {
                memset(virt_page, 0, PAGE_SIZE);
            }
            
            pages_allocated++;
        }
        
        // Handle allocation failure
        if (allocation_failed) {
            // Clean up: unmap and free allocated pages
            for (size_t i = 0; i < pages_allocated; i++) {
                uintptr_t virt_addr = target_addr + (i * PAGE_SIZE);
                void* phys_addr = reinterpret_cast<void*>(
                    paging::get_physical_address(reinterpret_cast<void*>(virt_addr), page_table)
                );
                if (phys_addr) {
                    paging::unmap_page(virt_addr, page_table);
                    physalloc.free_page(phys_addr);
                }
            }
            
            // Remove the VMA
            remove_vma(mm_ctx, vma);
            
#ifdef STELLUX_STRACE_ENABLED
            kprint("-ENOMEM (failed to allocate physical pages)\n");
#endif
            return_val = -ENOMEM;
            break;
        }
        
        // Success!
#ifdef STELLUX_STRACE_ENABLED
        kprint("0x%llx\n", target_addr);
#endif
        return_val = static_cast<long>(target_addr);
        break;
    }
    case SYSCALL_SYS_MUNMAP: {
        uintptr_t addr = static_cast<uintptr_t>(arg1);
        size_t length = static_cast<size_t>(arg2);
        
#ifdef STELLUX_STRACE_ENABLED
        kprint("munmap(0x%llx, %llu) = ", addr, length);
#endif

        // Parameter validation
        
        // Check for null address
        if (!addr) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (null address)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Check for zero length
        if (length == 0) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (zero length)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Check address alignment (must be page-aligned)
        if (addr & (PAGE_SIZE - 1)) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (address not page-aligned)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Check for length overflow when page-aligned
        if (length > SIZE_MAX - PAGE_SIZE + 1) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (length too large)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        // Page-align length (round up)
        length = PAGE_ALIGN_UP(length);
        
        // Validate address range is within user space
        if (addr < USERSPACE_START || addr > USERSPACE_END - length) {
#ifdef STELLUX_STRACE_ENABLED
            kprint("-EINVAL (address range outside user space)\n");
#endif
            return_val = -EINVAL;
            break;
        }
        
        mm_context* mm_ctx = &current->get_core()->mm_ctx;
        paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
        auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
        
        uintptr_t range_end = addr + length;
        
        // Find all VMAs that overlap with [addr, addr + length) and handle them
        vma_area* vma = mm_ctx->vma_list;
        
        while (vma) {
            vma_area* next_vma = vma->next;  // Save next before potential deletion/modification
            
            // Check if this VMA overlaps with our target range
            if (vma->start < range_end && vma->end > addr) {
                uintptr_t overlap_start = vma->start > addr ? vma->start : addr;
                uintptr_t overlap_end = vma->end < range_end ? vma->end : range_end;
                
                // Unmap physical pages in the overlapping region
                for (uintptr_t page_addr = PAGE_ALIGN_DOWN(overlap_start); 
                     page_addr < PAGE_ALIGN_UP(overlap_end); 
                     page_addr += PAGE_SIZE) {
                    
                    void* phys_addr = reinterpret_cast<void*>(
                        paging::get_physical_address(reinterpret_cast<void*>(page_addr), page_table)
                    );
                    if (phys_addr) {
                        paging::unmap_page(page_addr, page_table);
                        physalloc.free_page(phys_addr);
                    }
                    // Note: We gracefully handle pages that aren't mapped (Linux behavior)
                }
                
                // Handle VMA modification based on overlap type
                if (vma->start >= addr && vma->end <= range_end) {
                    // Case 1: VMA is completely within munmap range - remove it entirely
                    remove_vma(mm_ctx, vma);
                } else if (vma->start < addr && vma->end > range_end) {
                    // Case 4: VMA completely contains munmap range - split into two parts
                    // First, split at range_end to create the second part
                    vma_area* second_part = split_vma(mm_ctx, vma, range_end);
                    if (second_part) {
                        // Now split the first part at addr
                        vma_area* middle_part = split_vma(mm_ctx, vma, addr);
                        // The middle part (between addr and range_end) needs to be removed
                        if (middle_part) {
                            remove_vma(mm_ctx, middle_part);
                        }
                    }
                } else if (vma->start < addr && vma->end > addr) {
                    // Case 2: VMA overlaps start of munmap range - shrink from end
                    vma->end = addr;
                } else if (vma->start < range_end && vma->end > range_end) {
                    // Case 3: VMA overlaps end of munmap range - shrink from start
                    // Adjust file offset if this is a file-backed mapping
                    if (vma->file_backing) {
                        vma->file_offset += (range_end - vma->start);
                    }
                    vma->start = range_end;
                }
                
                // Try to merge with adjacent VMAs after modification (optimization)
                if (vma->start < addr || vma->end > range_end) {
                    // Only try to merge if the VMA still exists and wasn't completely removed
                    merge_vmas(mm_ctx, vma);
                }
            }
            
            vma = next_vma;
        }
        
        // Success! Linux munmap always succeeds if parameters are valid
#ifdef STELLUX_STRACE_ENABLED
        kprint("0\n");
#endif
        return_val = 0;
        break;
    }
    case SYSCALL_SYS_IOCTL: {
        int fd = static_cast<int>(arg5);
        __unused fd;

        uint64_t req = arg2;
        void* userbuf = reinterpret_cast<void*>(arg3); 

        switch (req) {
        case 0x5413: { /* TIOCGWINSZ */
            struct winsize {
                unsigned short ws_row;
                unsigned short ws_col;
                unsigned short ws_xpixel;
                unsigned short ws_ypixel;
            };
            
            struct winsize ws = { 24, 80, 0, 0 };
            memcpy(userbuf, &ws, sizeof(struct winsize));
#ifdef STELLUX_STRACE_ENABLED
            kprint("ioctl(%llu, TIOCGWINSZ, {ws_row=%d, ws_col=%d, ws_xpixel=%d, ws_ypixel=%d}) = 0\n", 
                   fd, ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
#endif
            return_val = 0;
            break;
        }
        default:
            return_val = -ENOTTY; /* "not a terminal" */
            break;
        }
        break;
    }
    case SYSCALL_SYS_WRITEV: {
        /* -----------------------------------------------------------------
         *  Linux-style calling convention
         *      arg1 = int        fd
         *      arg2 = const struct iovec *  iov       (user pointer)
         *      arg3 = size_t     vlen
         *  We ignore arg4-arg5.
         * -----------------------------------------------------------------*/

        /* Minimal local definition; nothing needs to leak outside */
        struct iovec {
            void   *iov_base;   /* start of user buffer           */
            size_t  iov_len;    /* length of that buffer in bytes */
        };

        int fd = static_cast<int>(arg5);
        __unused fd;

        iovec* iov  = reinterpret_cast<iovec*>(arg2); /* user pointer */
        size_t vlen = arg3;

        size_t total_written = 0;

#ifdef STELLUX_STRACE_ENABLED
        kprint("writev(%llu, [", fd);
#endif
        for (size_t i = 0; i < vlen; ++i) {
            /* Copy the iovec descriptor itself onto the kernel stack. */
            struct iovec k_iov;
            memcpy(&k_iov, &iov[i], sizeof k_iov);

#ifdef STELLUX_STRACE_ENABLED
            if (i > 0) kprint(", ");
            kprint("{iov_base=0x%llx, iov_len=%llu}", reinterpret_cast<uint64_t>(k_iov.iov_base), k_iov.iov_len);
#endif

            if (k_iov.iov_len == 0) {
                continue;
            }

            ssize_t n = k_iov.iov_len;
            serial::write(serial::g_kernel_uart_port, reinterpret_cast<char*>(k_iov.iov_base), n);

            if (n < 0) {                 /* propagate error as-is */
                return_val = n;
                break;
            }

            total_written += (size_t)n;

            if ((size_t)n < k_iov.iov_len) {
                break;   /* short write – stop early like Linux */
            }
        }
#ifdef STELLUX_STRACE_ENABLED
        kprint("], %llu) = %llu\n", vlen, total_written);
#endif

        /* On success return the total number of bytes consumed.  */
        if (total_written > 0 || return_val == 0) {
            return_val = (ssize_t)total_written;
        }
        break;
    }
    case SYSCALL_SYS_GETPID: {
        pid_t pid = current->get_core()->identity.pid;
        
#ifdef STELLUX_STRACE_ENABLED
        kprint("getpid() = %llu\n", pid);
#endif
        
        return_val = static_cast<long>(pid);
        break;
    }
    case SYSCALL_SYS_PROC_CREATE: {
        // arg1 = path to executable
        // arg2 = process creation flags
        // arg3 = access rights
        // arg4 = handle flags
        // arg5 = pointer to proc_info struct
        const char* path = reinterpret_cast<const char*>(arg1);
        uint64_t userland_proc_fl = arg2;
        uint32_t access_rights = static_cast<uint32_t>(arg3);
        uint32_t handle_flags = static_cast<uint32_t>(arg4);
        struct userland_proc_info {
            pid_t pid;          // Process ID
            char name[256];     // Process name
        } *info = reinterpret_cast<struct userland_proc_info*>(arg5);

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

        auto new_proc_flags = process_creation_flags::SCHEDULE_NOW;

        if (userland_proc_fl & PROC_CAN_ELEVATE) {
            new_proc_flags |= process_creation_flags::CAN_ELEVATE;
        }

        // Initialize the process with the loaded core
        if (!new_proc->init_with_core(core, new_proc_flags, true)) {
            new_proc->cleanup(); // Will call `destroy_process_core`
            delete new_proc;
            return_val = -ENOMEM;
            break;
        }

        // Add a handle to the new process in the parent's environment
        size_t handle_index = current->get_env()->handles.add_handle(
            handle_type::PROCESS,
            new_proc,
            access_rights,
            handle_flags,
            static_cast<uint64_t>(core->identity.pid) // Store PID in metadata
        );

        if (handle_index == SIZE_MAX) {
            // If we can't add the handle, we need to clean up the process
            new_proc->cleanup();
            delete new_proc;
            return_val = -ENOMEM;
            break;
        }

        // Increment the reference count for the parent's handle
        new_proc->add_ref();

        // Fill in process info if requested
        if (info) {
            info->pid = core->identity.pid;
            memcpy(info->name, core->identity.name, sizeof(info->name) - 1);
            info->name[sizeof(info->name) - 1] = '\0';
        }

        // Return the handle index
        return_val = static_cast<long>(handle_index);
        break;
    }
    case SYSCALL_SYS_PROC_WAIT: {
        // arg1 = handle to wait for
        // arg2 = pointer to store exit code
        int32_t handle = static_cast<int32_t>(arg1);
        int* exit_code = reinterpret_cast<int*>(arg2);

        if (handle < 0) {
            return_val = -EINVAL;
            break;
        }

        // Get the handle entry
        handle_entry* hentry = current->get_env()->handles.get_handle(handle);
        if (!hentry || hentry->type != handle_type::PROCESS) {
            return_val = -EINVAL;  // Invalid handle
            break;
        }

        // Get the process pointer from the handle
        process* target_proc = reinterpret_cast<process*>(hentry->object);
        if (!target_proc) {
            return_val = -EINVAL;  // Invalid handle
            break;
        }

        // Wait for the process to terminate
        while (target_proc->get_core()->state != process_state::TERMINATED) {
            // Yield to other processes while waiting
            sched::yield();
        }

        // Get the exit code if requested
        if (exit_code) {
            // TODO: Implement proper exit code tracking
            *exit_code = 0;
        }

        // Remove the handle and release our reference
        current->get_env()->handles.remove_handle(handle);
        target_proc->release_ref();

        // Add the process to the cleanup queue if needed
        if (target_proc->get_core()->ctx_switch_state.needs_cleanup == 1) {
            sched::scheduler::get().add_to_cleanup_queue(target_proc);
        }

        return_val = 0;
        break;
    }
    case SYSCALL_SYS_PROC_CLOSE: {
        // Process handle flags
        enum proc_handle_flags_t {
            PROC_HANDLE_NONE    = 0 << 0,  // No special flags
            PROC_HANDLE_INHERIT = 1 << 0,  // Handle is inherited by child processes
            PROC_HANDLE_PROTECT = 1 << 1   // Handle cannot be closed
        };

        // arg1 = handle to close
        int32_t handle = static_cast<int32_t>(arg1);

        if (handle < 0) {
            return_val = -EINVAL;
            break;
        }

        // Get the handle entry
        handle_entry* hentry = current->get_env()->handles.get_handle(handle);
        if (!hentry) {
            return_val = -EINVAL;  // Invalid handle
            break;
        }

        // Check if handle is protected
        if (hentry->flags & PROC_HANDLE_PROTECT) {
            return_val = -EACCES;  // Handle is protected
            break;
        }

        // Get the object pointer
        void* object = hentry->object;
        if (!object) {
            return_val = -EINVAL;  // Invalid handle
            break;
        }

        // Handle type-specific cleanup
        switch (hentry->type) {
        case handle_type::PROCESS: {
            process* proc = reinterpret_cast<process*>(object);
            proc->release_ref();

            // Add the process to the cleanup queue if needed
            if (proc->get_core()->ctx_switch_state.needs_cleanup == 1) {
                sched::scheduler::get().add_to_cleanup_queue(proc);
            }
            break;
        }
        // Add more handle type cleanup as needed
        default:
            break;
        }

        // Remove the handle
        if (!current->get_env()->handles.remove_handle(handle)) {
            return_val = -EINVAL;
            break;
        }

        return_val = 0;
        break;
    }
    case SYSCALL_SYS_BRK: {
        uintptr_t new_heap_end = static_cast<uintptr_t>(arg1);
        
        // If arg1 is 0, just return the current heap end
        if (new_heap_end == 0) {
            return_val = static_cast<long>(current->get_core()->mm_ctx.heap_end);
        } else {
            // Try to set the new heap end
            if (manage_process_heap(&current->get_core()->mm_ctx, new_heap_end)) {
                return_val = static_cast<long>(current->get_core()->mm_ctx.heap_end);
            } else {
                // On failure, return the current heap end (Linux behavior)
                return_val = static_cast<long>(current->get_core()->mm_ctx.heap_end);
            }
        }
        
#ifdef STELLUX_STRACE_ENABLED
        kprint("brk(0x%llx) = 0x%llx\n", arg1, return_val);
#endif
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
    case SYSCALL_SYS_SET_THREAD_AREA: {
        uint64_t code = arg1;
        uint64_t tls_userptr = arg2;
        uint64_t tls_size = arg3;
        __unused tls_size;

        switch (code) {
        case ARCH_SET_FS: {
            current->get_core()->fs_base = tls_userptr;
#ifdef STELLUX_STRACE_ENABLED
            kprint("arch_prctl(ARCH_SET_FS, 0x%llx) = 0\n", tls_userptr);
#endif
            arch::x86::msr::write(IA32_FS_BASE, tls_userptr);
            return_val = 0;
            break;
        }
        case ARCH_GET_FS: {
#ifdef STELLUX_STRACE_ENABLED
            kprint("arch_prctl(ARCH_GET_FS, 0x%llx) = 0 [writing 0x%llx]\n", tls_userptr, current->get_core()->fs_base);
#endif
            *reinterpret_cast<uint64_t*>(tls_userptr) = current->get_core()->fs_base;
            return_val = 0;
            break;
        }
        default: {
            return_val = -EINVAL;
            break;
        }
        }
        break;
    }
    case SYSCALL_SYS_SET_TID_ADDRESS: {
#ifdef STELLUX_STRACE_ENABLED
        kprint("set_tid_address(0x%llx) = %d\n", arg1, current->get_core()->identity.pid);
#endif
        return_val = current->get_core()->identity.pid;
        break;
    }
    default: {
        kprint("Unknown syscall number %llu\n", syscallnum);
        return_val = -ENOSYS;
        break;
    }
    }

    // Condition under which the SYS_ELEVATE call succeeded and
    // restoration of original elevate status is not necessary.
    bool successfull_elevation_syscall =
        (syscallnum == SYSCALL_SYS_ELEVATE) && (return_val == 0);

    if (!successfull_elevation_syscall) {
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

