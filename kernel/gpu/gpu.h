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
 *   process --ioctl, mmap--> GPU core (handles, rights, objects) --gpu::backend--> driver
 *
 * A buffer is GPU memory the core counts as one object. Its backend allocates the memory in `create_memory()` and gets
 * it back in `release_memory()` once nothing refers to it. A timeline is a 64-bit counter that only rises, which no
 * backend sees. A context holds one of the backend's address spaces and up to GPU_QUEUES_MAX of its hardware queues.
 */

namespace gpu {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_NO_MEMORY   = -1;
constexpr int32_t ERR_UNSUPPORTED = -2; // the backend lacks what the call needs, such as a GPU clock
constexpr int32_t ERR_DEVICE      = -3; // the GPU failed for good
constexpr int32_t ERR_REFUSED     = -4; // an argument only the backend can check, such as a mapping's kind, is wrong

class device;
struct context;
struct core_queue;

// The base of a backend's memory object, which the core holds and hands back without reading it
struct memory {};

// The base of a backend's address space, which the core holds and hands back without reading it
struct space {};

// The base of a backend's hardware queue, which the core holds and hands back without reading it
struct queue {};

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
 * The backend calls the core's `queue_stopped()` from its interrupt handler or its own task, never from inside a call
 * the core made.
 *
 * Once its GPU failed, a backend must return ERR_DEVICE from every call that may fail and keep every memory object,
 * space and queue for good, since a failed GPU may still access their memory.
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
     * `GPU_BUFFER_MAPPABLE`. For GPU_PLACEMENT_CONTEXT, `s` is the space whose commands allocated the memory and
     * `backend_id` the backend's name for it, and the memory comes as those commands left it. For every other placement
     * `s` is null and `backend_id` 0. `*out_device_id` gets the backend's name for the memory in command lists, 0 where
     * commands use addresses. May sleep while it zeroes.
     * @return OK, ERR_NO_MEMORY with nothing allocated, or ERR_DEVICE, the memory then kept for good.
     */
    virtual int32_t create_memory(
        uint64_t size,
        uint32_t placement,
        uint32_t flags,
        space* s,
        uint64_t backend_id,
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
     * @brief Take back memory that no handle and no CPU or GPU mapping refers to any more. Called from the reaper, so
     * it must not wait for the hardware: a backend whose release needs it queues the release to a task of its own.
     */
    virtual void release_memory(memory* mem) = 0;

    /**
     * @brief Create an address space for the context `core`. May sleep.
     * @return OK, ERR_NO_MEMORY with nothing created, or ERR_DEVICE.
     */
    virtual int32_t create_space(context* core, space** out) = 0;

    /**
     * @brief Free a space whose queues are all destroyed. Called from the reaper, so it must not wait.
     */
    virtual void destroy_space(space* s) = 0;

    /**
     * @brief Map `size` bytes of `mem` from `offset` at the GPU address `va` with the layout `kind`, read-only for the
     * GPU when `read_only`, returning once the GPU translates the range. On a GPU without user addresses `va` is the
     * memory's device id and `size` its whole size. The core makes one `map()` or `unmap()` call at a time per space.
     * May sleep.
     * @return OK. ERR_REFUSED for a kind it does not know or a read-only mapping it cannot enforce, or ERR_NO_MEMORY,
     * each with no entry left written. ERR_DEVICE, the range then perhaps translated.
     */
    virtual int32_t map(
        space* s,
        uint64_t va,
        memory* mem,
        uint64_t offset,
        uint64_t size,
        uint32_t kind,
        bool read_only
    ) = 0;

    /**
     * @brief Stop the GPU translating `size` bytes at `va`, returning once no TLB holds them. The range may cut
     * mappings and span pages no `map()` covered, and on a GPU without user addresses is the device id and size
     * `map()` got. Allocates nothing. May sleep.
     * @return OK, or ERR_DEVICE, the range then perhaps translated.
     */
    virtual int32_t unmap(space* s, uint64_t va, uint64_t size) = 0;

    /**
     * @brief Create a hardware queue of `kind` in `s` for `core`, which the backend names when it reports the queue's
     * stop. May sleep.
     * @return OK, ERR_NO_MEMORY with nothing created, or ERR_DEVICE.
     */
    virtual int32_t create_queue(space* s, uint32_t kind, core_queue* core, queue** out) = 0;

    /**
     * @brief Make the hardware stop running `q`'s work and report `queue_stopped()` once it has, within the backend's
     * own deadline and never from inside this call. Called at most once per queue, and never waits.
     */
    virtual void stop_queue(queue* q) = 0;

    /**
     * @brief Free a queue whose stop was reported. Called from the reaper, so it must not wait.
     */
    virtual void destroy_queue(queue* q) = 0;
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

/**
 * @brief Report that `q` runs nothing more after `stop_queue()`, as the backend's last call naming `q`. Never sleeps.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void queue_stopped(core_queue* q);

} // namespace gpu

#endif // STELLUX_GPU_GPU_H
