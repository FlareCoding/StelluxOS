#ifndef STELLUX_GPU_GPU_H
#define STELLUX_GPU_GPU_H

#include "common/types.h"
#include "gpu/gpu_abi.h"
#include "mm/pmm_types.h"

/*
 * The GPU core is the kernel's common code for /dev/gpuN, whose calls gpu_abi.h defines. It owns handles, rights and
 * the life of every object, and names no card or chip. A backend is the driver half for one kind of GPU. A driver
 * registers its backend with `register_backend()`, which publishes it as the next /dev/gpuN and keeps it for good.
 * Userland calls the core, and the core calls the backend in the core's own vocabulary, the codes below, which the core
 * translates once into the file layer's.
 *
 *   process --ioctl, mmap--> GPU core (handles, rights, buffers, timelines) --gpu::backend--> driver
 *
 * A buffer is GPU memory the core counts as one object. Its backend allocates the memory in `create_memory()` and gets
 * it back in `release_memory()` once no handle and no CPU mapping refer to it. A timeline is a 64-bit counter that only
 * rises, which processes signal, read and wait for, and which no backend sees.
 */

namespace gpu {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_MEMORY   = -1;
constexpr int32_t ERR_UNSUPPORTED = -2; // the backend lacks what the call needs, such as a GPU clock
constexpr int32_t ERR_DEVICE      = -3; // the GPU failed for good

class device;

// The base of a backend's memory object, which the core holds and hands back without reading it
struct memory {};

// What a backend reports about its GPU, once, at registration
struct device_facts {
    gpu_info info;
};

// Memory the CPU may map, contiguous for `size` bytes
struct cpu_view {
    pmm::phys_addr_t phys;
    uint64_t         size;
    bool             cacheable; // false maps it write-combining
};

/**
 * One kind of GPU's driver half, which the core calls from the calling thread unless a call says otherwise.
 *
 * Once its GPU failed, a backend must return ERR_DEVICE from every call that may fail and keep every memory object for
 * good, since a failed GPU may still access the memory.
 */
class backend {
public:
    virtual ~backend() = default;

    /**
     * @brief Fill the GPU's facts, which never change while it is registered. `info.page_size` must be
     * `pmm::PAGE_SIZE`, which the core rounds buffers up to. The core calls `create_memory()` only with a placement the
     * facts' `placements` hold.
     */
    virtual void read_facts(device_facts& out) = 0;

    /**
     * @brief Report how much video memory and how much of the CPU's window into it are free. May sleep.
     */
    virtual void read_memory(gpu_memory& out) = 0;

    /**
     * @brief Read the GPU's clock, in nanoseconds. May sleep.
     * @return OK, ERR_UNSUPPORTED when the GPU has no clock, or ERR_DEVICE.
     */
    virtual int32_t read_timestamp(uint64_t& gpu_ns) = 0;

    /**
     * @brief Allocate `size` bytes, a whole number of pages, of zeroed memory in `placement` (`GPU_PLACEMENT_*`),
     * placed for scanout with `GPU_BUFFER_SCANOUT` and mappable for the CPU for its whole life with
     * `GPU_BUFFER_MAPPABLE`. `*out_device_id` gets the backend's name for the memory in command lists, 0 where
     * commands use addresses. May sleep while it zeroes.
     * @return OK, ERR_NO_MEMORY with nothing allocated, or ERR_DEVICE, the memory then kept for good.
     */
    virtual int32_t create_memory(
        uint64_t size,
        uint32_t placement,
        uint32_t flags,
        memory** out,
        uint32_t* out_device_id
    ) = 0;

    /**
     * @brief Describe mappable memory as one contiguous range the CPU maps, which must start on a page boundary, cover
     * the whole memory and stay valid for its whole life. May sleep.
     * @return OK, or ERR_DEVICE.
     */
    virtual int32_t view_for_cpu(memory* mem, cpu_view& out) = 0;

    /**
     * @brief Take back memory that no handle and no CPU mapping refers to any more. Called from the reaper, so it
     * must not wait for the hardware: a backend whose release needs it queues the release to a task of its own.
     */
    virtual void release_memory(memory* mem) = 0;
};

/**
 * @brief Ready the core for `register_backend()`, once at boot before any driver calls it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init();

/**
 * @brief Publish `b` as the next /dev/gpuN, reading its facts once. May sleep. The core keeps the device for good,
 * since no backend unregisters, so the caller must keep `b` allocated for good once this returns OK.
 * @return OK with the device in `*out`, or ERR_NO_MEMORY when the device cannot be allocated or published.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_backend(backend* b, device** out);

} // namespace gpu

#endif // STELLUX_GPU_GPU_H
