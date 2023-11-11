#include "kheap.h"
#include "kmemory.h"
#include <paging/page_frame_allocator.h>
#include <kprint.h>

#define MIN_HEAP_SEGMENT_CAPACITY 1

#define GET_USABLE_BLOCK_MEMORY_SIZE(seg) seg->size - sizeof(HeapSegmentHeader)

#define WRITE_SEGMENT_MAGIC_FIELD(seg) \
    memcpy(seg->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, sizeof(seg->magic));

DynamicMemoryAllocator g_kernelHeapAllocator;

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
    size_t newSegmentSize = size + sizeof(HeapSegmentHeader);
    HeapSegmentHeader* segment = _findFreeSegment(newSegmentSize);

    if (!_splitSegment(segment, newSegmentSize)) {
        return nullptr;
    }

    // Mark segment as used
    segment->flags.free = false;

    // Return the usable memory after the segment header
    uint8_t* usableRegionStart = reinterpret_cast<uint8_t*>(segment) + sizeof(HeapSegmentHeader);

    return static_cast<void*>(usableRegionStart);
}

void DynamicMemoryAllocator::free(void* ptr) {
    HeapSegmentHeader* segment = reinterpret_cast<HeapSegmentHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(HeapSegmentHeader)
    );

    // Verify the given pointer to be a heap segment header
    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        kuPrint("Invalid pointer provided to free()!\n");
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
}

void* DynamicMemoryAllocator::reallocate(void* ptr, size_t newSize) {
    // If the given pointer is null, just allocate new memory.
    if (ptr == nullptr) {
        return allocate(newSize);
    }

    HeapSegmentHeader* segment = reinterpret_cast<HeapSegmentHeader*>(
        reinterpret_cast<uint8_t*>(ptr) - sizeof(HeapSegmentHeader)
    );

    // Verify the given pointer to be a heap segment header
    if (memcmp(segment->magic, (void*)KERNEL_HEAP_SEGMENT_HDR_SIGNATURE, 7) != 0) {
        kuPrint("Invalid pointer provided to realloc()!\n");
        return nullptr;
    }

    // If the current segment is big enough to hold the new size,
    // potentially resize (shrink) it and return the same pointer.
    if (segment->size >= newSize + sizeof(HeapSegmentHeader)) {
        _splitSegment(segment, newSize + sizeof(HeapSegmentHeader));
        return ptr;
    } else {
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
    uint64_t segId = 1;

    kuPrint("---------------------------------------------\n");
    while (seg) {
        kuPrint("Segment %llu:\n", segId);
        kuPrint("    base         : %llx\n", (uint64_t)seg);
        kuPrint("    total size   : %llx\n", seg->size);
        kuPrint("    usable size  : %llx\n", GET_USABLE_BLOCK_MEMORY_SIZE(seg));
        kuPrint("    status       : %s\n", seg->flags.free ? "free" : "used");
        kuPrint("    next         : %llx\n", (uint64_t)seg->next);
        kuPrint("    prev         : %llx\n\n", (uint64_t)seg->prev);
        
        segId++;
        seg = seg->next;
    }
    kuPrint("---------------------------------------------\n");
}
