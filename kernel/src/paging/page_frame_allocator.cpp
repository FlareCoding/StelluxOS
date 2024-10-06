#include "page_frame_allocator.h"
#include <memory/kmemory.h>
#include <memory/efimem.h>
#include <memory/kheap.h>
#include "phys_addr_translation.h"
#include "page.h"
#include "tlb.h"
#include <sync.h>
#include <kprint.h>

paging::PageFrameAllocator g_globalAllocator;

extern uint64_t __ksymstart;

DECLARE_SPINLOCK(__kpage_allocator_lock);
DECLARE_SPINLOCK(__kpage_request_lock);

namespace paging {
    void PageFrameBitmap::initialize(uint64_t size, uint8_t* buffer) {
        m_size = size;
        m_buffer = buffer;

        // Initially mark all pages as used
        memset(buffer, 0xff, size);
    }

    int PageFrameBitmap::markPageFree(void* addr) {
        return _setPageValue(addr, false);
    }

    int PageFrameBitmap::markPageUsed(void* addr) {
        return _setPageValue(addr, true);
    }

    bool PageFrameBitmap::isPageFree(void* addr) {
        return (_getPageValue(addr) == false);
    }

    bool PageFrameBitmap::isPageUsed(void* addr) {
        return (_getPageValue(addr) == true);
    }

    bool PageFrameBitmap::_setPageValue(void* addr, bool value) {
        uint64_t index = _addrToIndex(addr);

        // Preventing bitmap buffer overflow
        if (index > (m_size * 8))
            return false;

        uint64_t byteIdx = index / 8;
        uint8_t bitIdx = index % 8;
        uint8_t mask = 0b00000001 << bitIdx;

        // First disable the bit
        m_buffer[byteIdx] &= ~mask;

        // Now enable the bit if needed
        if (value) {
            m_buffer[byteIdx] |= mask;
        }

        return true;
    }

    bool PageFrameBitmap::_getPageValue(void* addr) {
        uint64_t index = _addrToIndex(addr);
        uint64_t byteIdx = index / 8;
        uint8_t bitIdx = index % 8;
        uint8_t mask = 0b00000001 << bitIdx;

        return (m_buffer[byteIdx] & mask) > 0;
    }

    uint64_t PageFrameBitmap::_addrToIndex(void* addr) {
        return reinterpret_cast<uint64_t>(addr) / PAGE_SIZE;
    }

    PageFrameAllocator& getGlobalPageFrameAllocator() {
        return g_globalAllocator;
    }

    __PRIVILEGED_CODE
    void PageFrameAllocator::initializeFromMemoryMap(
        void* memoryMap,
        uint64_t memoryDescriptorSize,
        uint64_t memoryDescriptorCount
    ) {
        auto& heapAllocator = DynamicMemoryAllocator::get();

        void*       largestFreeMemorySegment = nullptr;
        uint64_t    largestFreeMemorySegmentSize = 0;

        m_freeSystemMemory = 0;

        for (uint64_t i = 0; i < memoryDescriptorCount; ++i) {
            EFI_MEMORY_DESCRIPTOR* desc =
                (EFI_MEMORY_DESCRIPTOR*)((uint64_t)memoryMap + (i * memoryDescriptorSize));

            m_totalSystemMemory += desc->pageCount * PAGE_SIZE;

            // Only track EfiConventionalMemory
            if (desc->type != 7)
                continue;

            m_freeSystemMemory += desc->pageCount * PAGE_SIZE;

            // kprintInfo("0x%llx - 0x%llx (%llu pages) [%s]\n",
            //     (uint64_t)desc->paddr,
            //     (uint64_t)desc->paddr + desc->pageCount * PAGE_SIZE,
            //     desc->pageCount,
            //     EFI_MEMORY_TYPE_STRINGS[desc->type]
            // );

            // kprintInfo("0x%llx - 0x%llx (%llu pages) [%s]\n\n",
            //     __va(desc->paddr),
            //     __va((uint8_t*)desc->paddr + desc->pageCount * PAGE_SIZE),
            //     desc->pageCount,
            //     EFI_MEMORY_TYPE_STRINGS[desc->type]
            // );

            if (desc->pageCount * PAGE_SIZE > largestFreeMemorySegmentSize) {
                uint64_t virtualBase = (uint64_t)__va(desc->paddr);
                uint64_t virtualRegionEnd = virtualBase + desc->pageCount * PAGE_SIZE;

                if (virtualBase >= 0xffff000000000000 && virtualRegionEnd <= 0xffffffffffffffff) {
                    largestFreeMemorySegment = desc->paddr;
                    largestFreeMemorySegmentSize = desc->pageCount * PAGE_SIZE;
                }
            }
        }

        // Calculate the page frame bitmap size
        uint64_t pageBitmapSize = m_totalSystemMemory / PAGE_SIZE / 8 + 1;

        // Set the page frame bitmap base by default
        // to the beginning of the largest free segment.
        uint8_t* pageBitmapBase = static_cast<uint8_t*>(largestFreeMemorySegment);
        uint8_t* pageBitmapVirtualBase = (uint8_t*)__va(pageBitmapBase);

        // Check to make sure if the virtual page frame bitmap base is at a userspace
        // kernel address because otherwise the base would have to be recalculated.
        if (((uint64_t)pageBitmapVirtualBase < 0xffff000000000000) || pageBitmapVirtualBase == 0) {
            for (uint64_t i = 0; i < memoryDescriptorCount; ++i) {
                EFI_MEMORY_DESCRIPTOR* desc =
                    (EFI_MEMORY_DESCRIPTOR*)((uint64_t)memoryMap + (i * memoryDescriptorSize));

                // Only track EfiConventionalMemory
                if (desc->type != 7)
                    continue;

                if (desc->pageCount * PAGE_SIZE >= pageBitmapSize) {
                    pageBitmapBase = static_cast<uint8_t*>(desc->paddr);
                    pageBitmapVirtualBase = (uint8_t*)__va(pageBitmapBase);
                    break;
                }
            }
        }

        m_pageFrameBitmap.initialize(pageBitmapSize, pageBitmapVirtualBase);

        // Get the address of PML4 table from cr3
        auto pml4 = getCurrentTopLevelPageTable();

        // Mark all EfiConventionalMemory pages as free
        for (uint64_t i = 0; i < memoryDescriptorCount; ++i) {
            EFI_MEMORY_DESCRIPTOR* desc =
                (EFI_MEMORY_DESCRIPTOR*)((uint64_t)memoryMap + (i * memoryDescriptorSize));

            // Only track EfiConventionalMemory
            if (desc->type != 7)
                continue;

            uint8_t* frameStart = reinterpret_cast<uint8_t*>(desc->paddr);
            uint8_t* frameEnd = reinterpret_cast<uint8_t*>(desc->paddr) + desc->pageCount * PAGE_SIZE;

            for (uint8_t* freePagePtr = frameStart; freePagePtr < frameEnd; freePagePtr += PAGE_SIZE) {
                m_pageFrameBitmap.markPageFree(freePagePtr);

                // Mark free page's higher half virtual address with usermode permissions
                auto pte = getPteForAddr(__va(freePagePtr), pml4);
                pte->userSupervisor = USERSPACE_PAGE;
            }
        }

        // Initialize the kernel heap
        uint64_t kernelHeapBase = reinterpret_cast<uint64_t>(pageBitmapVirtualBase) + pageBitmapSize;
        heapAllocator.init(kernelHeapBase, KERNEL_HEAP_INIT_SIZE);

        // Mark higher-half pages where bitmap lives as accessible to usermode code
        for (uint8_t* bitmapPage = pageBitmapVirtualBase; bitmapPage < pageBitmapVirtualBase + pageBitmapSize; bitmapPage += PAGE_SIZE) {
            // Mark free page with usermode permissions
            auto pte = getPteForAddr(bitmapPage, pml4);
            pte->userSupervisor = USERSPACE_PAGE;
        }

        // Flush the TLB to activate new usermode permissions
        flushTlbAll();

        // Identify the first usable page index
        for (
            uint8_t* page = NULL;
            page < reinterpret_cast<uint8_t*>(m_totalSystemMemory);
            page += PAGE_SIZE
        ) {
            if (m_pageFrameBitmap.isPageFree(page)) {
                m_lastTrackedFreePage = page;
                break;
            }
        }

        m_usedSystemMemory = m_totalSystemMemory - m_freeSystemMemory;

        // Lock the pages used for the bitmap
        lockPhysicalPages(pageBitmapBase, pageBitmapSize / PAGE_SIZE + 1);
        lockPages(pageBitmapVirtualBase, pageBitmapSize / PAGE_SIZE + 1);

        // These pages will later be used for AP startup code
        lockPhysicalPages((void*)0x6000, 20);
    }

    void PageFrameAllocator::freePhysicalPage(void* paddr) {
        acquireSpinlock(&__kpage_allocator_lock);

        // Check if the page is already free
        if (m_pageFrameBitmap.isPageFree(paddr)) {
            releaseSpinlock(&__kpage_allocator_lock);
            return;
        }

        if (m_pageFrameBitmap.markPageFree(paddr)) {
            m_freeSystemMemory += PAGE_SIZE;
            m_usedSystemMemory -= PAGE_SIZE;
        }

        releaseSpinlock(&__kpage_allocator_lock);
    }

    void PageFrameAllocator::freePhysicalPages(void* paddr, uint64_t pages) {
        for (uint64_t i = 0; i < pages; ++i) {
            void* page = (void*)((uint64_t)paddr + (i * PAGE_SIZE));
            freePhysicalPage(page);
        }
    }

    void PageFrameAllocator::lockPhysicalPage(void* paddr) {
        acquireSpinlock(&__kpage_allocator_lock);

        // Check if the page is already in use
        if (m_pageFrameBitmap.isPageUsed(paddr)) {
            releaseSpinlock(&__kpage_allocator_lock);
            return;
        }

        if (m_pageFrameBitmap.markPageUsed(paddr)) {
            m_freeSystemMemory -= PAGE_SIZE;
            m_usedSystemMemory += PAGE_SIZE;
        }

        releaseSpinlock(&__kpage_allocator_lock);
    }

    void PageFrameAllocator::lockPhysicalPages(void* paddr, uint64_t pages) {
        for (uint64_t i = 0; i < pages; ++i) {
            void* page = (void*)((uint64_t)paddr + (i * PAGE_SIZE));
            lockPhysicalPage(page);
        }
    }

    void PageFrameAllocator::freePage(void* vaddr) {
        acquireSpinlock(&__kpage_allocator_lock);

        void* paddr = __pa(vaddr);

        // Check if the page is already free
        if (m_pageFrameBitmap.isPageFree(paddr)) {
            releaseSpinlock(&__kpage_allocator_lock);
            return;
        }

        if (m_pageFrameBitmap.markPageFree(paddr)) {
            m_freeSystemMemory += PAGE_SIZE;
            m_usedSystemMemory -= PAGE_SIZE;
        }

        releaseSpinlock(&__kpage_allocator_lock);
    }

    void PageFrameAllocator::freePages(void* vaddr, uint64_t pages) {
        for (uint64_t i = 0; i < pages; ++i) {
            void* page = (void*)((uint64_t)vaddr + (i * PAGE_SIZE));
            freePage(page);
        }
    }

    void PageFrameAllocator::lockPage(void* vaddr) {
        acquireSpinlock(&__kpage_allocator_lock);

        void* paddr = __pa(vaddr);

        // Check if the page is already in use
        if (m_pageFrameBitmap.isPageUsed(paddr)) {
            releaseSpinlock(&__kpage_allocator_lock);
            return;
        }

        if (m_pageFrameBitmap.markPageUsed(paddr)) {
            m_freeSystemMemory -= PAGE_SIZE;
            m_usedSystemMemory += PAGE_SIZE;
        }

        releaseSpinlock(&__kpage_allocator_lock);
    }

    void PageFrameAllocator::lockPages(void* vaddr, uint64_t pages) {
        for (uint64_t i = 0; i < pages; ++i) {
            void* page = (void*)((uint64_t)vaddr + (i * PAGE_SIZE));
            lockPage(page);
        }
    }

    bool PageFrameAllocator::isPageLocked(void* vaddr) {
        bool ret = false;
        uint64_t pageAddress = ((uint64_t)vaddr) & ~(PAGE_SIZE - 1);

        acquireSpinlock(&__kpage_allocator_lock);
        void* paddr = __pa((void*)pageAddress);

        ret = m_pageFrameBitmap.isPageUsed(paddr);

        releaseSpinlock(&__kpage_allocator_lock);

        return ret;
    }

    bool PageFrameAllocator::isPhysicalPageLocked(void* paddr) {
        bool ret = false;
        uint64_t pageAddress = ((uint64_t)paddr) & ~(PAGE_SIZE - 1);

        acquireSpinlock(&__kpage_allocator_lock);
        ret = m_pageFrameBitmap.isPageUsed((void*)pageAddress);

        releaseSpinlock(&__kpage_allocator_lock);

        return ret;
    }

    void* PageFrameAllocator::requestFreePage() {
        acquireSpinlock(&__kpage_request_lock);

        for (
            uint8_t* page = reinterpret_cast<uint8_t*>(m_lastTrackedFreePage);
            page < reinterpret_cast<uint8_t*>(m_totalSystemMemory);
            page += PAGE_SIZE
        ) {
            // Skip pages that are already in use
            if (m_pageFrameBitmap.isPageUsed(page)) {
                continue;
            }

            lockPhysicalPage(page);

            m_lastTrackedFreePage = page + PAGE_SIZE;
            releaseSpinlock(&__kpage_request_lock);
            return __va(page);
        }

        releaseSpinlock(&__kpage_request_lock);

        // If there are no more pages in RAM to give out,
        // a disk page frame swap is required to request more pages,
        // but Stellux kernel doesn't support it yet.
        kprintError("Out of RAM! Disk page frame swap is not yet implemented\n");
        return NULL;
    }

    void* PageFrameAllocator::requestFreePageZeroed() {
        void* page = requestFreePage();
        zeromem(page, PAGE_SIZE);

        return page;
    }

    void* PageFrameAllocator::requestFreePages(size_t pages) {
        acquireSpinlock(&__kpage_request_lock);

        for (
            uint8_t* page = reinterpret_cast<uint8_t*>(m_lastTrackedFreePage);
            page < reinterpret_cast<uint8_t*>(m_totalSystemMemory);
            page += PAGE_SIZE
        ) {
            bool freeContiguousBlockFound = true;

            // Skip contiguous page blocks that are already in use
            for (uint8_t* pageBlockPtr = page; pageBlockPtr < (page + PAGE_SIZE * pages); pageBlockPtr += PAGE_SIZE) {
                if (m_pageFrameBitmap.isPageUsed(pageBlockPtr)) {
                    freeContiguousBlockFound = false;
                    break;
                }
            }

            // Skip this page if the free contiguous block of memory wasn't found
            if (!freeContiguousBlockFound) {
                continue;
            }

            // Lock all pages in the contiguous region
            for (uint8_t* pageBlockPtr = page; pageBlockPtr < (page + PAGE_SIZE * pages); pageBlockPtr += PAGE_SIZE) {
                lockPhysicalPage(pageBlockPtr);
            }

            // Check if we skipped over a free page in search for a
            // contiguous block and assign the last tracked free page to it.
            for (
                uint8_t* pg = reinterpret_cast<uint8_t*>(m_lastTrackedFreePage);
                pg < reinterpret_cast<uint8_t*>(m_totalSystemMemory);
                pg += PAGE_SIZE
            ) {
                if (m_pageFrameBitmap.isPageFree(pg)) {
                    m_lastTrackedFreePage = pg;
                    break;
                }
            }

            releaseSpinlock(&__kpage_request_lock);
            return __va(page);
        }

        releaseSpinlock(&__kpage_request_lock);

        // If there are no more pages in RAM to give out,
        // a disk page frame swap is required to request more pages,
        // but Stellux kernel doesn't support it yet.
        kprintError("Out of RAM! Disk page frame swap is not yet implemented\n");
        return nullptr;
    }

    void* PageFrameAllocator::requestFreePagesZeroed(size_t pages) {
        void* page = requestFreePages(pages);
        if (!page) {
            return nullptr;
        }

        zeromem(page, PAGE_SIZE * pages);

        return page;
    }
} // namespace paging
