#ifndef STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_BACKEND_H
#define STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_BACKEND_H

#include "common/list.h"
#include "dma/dma.h"
#include "drivers/virtio/virtqueue.h"
#include "gpu/gpu.h"
#include "mm/shmem.h"
#include "rc/reaper.h"

/*
 * virtio-gpu's 3D path as the GPU core's backend, for venus, Mesa's Vulkan
 * driver for the device. A space is a venus context. A host blob is memory the
 * host allocated for a context, and a guest blob is a resource of the guest's
 * pages. A CONTEXT buffer is a host blob, mapped into the host-visible region
 * when mappable. A SYSTEM buffer is a guest blob, offered only when the host
 * accepted the one-page guest blob that the driver's task creates before
 * registering.
 */

namespace drivers::virtio {
struct virtio_gpu_mem_entry;
} // namespace drivers::virtio

namespace drivers {

class virtio_gpu_driver;

// A range of the device's host-visible memory region, in bytes from the region's start
struct region_range {
    uint64_t   offset;
    uint64_t   size;
    list::node link;
};

// The host-visible region's allocator, handing out whole pages at the lowest offset with room. Each range lives in the
// object holding it, so no call allocates, and the caller serializes the calls
class region_allocator {
public:
    // Starts with none of the region's `size` bytes handed out, a partial last page left unused
    void init(uint64_t size);

    // Hands out `size` bytes, which must be a nonzero whole number of pages, in `out`, or returns false without room
    bool allocate(uint64_t size, region_range& out);

    void release(region_range& range);

private:
    using range_list = list::head<region_range, &region_range::link>;

    range_list m_ranges; // handed out, by offset
    uint64_t   m_size = 0;
};

// Lists `count` pages in `entries` as runs of contiguous pages, returning how many entries it wrote, or 0 when they
// need more than `capacity` entries
size_t list_page_runs(
    const pmm::phys_addr_t* pages,
    size_t count,
    virtio::virtio_gpu_mem_entry* entries,
    size_t capacity
);

// A virtio context id, held from its context's creation until the host answers the context's destroy OK
struct context_id {
    uint32_t   value;
    list::node link;
};

// The allocator of virtio context ids, from a counter that wraps and skips every id still held, since the host answers
// a create on an id in use OK and keeps that id's context. The caller serializes the calls
class context_id_allocator {
public:
    // Starts with no id held, counting up from 1 to `last` and wrapping back to 1, since 0 names no context
    void init(uint32_t last);

    // Holds the next id no other holder has in `out`, or returns false when every id is held
    bool take(context_id& out);

    void give_back(context_id& id);

private:
    using id_list = list::head<context_id, &context_id::link>;

    bool is_held(uint32_t value);

    id_list  m_held;
    uint32_t m_next = 1;
    uint32_t m_last = 0;
};

// A blob resource the backend created or wrapped, on the unprivileged heap
struct virtio_gpu_memory : gpu::memory {
    uint32_t              resource_id;
    uint32_t              placement;    // `GPU_PLACEMENT_CONTEXT` or `GPU_PLACEMENT_SYSTEM`
    dma::buffer           pages;        // a created guest blob's pages
    mm::shmem_mapping     pin;          // keeps a wrapped guest blob's pages allocated
    region_range          range;        // where a mapped host blob lies in the host-visible region
    bool                  is_mapped;    // the host mapped the host blob at `range`
    bool                  is_cacheable; // the host maps a mapped host blob cached
    list::node            release_link; // on the backend's release list once the core released it
    rc::reaper::dead_node reaper_node;  // releases the pin on the reaper
};

// A venus context the backend created for the GPU core, on the unprivileged heap
struct virtio_gpu_space : gpu::space {
    context_id id;
    list::node destroy_link; // on the backend's destroy list once the core destroyed it
};

/**
 * The 3D path's backend, which the driver holds for its life. The driver's
 * task registers it when the device offers 3D commands, blobs, context
 * initialization, venus and a host-visible region of at least a page that
 * starts on a page boundary.
 *
 * The driver's task sends the releases and destroys the core asks for. A guest
 * blob's pages or pin, a mapped host blob's range and a context's id are freed
 * only once the host answers their release or destroy OK, else kept for good.
 *
 * The backend fails for good when the host refuses a detach, when one of its
 * commands times out, or when the backend next tries a command after the
 * driver marks the device broken. A registered backend's task then reports the
 * failure to the core.
 */
class virtio_gpu_backend : public gpu::backend {
public:
    explicit virtio_gpu_backend(virtio_gpu_driver& driver);

    virtio_gpu_backend(const virtio_gpu_backend&) = delete;
    virtio_gpu_backend& operator=(const virtio_gpu_backend&) = delete;

    // Registers the backend with the GPU core when the device offers what venus needs, once, from the driver's task
    void register_with_core();

    // Whether the driver's task has the backend's work to serve, under the driver's work lock
    bool has_task_work() const;

    // Serves one queued release, one queued destroy and an unreported failure, whichever are pending
    void serve_task_work();

    void read_facts(gpu::device_facts& out) override;
    void read_memory(gpu_memory& out) override;
    int32_t read_timestamp(uint64_t& gpu_ns) override;

    int32_t create_memory(
        uint64_t size,
        uint32_t placement,
        uint32_t flags,
        gpu::space* s,
        uint64_t backend_id,
        gpu::memory** out,
        uint32_t* out_device_id
    ) override;
    int32_t wrap_pages(
        const pmm::phys_addr_t* pages,
        size_t count,
        mm::shmem_mapping&& pin,
        gpu::memory** out,
        uint32_t* out_device_id
    ) override;
    int32_t view_for_cpu(gpu::memory* mem, gpu::cpu_view& out) override;
    void release_memory(gpu::memory* mem) override;

    int32_t create_space(gpu::context* core, gpu::space** out) override;
    void destroy_space(gpu::space* s) override;
    int32_t map(
        gpu::space* s,
        uint64_t va,
        gpu::memory* mem,
        uint64_t offset,
        uint64_t size,
        uint32_t kind,
        bool read_only
    ) override;
    int32_t unmap(gpu::space* s, uint64_t va, uint64_t size) override;

    int32_t create_queue(gpu::space* s, uint32_t kind, gpu::core_queue* core, gpu::queue** out) override;
    void stop_queue(gpu::queue* q) override;
    void destroy_queue(gpu::queue* q) override;
    int32_t check_commands(const void* commands, size_t size) override;
    void run(gpu::queue* q, uint64_t sequence, const void* commands, size_t size) override;

private:
    // How the probe ended. `refused` also covers a probe without memory, which never reaches the host
    enum class probe_answer : uint8_t { created, refused, broken };

    using release_list = list::head<virtio_gpu_memory, &virtio_gpu_memory::release_link>;
    using destroy_list = list::head<virtio_gpu_space, &virtio_gpu_space::destroy_link>;

    // Reads the bit of each capset id the device lists into `m_capsets`
    int32_t read_capsets();

    // Creates and releases a one-page guest blob, keeping its page for good if the device breaks or the release fails
    probe_answer probe_guest_blobs();

    int32_t allocate_pages(size_t count, paging::page_flags_t access, dma::buffer& out);

    // Creates `mem` as a guest blob of the `size` bytes `list`'s `count` entries hold, freeing `list` once answered
    int32_t create_guest_blob(virtio_gpu_memory& mem, dma::buffer& list, uint32_t count, uint64_t size);

    int32_t create_system_memory(uint64_t size, virtio_gpu_memory& mem);
    int32_t create_context_memory(
        uint64_t size,
        uint32_t flags,
        const virtio_gpu_space& space,
        uint64_t backend_id,
        virtio_gpu_memory& mem
    );

    // Maps `mem`'s new host blob at its range, under the batch lock, dropping the blob when the host refuses
    int32_t map_host_blob(virtio_gpu_memory& mem);

    // The task's halves of `release_memory()` and `destroy_space()`
    void release_on_task(virtio_gpu_memory* mem);
    void destroy_on_task(virtio_gpu_space* space);

    // Under the batch lock, before a command: whether the backend may send it, failing the backend on a broken device
    bool can_send();

    /**
     * Under the batch lock: runs one command in a batch of its own, with
     * `list` chained between its request and response when not null, writing
     * the response's offset in the command page to `out_response_offset` when
     * not null. Returns `gpu::OK` for an OK answer, `gpu::ERR_NO_MEMORY` for
     * an error answer, which it does not log, or `gpu::ERR_DEVICE` once the
     * device is broken or the backend failed.
     */
    int32_t send_command(
        const void* request,
        uint32_t request_size,
        uint32_t response_size,
        uint32_t* out_response_offset,
        const virtio::chain_buffer* list
    );

    // For commands whose response is only a header
    int32_t send(const void* request, uint32_t request_size);

    template <typename T>
    int32_t send(const T& request) {
        return send(&request, sizeof(request));
    }

    bool has_failed();

    // Sets the failed mark and wakes the task to report it, from any thread
    void fail();

    void lock_batch();
    void unlock_batch();

    virtio_gpu_driver& m_driver;
    gpu::device*       m_device = nullptr; // the core's device once registered

    // Fixed at registration, so every call reads them with no lock
    uint64_t         m_capsets = 0;
    pmm::phys_addr_t m_region_phys = 0;
    uint64_t         m_region_size = 0;
    bool             m_offers_system = false;
    uint8_t          m_pci_revision = 0;

    // Under the driver's batch lock
    region_allocator     m_region;
    context_id_allocator m_context_ids;

    // Under the driver's work lock
    release_list m_releases;
    destroy_list m_destroys;
    bool         m_is_failed = false;
    bool         m_is_failure_reported = false;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_VIRTIO_GPU_BACKEND_H
