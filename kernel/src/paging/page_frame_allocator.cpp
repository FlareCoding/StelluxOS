#include "page_frame_allocator.h"
#include <memory/efimem.h>
#include <memory/phys_addr_translation.h>
#include <kprint.h>

paging::PageFrameAllocator g_globalAllocator;

namespace paging {
    PageFrameAllocator& getGlobalPageFrameAllocator() {
        return g_globalAllocator;
    }

    void PageFrameAllocator::initializeFromMemoryMap(
        void* memoryMap,
        uint64_t memoryDescriptorSize,
        uint64_t memoryDescriptorCount
    ) {
        // void*       largest_free_mem_seg = NULL;
        // uint64_t    largest_free_mem_seg_size = 0;

        for (uint64_t i = 0; i < memoryDescriptorCount; ++i) {
            EFI_MEMORY_DESCRIPTOR* desc =
                (EFI_MEMORY_DESCRIPTOR*)((uint64_t)memoryMap + (i * memoryDescriptorSize));

            // Only track EfiConventionalMemory
            if (desc->type != 7) continue;

            kprintInfo("0x%llx - 0x%llx (%llu pages) [%s]\n",
                (uint64_t)desc->paddr,
                (uint64_t)desc->paddr + desc->pageCount * PAGE_SIZE,
                desc->pageCount,
                EFI_MEMORY_TYPE_STRINGS[desc->type]
            );

            kprintInfo("0x%llx - 0x%llx (%llu pages) [%s]\n\n",
                __va((uint64_t)desc->paddr),
                __va((uint64_t)desc->paddr + desc->pageCount * PAGE_SIZE),
                desc->pageCount,
                EFI_MEMORY_TYPE_STRINGS[desc->type]
            );

            // if (desc->pageCount * PAGE_SIZE > largest_free_mem_seg_size) {
            //     largest_free_mem_seg = desc->paddr;
            //     largest_free_mem_seg_size = desc->pageCount * PAGE_SIZE;
            // }
        }
    }
} // namespace paging
