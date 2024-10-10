#include "kheap.h"
#include "kmemory.h"
#include <paging/page_frame_allocator.h>
#include <sync.h>
#include <kprint.h>

#define MIN_HEAP_SEGMENT_CAPACITY 1

#define GET_USABLE_BLOCK_MEMORY_SIZE(seg) seg->size - sizeof(HeapSegmentHeader)

#define WRITE_SEGMENT_MAGIC_FIELD(seg) \
    memcpy(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic));

DynamicMemoryAllocator g_kernelHeapAllocator;

DECLARE_SPINLOCK(__kheap_lock);

DynamicMemoryAllocator& DynamicMemoryAllocator::get() {
    return g_kernelHeapAllocator;
}

void DynamicMemoryAllocator::init(uint64_t base, size_t size) {
    m_heapSize = size;

    m_firstSegment = reinterpret_cast<HeapSegmentHeader*>(base);

    // Lock the pages pertaining to the heap
    paging::getGlobalPageFrameAllocator().lockPages(reinterpret_cast<void*>(base), size / PAGE_SIZE);

    // Setup the root segment
    WRITE_SEGMENT_MAGIC_FIELD(m_firstSegment);
    m_firstSegment->flags = {
        .free = true,
        .reserved = 0
    };
    m_firstSegment->size = size;
    m_firstSegment->next = nullptr;
    m_firstSegment->prev = nullptr;
}

void* DynamicMemoryAllocator::allocate(size_t size) {
    acquireSpinlock(&__kheap_lock);

    size_t newSegmentSize = size + sizeof(HeapSegmentHeader);

    // + sizeof(HeapSegmentHeader) is to account for the splitting
    HeapSegmentHeader* segment = _findFreeSegment(newSegmentSize + sizeof(HeapSegmentHeader));

    if (!segment) {
        releaseSpinlock(&__kheap_lock);
        return nullptr;
    }

    // Attempt to split the segment if it's large enough
    _splitSegment(segment, newSegmentSize);

    // Mark segment as used
    segment->flags.free = false;

    // Return the usable memory after the segment header
    uint8_t* usableRegionStart = reinterpret_cast<uint8_t*>(segment) + sizeof(HeapSegmentHeader);

    releaseSpinlock(&__kheap_lock);
    return static_cast<void*>(usableRegionStart);
}

void DynamicMemoryAllocator::free(void* ptr) {
    acquireSpinlock(&__kheap_lock);

    HeapSegmentHeader* segment = reinterpret_cast<HeapSegmentHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(HeapSegmentHeader)
    );

    // Verify the given pointer to be a heap segment header
    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        kprintf("Invalid pointer provided to free()!\n");
        releaseSpinlock(&__kheap_lock);
        return;
    }

    segment->flags.free = true;

    // Merging with the next segment has to come first
    // to preserve correct base-relative calculations.
    if (segment->next && segment->next->flags.free) {
        _mergeSegmentWithNext(segment);
    }

    // Check if merging with the previous segment is possible
    if (segment->prev && segment->prev->flags.free) {
        _mergeSegmentWithPrevious(segment);
    }

    releaseSpinlock(&__kheap_lock);
}

void* DynamicMemoryAllocator::reallocate(void* ptr, size_t newSize) {
    // If the given pointer is null, just allocate new memory.
    if (ptr == nullptr) {
        return allocate(newSize);
    }

    acquireSpinlock(&__kheap_lock);

    HeapSegmentHeader* segment = reinterpret_cast<HeapSegmentHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(HeapSegmentHeader)
    );

    // Verify the given pointer to be a heap segment header
    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        kprintf("Invalid pointer provided to realloc()!\n");
        releaseSpinlock(&__kheap_lock);
        return nullptr;
    }

    // If the current segment is big enough to hold the new size,
    // potentially resize (shrink) it and return the same pointer.
    if (segment->size >= newSize + sizeof(HeapSegmentHeader)) {
        _splitSegment(segment, newSize + sizeof(HeapSegmentHeader));
        releaseSpinlock(&__kheap_lock);
        return ptr;
    } else {
        // Release the currently held lock because further `allocate`
        // and `free` calls will attempt to acquire the lock themselves.
        releaseSpinlock(&__kheap_lock);

        // Allocate new memory, and check if allocation was successful
        void* newPtr = allocate(newSize);
        if (!newPtr) {
            return nullptr;
        }

        // Copy the old data to the new location
        memcpy(newPtr, ptr, segment->size - sizeof(HeapSegmentHeader));

        // Free the old segment
        free(ptr);

        return newPtr;
    }

    // Should never get here
    return nullptr;
}

HeapSegmentHeader* DynamicMemoryAllocator::_findFreeSegment(size_t minSize) {
    HeapSegmentHeader* seg = m_firstSegment;

    while (seg) {
        if (seg->flags.free && (seg->size >= minSize)) {
            return seg;
        }

        seg = seg->next;
    }

    return nullptr;
}

bool DynamicMemoryAllocator::_splitSegment(HeapSegmentHeader* segment, size_t size) {
    // Check if two sub-segments can be formed from the given segment
    if (static_cast<int64_t>(segment->size - (size + sizeof(HeapSegmentHeader))) < MIN_HEAP_SEGMENT_CAPACITY * 2) {
        return false;
    }

    HeapSegmentHeader* newSegment = reinterpret_cast<HeapSegmentHeader*>(
        reinterpret_cast<uint8_t*>(segment) + size
    );

    // Setup the new segment
    WRITE_SEGMENT_MAGIC_FIELD(newSegment)
    newSegment->flags.free = segment->flags.free;
    newSegment->size = segment->size - size;
    newSegment->next = segment->next;
    newSegment->prev = segment;

    // Adjust the existing segment
    segment->size = size;
    segment->next = newSegment;

    return true;
}

bool DynamicMemoryAllocator::_mergeSegmentWithPrevious(HeapSegmentHeader* segment) {
    HeapSegmentHeader* previousSegment = segment->prev;

    if (previousSegment == nullptr) {
        return false;
    }

    // When merging with a previous segment,
    // this segment essentially ceases to exist.
    previousSegment->size += segment->size;
    previousSegment->next = segment->next;
    
    // Adjust the next segment's "previous"
    // pointer to point to the previous segment.
    if (previousSegment->next) {
        previousSegment->next->prev = previousSegment;
    }

    return true;
}

bool DynamicMemoryAllocator::_mergeSegmentWithNext(HeapSegmentHeader* segment) {
    HeapSegmentHeader* nextSegment = segment->next;

    if (nextSegment == nullptr) {
        return false;
    }

    // When merging with a next segment, the
    // next segment essentially ceases to exist.
    segment->size += nextSegment->size;
    segment->next = nextSegment->next;
    
    // Adjust the futher next segment's "previous"
    // pointer to point to the current segment.
    if (segment->next) {
        segment->next->prev = segment;
    }

    return true;
}

void DynamicMemoryAllocator::__debugHeap() {
    HeapSegmentHeader* seg = m_firstSegment;
    int64_t segId = 1;

    kprintf("---------------------------------------------\n");
    while (seg) {
        kprintf("Segment %llu:\n", segId);
        kprintf("    base         : %llx\n", (uint64_t)seg);
        kprintf("    userptr      : %llx\n", (uint64_t)seg + sizeof(HeapSegmentHeader));
        kprintf("    total size   : %llx\n", seg->size);
        kprintf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
        kprintf("    status       : %s\n", seg->flags.free ? "free" : "used");
        kprintf("    next         : %llx\n", (uint64_t)seg->next);
        kprintf("    prev         : %llx\n\n", (uint64_t)seg->prev);
        
        segId++;
        seg = seg->next;
    }
    kprintf("---------------------------------------------\n");
}

void DynamicMemoryAllocator::__debugHeapSegment(void* ptr, int64_t segId) {
    HeapSegmentHeader* seg = (HeapSegmentHeader*)ptr;
    
    if (segId != -1)
        kprintf("Segment %llu:\n", segId);
    else
        kprintf("Segment\n");

    kprintf("    base         : %llx\n", (uint64_t)seg);
    kprintf("    userptr      : %llx\n", (uint64_t)seg + sizeof(HeapSegmentHeader));
    kprintf("    total size   : %llx\n", seg->size);
    kprintf("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
    kprintf("    status       : %s\n", seg->flags.free ? "free" : "used");
    kprintf("    next         : %llx\n", (uint64_t)seg->next);
    kprintf("    prev         : %llx\n\n", (uint64_t)seg->prev);
}

void DynamicMemoryAllocator::__debugUserHeapPointer(void* ptr, int64_t id) {
    void* seg = (void*)((uint64_t)ptr - sizeof(HeapSegmentHeader));
    __debugHeapSegment(seg, id);
}

bool DynamicMemoryAllocator::__detectHeapCorruption(bool dbgLog) {
    HeapSegmentHeader* seg = m_firstSegment;
    int64_t segId = 1;

    while (seg) {
        if (memcmp(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic)) != 0) {
            if (dbgLog) {
                kprintf("---- Detected Heap Corruption (segment %lli) ----\n", segId);
                __debugHeapSegment(seg, segId);
            }

            return true;
        }
        
        segId++;
        seg = seg->next;
    }

    if (dbgLog) {
        kprintf("---- No Heap Corruption Detected (checked %lli segments) ----\n", segId - 1);
    }

    return false;
}
