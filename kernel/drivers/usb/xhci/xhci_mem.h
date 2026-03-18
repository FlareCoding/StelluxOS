#ifndef XHCI_MEM_H
#define XHCI_MEM_H

#include "mm/paging_types.h"

namespace drivers::xhci {

// Memory alignment, boundary, and max-size limits
constexpr size_t XHCI_DEVICE_CONTEXT_INDEX_MAX_SIZE     = 2048;
constexpr size_t XHCI_DEVICE_CONTEXT_MAX_SIZE           = 2048;
constexpr size_t XHCI_INPUT_CONTROL_CONTEXT_MAX_SIZE    = 64;
constexpr size_t XHCI_SLOT_CONTEXT_MAX_SIZE             = 64;
constexpr size_t XHCI_ENDPOINT_CONTEXT_MAX_SIZE         = 64;
constexpr size_t XHCI_STREAM_CONTEXT_MAX_SIZE           = 16;
constexpr size_t XHCI_STREAM_ARRAY_LINEAR_MAX_SIZE      = 1024 * 1024;  // 1 MB
constexpr size_t XHCI_STREAM_ARRAY_PRI_SEC_MAX_SIZE     = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_TRANSFER_RING_SEGMENTS_MAX_SIZE   = 1024 * 64;    // 64 KB
constexpr size_t XHCI_COMMAND_RING_SEGMENTS_MAX_SIZE    = 1024 * 64;    // 64 KB
constexpr size_t XHCI_EVENT_RING_SEGMENTS_MAX_SIZE      = 1024 * 64;    // 64 KB
constexpr size_t XHCI_EVENT_RING_SEGMENT_TABLE_MAX_SIZE = 1024 * 512;  // 512 KB
constexpr size_t XHCI_SCRATCHPAD_BUFFER_ARRAY_MAX_SIZE  = 248;
constexpr size_t XHCI_SCRATCHPAD_BUFFERS_MAX_SIZE       = paging::PAGE_SIZE_4KB;

constexpr size_t XHCI_DEVICE_CONTEXT_INDEX_BOUNDARY     = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_DEVICE_CONTEXT_BOUNDARY           = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY    = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_SLOT_CONTEXT_BOUNDARY             = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_ENDPOINT_CONTEXT_BOUNDARY         = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_STREAM_CONTEXT_BOUNDARY           = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_STREAM_ARRAY_LINEAR_BOUNDARY      = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_STREAM_ARRAY_PRI_SEC_BOUNDARY     = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY   = 1024 * 64;    // 64 KB
constexpr size_t XHCI_COMMAND_RING_SEGMENTS_BOUNDARY    = 1024 * 64;    // 64 KB
constexpr size_t XHCI_EVENT_RING_SEGMENTS_BOUNDARY      = 1024 * 64;    // 64 KB
constexpr size_t XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_SCRATCHPAD_BUFFER_ARRAY_BOUNDARY  = paging::PAGE_SIZE_4KB;
constexpr size_t XHCI_SCRATCHPAD_BUFFERS_BOUNDARY       = paging::PAGE_SIZE_4KB;

constexpr size_t XHCI_DEVICE_CONTEXT_INDEX_ALIGNMENT     = 64;
constexpr size_t XHCI_DEVICE_CONTEXT_ALIGNMENT           = 64;
constexpr size_t XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT    = 64;
constexpr size_t XHCI_SLOT_CONTEXT_ALIGNMENT             = 32;
constexpr size_t XHCI_ENDPOINT_CONTEXT_ALIGNMENT         = 32;
constexpr size_t XHCI_STREAM_CONTEXT_ALIGNMENT           = 16;
constexpr size_t XHCI_STREAM_ARRAY_LINEAR_ALIGNMENT      = 16;
constexpr size_t XHCI_STREAM_ARRAY_PRI_SEC_ALIGNMENT     = 16;
constexpr size_t XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT   = 64;
constexpr size_t XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT    = 64;
constexpr size_t XHCI_EVENT_RING_SEGMENTS_ALIGNMENT      = 64;
constexpr size_t XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT = 64;
constexpr size_t XHCI_SCRATCHPAD_BUFFER_ARRAY_ALIGNMENT  = 64;
constexpr size_t XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT       = paging::PAGE_SIZE_4KB;

void* alloc_xhci_memory(size_t size);

void free_xhci_memory(void* ptr);

uintptr_t xhci_get_physical_addr(void* vaddr);

} // namespace drivers::xhci

#endif // XHCI_MEM_H
