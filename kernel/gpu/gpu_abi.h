#ifndef STELLUX_GPU_GPU_ABI_H
#define STELLUX_GPU_GPU_ABI_H

/*
 * Each GPU's device file, /dev/gpuN, takes the ioctl calls this header defines. The kernel and userland, which includes
 * the copy libstlx installs as <stlx/gpu_abi.h>, take the fixed-width integer types from different headers, so the
 * header includes nothing and an includer must provide those types first.
 *
 * A buffer is GPU memory. A process names buffers by resource handles, which it can send to other processes over unix
 * sockets with their rights. A buffer has no destroy call: it lives until its last handle closes and its last CPU
 * mapping is unmapped. A `GPU_BUFFER_MAPPABLE` buffer maps through a shared mmap of a handle with `GPU_RIGHT_READ`,
 * writable only when the handle has `GPU_RIGHT_WRITE` too.
 *
 * Each call takes the address of its argument structure and fails with
 * - EINVAL for an argument the kernel cannot read or write, a field outside what its comment allows (an unknown flag
 *   and a nonzero reserved or padding field included) or a buffer of another device than the file's,
 * - EBADF for a handle that names no object of the kind the call needs or lacks the right its use needs,
 * - ENOMEM when the kernel or the GPU has no memory left for the call, a full handle table included,
 * - EIO when the GPU fails,
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
#define GPU_BUFFER_CREATE   0x4807u
#define GPU_HANDLE_RESTRICT 0x4810u

// A handle's rights, the values of RIGHT_READ and RIGHT_WRITE in kernel/resource/resource_types.h
#define GPU_RIGHT_READ  0x1u
#define GPU_RIGHT_WRITE 0x2u

#define GPU_BACKEND_NVIDIA 1u
#define GPU_BACKEND_VIRTIO 2u
#define GPU_BACKEND_FAKE   3u // kernel tests only

#define GPU_QUEUE_GRAPHICS 1u // 3D, compute and copy commands
#define GPU_QUEUE_COPY     2u // copy commands

#define GPU_PLACEMENT_VIDEO  1u // the GPU's own memory
#define GPU_PLACEMENT_SYSTEM 2u // system memory the CPU caches and the GPU reads coherently

#define GPU_BUFFER_MAPPABLE 0x1u // the CPU may map it
#define GPU_BUFFER_SCANOUT  0x2u // placed where the display can scan it out, in video memory only

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

// GPU_BUFFER_CREATE argument: size, placement and flags in, buffer and device_id out
struct gpu_buffer_args {
    uint64_t size;       // bytes, nonzero, rounded up to page_size
    uint32_t placement;  // GPU_PLACEMENT_*, one the device offers
    uint32_t flags;      // GPU_BUFFER_*
    int32_t  source;     // reserved
    int32_t  buffer;     // the new handle, with both rights
    uint32_t device_id;  // the backend's name for the memory in command lists, 0 where commands use addresses
    int32_t  context;    // reserved
    uint64_t backend_id; // reserved
};

// GPU_HANDLE_RESTRICT argument: a new handle to the same buffer with a subset of the source's rights
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
GPU_ABI_ASSERT(sizeof(struct gpu_buffer_args) == 40, "gpu_buffer_args layout");
GPU_ABI_ASSERT(sizeof(struct gpu_restrict_args) == 16, "gpu_restrict_args layout");

#endif // STELLUX_GPU_GPU_ABI_H
