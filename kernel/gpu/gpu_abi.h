#ifndef STELLUX_GPU_GPU_ABI_H
#define STELLUX_GPU_GPU_ABI_H

/*
 * Each GPU's device file, /dev/gpuN, takes the ioctl calls this header defines. The kernel and userland, which includes
 * the copy libstlx installs as <stlx/gpu_abi.h>, take the fixed-width integer types from different headers, so the
 * header includes nothing and an includer must provide those types first.
 *
 * A context is a GPU address space with queues that run its command lists, a buffer is GPU memory, and a timeline is a
 * 64-bit counter that only rises. A point (a timeline and a value) is reached once the timeline's value is at least the
 * point's. A process names contexts, buffers and timelines by resource handles, which it can send to other processes
 * over unix sockets with their rights. None has a destroy call: each lives until its last handle closes and, for a
 * buffer, its last CPU and GPU mapping is gone and the work running at its unmaps finished, and, for a timeline, every
 * wait and submission naming it ended. Once a context's last handle closed and every destroyed queue freed its index,
 * its held work (not started) fails and its queues stop, failing the work running at the stop. A `GPU_BUFFER_MAPPABLE`
 * buffer maps through a shared mmap of a handle with `GPU_RIGHT_READ`, writable only with `GPU_RIGHT_WRITE` too.
 *
 * Each call takes the address of its argument structure and fails with
 * - EINVAL for an argument the kernel cannot read or write, a field outside what its comment allows (an unknown flag
 *   and a nonzero reserved or padding field included) or a context or buffer of another device than the file's,
 * - EBADF for a handle that names no object of the kind the call needs or lacks the right its use needs,
 * - ENOMEM when the kernel or the GPU has no memory left for the call, a full handle table included,
 * - EIO when the GPU fails or a queue create, map, submit or GPU_PLACEMENT_CONTEXT create names a lost context,
 * - ENOTTY for a call the device does not offer, `GPU_READ_TIMESTAMP` on a GPU without a clock included.
 *
 * An mmap of a buffer handle fails with EACCES for a protection the handle's rights do not allow, with EINVAL for a
 * buffer without `GPU_BUFFER_MAPPABLE` or a range past its end, with ENODEV for a private mapping and with ENOMEM when
 * the kernel has no memory left or the GPU failed.
 */

#ifdef __cplusplus
#define GPU_ABI_ASSERT(x, m) static_assert(x, m)
#else
#define GPU_ABI_ASSERT(x, m) _Static_assert(x, m)
#endif

#define GPU_GET_INFO        0x4800u
#define GPU_GET_MEMORY      0x4801u
#define GPU_READ_TIMESTAMP  0x4802u
#define GPU_CONTEXT_CREATE  0x4803u
#define GPU_CONTEXT_QUERY   0x4804u
#define GPU_QUEUE_CREATE    0x4805u
#define GPU_QUEUE_DESTROY   0x4806u
#define GPU_BUFFER_CREATE   0x4807u
#define GPU_BUFFER_IMPORT   0x4808u
#define GPU_MAP             0x4809u
#define GPU_UNMAP           0x480Au
#define GPU_TIMELINE_CREATE 0x480Bu
#define GPU_TIMELINE_SIGNAL 0x480Cu
#define GPU_TIMELINE_QUERY  0x480Du
#define GPU_TIMELINE_WAIT   0x480Eu
#define GPU_SUBMIT          0x480Fu
#define GPU_HANDLE_RESTRICT 0x4810u

// A handle's rights, the values of RIGHT_READ and RIGHT_WRITE in kernel/resource/resource_types.h
#define GPU_RIGHT_READ  0x1u
#define GPU_RIGHT_WRITE 0x2u

#define GPU_BACKEND_NVIDIA 1u
#define GPU_BACKEND_VIRTIO 2u
#define GPU_BACKEND_FAKE   3u // kernel tests only

#define GPU_QUEUE_GRAPHICS 1u // 3D, compute and copy commands
#define GPU_QUEUE_COPY     2u // copy commands

#define GPU_PLACEMENT_VIDEO   1u // the GPU's own memory
#define GPU_PLACEMENT_SYSTEM  2u // system memory the CPU caches and the GPU reads coherently
#define GPU_PLACEMENT_CONTEXT 3u // memory a context's commands allocated, where the host allocates it (virtio-gpu)

#define GPU_BUFFER_MAPPABLE 0x1u // the CPU may map it
#define GPU_BUFFER_SCANOUT  0x2u // placed where the display can scan it out, in video memory only

#define GPU_MAP_READ_ONLY 0x1u

// NVIDIA range flag: earlier work on the queue writes the range, so the GPU fetches it only once the commands before
// it were processed. Their engine work may still run then, so the work writing it must be followed by a wait for idle
#define GPU_COMMANDS_GPU_WRITTEN 0x1u

#define GPU_WAIT_ALL 0x1u // wait until every point is reached instead of any one

#define GPU_TIMELINE_FAILED 0x1u // a submission that would reach one of its points failed

// Why a context was lost, 0 while it is live
#define GPU_LOST_FAULT  1u // its work reached memory not mapped for it
#define GPU_LOST_ENGINE 2u // an engine reported an error in its work
#define GPU_LOST_HUNG   3u // a submission ran past GPU_SUBMIT_TIMEOUT_NS
#define GPU_LOST_DEVICE 4u // the GPU failed

#define GPU_SUBMIT_TIMEOUT_NS 7000000000ull

#define GPU_QUEUES_MAX         8u     // queues one context holds at once
#define GPU_QUEUE_PENDING_MAX  64u    // submissions held and running on one queue at once
#define GPU_POINTS_MAX         32u    // points one GPU_TIMELINE_WAIT names, and waits or signals one submission names
#define GPU_COMMANDS_BYTES_MAX 65536u // the largest command list

// NVIDIA facts NVK needs, zero on other backends
struct gpu_nvidia_info {
    uint32_t chipset;       // the chip's number as NVK numbers chips, such as 0x172
    uint16_t class_gpfifo;  // channel class
    uint16_t class_copy;    // copy engine class
    uint16_t class_2d;
    uint16_t class_3d;
    uint16_t class_m2mf;    // inline-to-memory class
    uint16_t class_compute;
    uint16_t gpc_count;
    uint16_t tpc_count;     // across every GPC
    uint32_t padding;
};

// virtio-gpu facts venus needs, zero on other backends
struct gpu_virtio_info {
    uint64_t capsets;           // a bit per capset id the device offers
    uint64_t host_visible_size; // bytes of the device's host-visible memory region, 0 without one
};

// GPU_GET_INFO argument, out: what the GPU is, which never changes while it is registered
struct gpu_info {
    uint32_t backend;             // GPU_BACKEND_*
    uint32_t queue_kinds;         // a bit (1 << kind) per GPU_QUEUE_* kind the device offers
    uint16_t vendor_id;           // PCI identity, zero where the device has none
    uint16_t device_id;           // the PCI device id
    uint16_t pci_segment;
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_function;
    uint8_t  pci_revision;
    uint16_t padding0[3];
    char     name[64];            // the product name, NUL terminated
    uint64_t video_size;          // bytes of video memory, 0 when the device reports none
    uint64_t mappable_video_size; // bytes of video memory the CPU can map at once, 0 without any
    uint64_t va_start;            // GPU virtual addresses userland may map, [va_start, va_end), both 0 without any
    uint64_t va_end;
    uint32_t page_size;           // the mapping granule, 4096
    uint32_t placements;          // a bit (1 << placement) per GPU_PLACEMENT_* the device offers
    struct gpu_nvidia_info nvidia;
    struct gpu_virtio_info virtio;
};

// GPU_GET_MEMORY argument, out
struct gpu_memory {
    uint64_t video_free;          // bytes of video memory no buffer holds
    uint64_t mappable_video_free; // bytes of the CPU's window into video memory no mapping holds
};

// GPU_READ_TIMESTAMP argument, out: the GPU's clock and the kernel's monotonic clock, read right after the GPU's
struct gpu_timestamp {
    uint64_t gpu_ns;
    uint64_t cpu_ns;
};

// GPU_CONTEXT_CREATE (lost and fault_va zero in, context out, with both rights) and GPU_CONTEXT_QUERY (context in, the
// rest out). A lost context never runs work again, so its process must create another to run more
struct gpu_context_args {
    int32_t  context;
    uint32_t lost;     // GPU_LOST_*, 0 while live
    uint64_t fault_va; // the faulting address when lost to a fault and the backend knows it, else 0
};

// GPU_QUEUE_CREATE (context and kind in, queue out) and GPU_QUEUE_DESTROY (context, queue and a zero kind in). A
// create fails with EBUSY while the context holds GPU_QUEUES_MAX queues. A destroy ends the queue's held work failed
// and frees its index once its running work ended, and returns OK on a lost context too
struct gpu_queue_args {
    int32_t  context;
    uint32_t kind;    // GPU_QUEUE_*, one the device offers
    uint32_t queue;   // the queue's index in its context
    uint32_t padding;
};

// GPU_BUFFER_CREATE (size, placement and flags in, and context and backend_id for GPU_PLACEMENT_CONTEXT, with buffer
// and device_id out) and GPU_BUFFER_IMPORT (source in, context and backend_id zero, the rest out). Importing a GPU
// buffer gives another handle to it. Importing shared memory gives a GPU_PLACEMENT_SYSTEM buffer of the object's pages,
// kept while the GPU may reach them however the object is resized, and fails with EINVAL for an empty object or on a
// device without GPU_PLACEMENT_SYSTEM, and with ENOMEM when the GPU cannot reach one of the pages coherently. The CPU
// maps those pages only through the shared memory handle
struct gpu_buffer_args {
    uint64_t size;       // bytes, nonzero, rounded up to page_size
    uint32_t placement;  // GPU_PLACEMENT_*, one the device offers
    uint32_t flags;      // GPU_BUFFER_*
    int32_t  source;     // import only: a GPU buffer or shared memory handle with read, whose rights the new one gets
    int32_t  buffer;     // the new handle, with both rights from a create
    uint32_t device_id;  // the backend's name for the memory in command lists, 0 where commands use addresses
    int32_t  context;    // GPU_PLACEMENT_CONTEXT only: the context whose commands allocated the memory, with write
    uint64_t backend_id; // GPU_PLACEMENT_CONTEXT only: the backend's name for that memory, venus's memory id
};

// GPU_MAP and GPU_UNMAP argument. Unmap ignores buffer, offset, kind and flags. Map fails with EBUSY on a range another
// mapping holds, and unmap splits the mappings its range cuts. Once the GPU failed, unmap fails with EIO and removes
// the range all the same. On a device without user addresses (va_start and va_end 0) map takes va, offset, size and
// kind as 0, maps the whole buffer and fails with EBUSY on a buffer the context maps already, and unmap names the
// mapping by the buffer's device_id in va and ignores size. The field comments hold for a device with user addresses
struct gpu_map_args {
    int32_t  context;
    int32_t  buffer;  // read, and write unless GPU_MAP_READ_ONLY
    uint64_t va;      // page aligned, the range inside [va_start, va_end)
    uint64_t offset;  // into the buffer, page aligned, the range inside the buffer
    uint64_t size;    // page aligned, nonzero
    uint32_t kind;    // the backend's layout code, 0 for linear memory
    uint32_t flags;   // GPU_MAP_*
};

struct gpu_point {
    int32_t  timeline;
    uint32_t padding;
    uint64_t value;
};

// GPU_TIMELINE_CREATE (value in, timeline out), GPU_TIMELINE_SIGNAL (both in, EINVAL for a value at or below the
// timeline's) and GPU_TIMELINE_QUERY (timeline in, value out)
struct gpu_timeline_args {
    int32_t  timeline;
    uint32_t flags;  // zero into a create or signal, and GPU_TIMELINE_* out of a query
    uint64_t value;
};

// GPU_TIMELINE_WAIT argument. The wait fails with ETIMEDOUT once its deadline passes before it is satisfied, and with
// EINTR when a signal with a handler arrives first, or restarts with the same deadline when the handler has SA_RESTART
struct gpu_wait_args {
    uint64_t points;      // the address of `count` gpu_point
    uint64_t deadline_ns; // on the kernel's monotonic clock, UINT64_MAX for none
    uint32_t count;       // 1 to GPU_POINTS_MAX
    uint32_t flags;       // GPU_WAIT_*
    uint32_t reached;     // out: the index of a reached point when waiting for any
    uint32_t padding;
};

// One NVIDIA command range: push buffer bytes at a GPU virtual address of the submitting context. An NVIDIA command
// list is an array of them, run in order
struct gpu_nvidia_range {
    uint64_t va;    // a multiple of 4, the range inside [va_start, va_end)
    uint32_t size;  // bytes, a nonzero multiple of 4 below 8 MiB
    uint32_t flags; // GPU_COMMANDS_*
};

// GPU_SUBMIT argument. The submission runs after the queue's earlier submissions once every wait point is reached, and
// reaches its signal points when it ends, with GPU_TIMELINE_FAILED when it failed. A wait point reached while its
// timeline has that mark, which a timeline keeps for good, fails it without running. The call returns at once, with
// EBUSY while the queue holds GPU_QUEUE_PENDING_MAX submissions and EINVAL for a command list the backend refuses
struct gpu_submit_args {
    int32_t  context;
    uint32_t queue;         // the index GPU_QUEUE_CREATE returned
    uint64_t commands;      // the address of the command list, in the backend's format
    uint64_t waits;         // the address of `wait_count` gpu_point, each timeline with read
    uint64_t signals;       // the address of `signal_count` gpu_point, each timeline with write
    uint32_t commands_size; // bytes, 0 to GPU_COMMANDS_BYTES_MAX: 0 runs nothing but still waits and signals in order
    uint32_t wait_count;    // 0 to GPU_POINTS_MAX
    uint32_t signal_count;  // 0 to GPU_POINTS_MAX
    uint32_t padding;
};

// GPU_HANDLE_RESTRICT argument: a new handle to the same context, buffer or timeline with a subset of the source's
// rights
struct gpu_restrict_args {
    int32_t  handle;     // the source handle
    uint32_t rights;     // GPU_RIGHT_*, a nonempty subset of the source's
    int32_t  restricted; // out: the new handle
    uint32_t padding;
};

GPU_ABI_ASSERT(sizeof(struct gpu_nvidia_info) == 24, "gpu_nvidia_info layout");
GPU_ABI_ASSERT(sizeof(struct gpu_virtio_info) == 16, "gpu_virtio_info layout");
GPU_ABI_ASSERT(sizeof(struct gpu_info) == 168, "gpu_info layout");
GPU_ABI_ASSERT(sizeof(struct gpu_memory) == 16, "gpu_memory layout");
GPU_ABI_ASSERT(sizeof(struct gpu_timestamp) == 16, "gpu_timestamp layout");
GPU_ABI_ASSERT(sizeof(struct gpu_context_args) == 16, "gpu_context_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_queue_args) == 16, "gpu_queue_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_buffer_args) == 40, "gpu_buffer_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_map_args) == 40, "gpu_map_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_point) == 16, "gpu_point layout");
GPU_ABI_ASSERT(sizeof(struct gpu_timeline_args) == 16, "gpu_timeline_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_wait_args) == 32, "gpu_wait_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_nvidia_range) == 16, "gpu_nvidia_range layout");
GPU_ABI_ASSERT(sizeof(struct gpu_submit_args) == 48, "gpu_submit_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_restrict_args) == 16, "gpu_restrict_args layout");

#endif // STELLUX_GPU_GPU_ABI_H
