#ifndef KHEAP_H
#define KHEAP_H
#include <ktypes.h>

#define KERNEL_HEAP_INIT_SIZE               0x60000000 // 1.5GB Kernel Heap

#define KERNEL_HEAP_SEGMENT_HDR_SIGNATURE   "HEAPHDR"

struct HeapSegmentHeader {
    uint8_t     magic[7];
    
    struct {
        uint8_t free        : 1;
        uint8_t reserved    : 7;
    } flags;

    uint64_t    size;

    HeapSegmentHeader* next;
    HeapSegmentHeader* prev;
} __attribute__((packed, aligned(16)));

class DynamicMemoryAllocator {
public:
    static DynamicMemoryAllocator& get();

    void init(uint64_t base, size_t size);

    inline void* getHeapBase() const { return static_cast<void*>(m_firstSegment); }

    void* allocate(size_t size);
    void free(void* ptr);
    void* reallocate(void* ptr, size_t newSize);

    void __debugHeap();
    void __debugHeapSegment(void* ptr, int64_t segId = -1);
    void __debugUserHeapPointer(void* ptr, int64_t id = -1);

    bool __detectHeapCorruption(bool dbgLog = true);

private:
    uint64_t            m_heapSize;
    HeapSegmentHeader*  m_firstSegment;

private:
    HeapSegmentHeader* _findFreeSegment(size_t minSize);

    //
    // |-----|--------------|     |-----|------------|  |-----|------------|
    // | hdr |              | --> | hdr |            |  | hdr |            |
    // |-----|--------------|     |-----|------------|  |-----|------------|
    //
    // <-------- x --------->     <------ size ------>  <---- x - size ---->
    //
    bool _splitSegment(HeapSegmentHeader* segment, size_t size);

    bool _mergeSegmentWithPrevious(HeapSegmentHeader* segment);
    bool _mergeSegmentWithNext(HeapSegmentHeader* segment);
};

#endif
