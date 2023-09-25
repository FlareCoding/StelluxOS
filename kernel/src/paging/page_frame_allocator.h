#ifndef PAGE_FRAME_ALLOCATOR_H
#define PAGE_FRAME_ALLOCATOR_H
#include <ktypes.h>

#define PAGE_SIZE 0x1000

namespace paging {
// PageFrameBitmap expects physical page addresses
class PageFrameBitmap {
public:
    PageFrameBitmap() = default;
    void initialize(uint64_t size, uint8_t* buffer);

    inline uint64_t getSize() const { return m_size; }

    int markPageFree(void* addr);
    int markPageUsed(void* addr);

    bool isPageFree(void* addr);
    bool isPageUsed(void* addr);

private:
    bool _setPageValue(void* addr, bool value);
    bool _getPageValue(void* addr);

    uint64_t _addrToIndex(void* addr);

    uint64_t m_size;
    uint8_t* m_buffer;
};

class PageFrameAllocator {
public:
    void initializeFromMemoryMap(
        void* memoryMap,
        uint64_t memoryDescriptorSize,
        uint64_t memoryDescriptorCount
    );

    void freePhysicalPage(void* paddr);
    void freePhysicalPages(void* paddr, uint64_t pages);

    void lockPhysicalPage(void* paddr);
    void lockPhysicalPages(void* paddr, uint64_t pages);

    void freePage(void* vaddr);
    void freePages(void* vaddr, uint64_t pages);

    void lockPage(void* vaddr);
    void lockPages(void* vaddr, uint64_t pages);

    void* requestFreePage();
    void* requestFreePageZeroed();

    inline uint64_t getTotalSystemMemory() const { return m_totalSystemMemory; }
    inline uint64_t getFreeSystemMemory() const { return m_freeSystemMemory; }
    inline uint64_t getUsedSystemMemory() const { return m_usedSystemMemory; }

private:
    uint64_t m_totalSystemMemory = 0;
    uint64_t m_freeSystemMemory = 0;
    uint64_t m_usedSystemMemory = 0;

    // Pointer to the last available free
    // page that is currently in the page bitmap.
    void*    m_lastTrackedFreePage = nullptr;

    PageFrameBitmap m_pageFrameBitmap;
};

EXTERN_C PageFrameAllocator& getGlobalPageFrameAllocator();
} // namespace paging
#endif
