#include <syscall/handlers/sys_graphics.h>
#include <memory/memory.h>
#include <core/klog.h>
#include <modules/module_manager.h>
#include <modules/graphics/gfx_framebuffer_module.h>
#include <memory/paging.h>
#include <memory/vmm.h>
#include <process/vma.h>
#include <process/mm.h>
#include <process/process.h>
#include <time/time.h>

DECLARE_SYSCALL_HANDLER(gfx_fb_op) {
    uint64_t operation = arg1;
    
    SYSCALL_TRACE("gfx_fb_op(%llu, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) = ", 
                  operation, arg2, arg3, arg4, arg5, arg6);
    
    // Find the graphics framebuffer module
    modules::module_base* base_module = modules::module_manager::get().find_module("gfx_framebuffer_module");
    if (!base_module) {
        SYSCALL_TRACE("-ENOENT [graphics module not found]\n");
        return -ENOENT;
    }
    
    modules::gfx_framebuffer_module* gfx_module = 
        static_cast<modules::gfx_framebuffer_module*>(base_module);

    int timeout = 6;
    while (--timeout && gfx_module->state() != modules::module_state::running) {
        sleep(1);
    }
    
    if (gfx_module->state() != modules::module_state::running) {
        SYSCALL_TRACE("-ETIMEDOUT [graphics module not ready]\n");
        return -EIO;
    }

    switch (operation) {
        case GFX_OP_GET_INFO: {
            // arg2 should be a pointer to gfx_framebuffer_info struct
            gfx_framebuffer_info* user_info = reinterpret_cast<gfx_framebuffer_info*>(arg2);
            
            if (!user_info) {
                SYSCALL_TRACE("-EINVAL [null pointer]\n");
                return -EINVAL;
            }
            
            // Get real framebuffer info from the module
            if (!gfx_module->get_framebuffer_info(user_info)) {
                SYSCALL_TRACE("-EIO [failed to get framebuffer info]\n");
                return -EIO;
            }
            
            SYSCALL_TRACE("0 [GET_INFO: %dx%d, pitch=%d, bpp=%d, size=%d]\n", 
                         user_info->width, user_info->height, user_info->pitch, 
                         user_info->bpp, user_info->size);
            return 0;
        }
        case GFX_OP_MAP_FRAMEBUFFER: {        
            // Get framebuffer physical address and size
            uintptr_t phys_addr = gfx_module->get_physical_address();
            size_t fb_size = gfx_module->get_total_fb_size();
            
            if (!phys_addr || !fb_size) {
                SYSCALL_TRACE("-EIO [invalid framebuffer parameters]\n");
                return -EIO;
            }
            
            // Page-align the size
            size_t aligned_size = PAGE_ALIGN_UP(fb_size);
            size_t num_pages = aligned_size / PAGE_SIZE;
            
            // Find a virtual address for the mapping
            mm_context* mm_ctx = &current->get_core()->mm_ctx;
            uintptr_t target_addr = find_free_vma_range(mm_ctx, aligned_size, 0, 0);
            if (!target_addr) {
                SYSCALL_TRACE("-ENOMEM [no free address range found]\n");
                return -ENOMEM;
            }
            
            // Create VMA for the framebuffer mapping
            uint64_t vma_prot = PROT_READ | PROT_WRITE;
            uint64_t vma_type = VMA_TYPE_SHARED;
            
            vma_area* vma = create_vma(mm_ctx, target_addr, aligned_size, vma_prot, vma_type);
            if (!vma) {
                SYSCALL_TRACE("-ENOMEM [failed to create VMA]\n");
                return -ENOMEM;
            }
            
            // Map the physical framebuffer pages to virtual memory
            paging::page_table* page_table = reinterpret_cast<paging::page_table*>(mm_ctx->root_page_table);
            
            // Use standard page flags for now (PAT optimization comes in Step 6)
            uint64_t page_flags = PTE_PRESENT | PTE_US | PTE_RW | PTE_PAT; // PTE_PAT = WriteCombining
            
            // Map pages one by one
            for (size_t i = 0; i < num_pages; i++) {
                uintptr_t virt_page = target_addr + (i * PAGE_SIZE);
                uintptr_t phys_page = phys_addr + (i * PAGE_SIZE);
                
                paging::map_page(virt_page, phys_page, page_flags, page_table);
            }
            
            SYSCALL_TRACE("0x%llx [MAP_FRAMEBUFFER: mapped %llu bytes at virt=0x%llx, phys=0x%llx]\n", 
                         target_addr, fb_size, target_addr, phys_addr);
            return static_cast<long>(target_addr);
        }
        case GFX_OP_UNMAP_FRAMEBUFFER: {
            SYSCALL_TRACE("0 [UNMAP_FRAMEBUFFER - not implemented yet]\n");
            return 0;  // Success for now
        }
        default: {
            SYSCALL_TRACE("-EINVAL [unknown operation]\n");
            return -EINVAL;
        }
    }
}
