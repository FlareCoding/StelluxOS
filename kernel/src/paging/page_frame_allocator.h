#ifndef PAGE_FRAME_ALLOCATOR_H
#define PAGE_FRAME_ALLOCATOR_H
#include <ktypes.h>

#define PAGE_SIZE 0x1000

namespace paging {
class PageFrameAllocator {
public:
    void initializeFromMemoryMap(
        void* memoryMap,
        uint64_t memoryDescriptorSize,
        uint64_t memoryDescriptorCount
    );
};

EXTERN_C PageFrameAllocator& getGlobalPageFrameAllocator();
} // namespace paging
#endif
