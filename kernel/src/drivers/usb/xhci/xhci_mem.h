#ifndef XHCI_MEM_H
#define XHCI_MEM_H

#include "xhci_common.h"

#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>

// Memory Alignment and Boundary Definitions
#define XHCI_DEVICE_CONTEXT_INDEX_MAX_SIZE      2048
#define XHCI_DEVICE_CONTEXT_MAX_SIZE            2048
#define XHCI_INPUT_CONTROL_CONTEXT_MAX_SIZE     64
#define XHCI_SLOT_CONTEXT_MAX_SIZE              64
#define XHCI_ENDPOINT_CONTEXT_MAX_SIZE          64
#define XHCI_STREAM_CONTEXT_MAX_SIZE            16
#define XHCI_STREAM_ARRAY_LINEAR_MAX_SIZE       1024 * 1024 // 1 MB
#define XHCI_STREAM_ARRAY_PRI_SEC_MAX_SIZE      PAGE_SIZE
#define XHCI_TRANSFER_RING_SEGMENTS_MAX_SIZE    1024 * 64   // 64 KB
#define XHCI_COMMAND_RING_SEGMENTS_MAX_SIZE     1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENTS_MAX_SIZE       1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENT_TABLE_MAX_SIZE  1024 * 512  // 512 KB
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_MAX_SIZE   248
#define XHCI_SCRATCHPAD_BUFFERS_MAX_SIZE        PAGE_SIZE

#define XHCI_DEVICE_CONTEXT_INDEX_BOUNDARY      PAGE_SIZE
#define XHCI_DEVICE_CONTEXT_BOUNDARY            PAGE_SIZE
#define XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY     PAGE_SIZE
#define XHCI_SLOT_CONTEXT_BOUNDARY              PAGE_SIZE
#define XHCI_ENDPOINT_CONTEXT_BOUNDARY          PAGE_SIZE
#define XHCI_STREAM_CONTEXT_BOUNDARY            PAGE_SIZE
#define XHCI_STREAM_ARRAY_LINEAR_BOUNDARY       PAGE_SIZE
#define XHCI_STREAM_ARRAY_PRI_SEC_BOUNDARY      PAGE_SIZE
#define XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY    1024 * 64   // 64 KB
#define XHCI_COMMAND_RING_SEGMENTS_BOUNDARY     1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENTS_BOUNDARY       1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY  PAGE_SIZE
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_BOUNDARY   PAGE_SIZE
#define XHCI_SCRATCHPAD_BUFFERS_BOUNDARY        PAGE_SIZE

#define XHCI_DEVICE_CONTEXT_INDEX_ALIGNMENT      64
#define XHCI_DEVICE_CONTEXT_ALIGNMENT            64
#define XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT     64
#define XHCI_SLOT_CONTEXT_ALIGNMENT              32
#define XHCI_ENDPOINT_CONTEXT_ALIGNMENT          32
#define XHCI_STREAM_CONTEXT_ALIGNMENT            16
#define XHCI_STREAM_ARRAY_LINEAR_ALIGNMENT       16
#define XHCI_STREAM_ARRAY_PRI_SEC_ALIGNMENT      16
#define XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT    64
#define XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT     64
#define XHCI_EVENT_RING_SEGMENTS_ALIGNMENT       64
#define XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT  64
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_ALIGNMENT   64
#define XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT        PAGE_SIZE

template <typename T = void>
struct XhciDma {
    T*          virtualBase;
    uint64_t    physicalBase;
};

template <typename T = void>
static XhciDma<T> xhciAllocDma(size_t size, size_t alignment = 64, size_t boundary = PAGE_SIZE) {
    // Allocate extra memory to ensure we can align the block within the boundary
    size_t totalSize = size + boundary - 1;
    void* memblock = kzmallocAligned(totalSize, alignment);

    if (!memblock) {
        while (1); // TO-DO: properly handle the error
        return XhciDma<T> {
            .virtualBase = nullptr,
            .physicalBase = 0
        };
    }

    // Align the memory block to the specified boundary
    size_t alignedAddress = ((size_t)memblock + boundary - 1) & ~(boundary - 1);
    void* aligned = (void*)alignedAddress;

    // Mark the aligned memory block as uncacheable
    paging::markPageUncacheable(aligned);

    return XhciDma<T> {
        .virtualBase = reinterpret_cast<T*>(aligned),
        .physicalBase = reinterpret_cast<uint64_t>(__pa(aligned))
    };
}

uint64_t xhciMapMmio(uint64_t pciBarAddress);

#endif
