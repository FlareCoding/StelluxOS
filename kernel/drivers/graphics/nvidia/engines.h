#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H

#include "common/list.h"
#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/address_space.h"
#include "drivers/graphics/nvidia/channel.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/context_page_tables.h"
#include "drivers/graphics/nvidia/copy_channel.h"
#include "drivers/graphics/nvidia/engine_table.h"
#include "drivers/graphics/nvidia/failed_mark.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/graphics_context.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/operation_queue.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "gpu/gpu.h"
#include "mm/shmem.h"
#include "rc/reaper.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"

namespace drivers::nvidia {

class engines;
struct backend_memory;
struct backend_space;
struct backend_queue;

// The channel ids one word of the engines' id bitmap holds
constexpr uint32_t CHANNEL_IDS_PER_WORD = 64;

// The card as its PCI function and the firmware's static configuration describe it, which the GPU core reports
struct card_identity {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint8_t  revision;
    uint32_t gpc_count;
    uint32_t tpc_count;
    char     name[GPU_NAME_BYTES]; // NUL terminated
};

// A memory object's release on the driver task, kept in the object so its release allocates nothing
struct memory_operation : operation {
    engines*        owner;
    backend_memory* memory;
};

// The record of memory the engines created or wrapped for the GPU core, on the unprivileged heap, since its release
// runs on the lowered driver task
struct backend_memory : gpu::memory {
    uint32_t         placement;      // `GPU_PLACEMENT_VIDEO` or `GPU_PLACEMENT_SYSTEM`
    vram_range       range;          // video memory
    cpu_mapping      mapping;        // video memory's mapping through BAR1 for its whole life, all 0 when unmapped
    dma::buffer      pages;          // system memory the engines allocated
    memory_operation task_operation; // the release

    memory_run*           wrapped_pages; // a shared memory object's pages, one run each, or null
    mm::shmem_mapping     pin;           // keeps the wrapped pages allocated
    rc::reaper::dead_node pin_release;   // the pin's release with the record, on the reaper
};

// A space's creation and destroy on the driver task, kept in the space so its destroy allocates nothing
struct space_operation : operation {
    engines*       owner;
    backend_space* space;
};

// A GPU context's address space the engines created for the GPU core, on the unprivileged heap, since the lowered
// driver task creates and destroys it
struct backend_space : gpu::space {
    backend_space(firmware& fw, const gpu_objects& objects, video_memory& memory, failed_mark& failed)
        : addresses(fw, objects, memory, failed) {}

    address_space   addresses;
    gpu::context*   core = nullptr;      // the GPU core's context the space was created for
    dma::buffer     semaphores = {};     // a progress semaphore per queue slot, allocated for the first queue
    uint32_t        queue_slots = 0;     // a bit per slot a queue holds, each a semaphore and a window for its rings
    space_operation task_operation = {}; // the creation, then the destroy

    bool are_global_buffers_mapped = false; // by the first graphics queue to map them
};

// A queue's work on the driver task, kept in the queue so its stop and destroy allocate nothing
struct queue_operation : operation {
    engines*       owner;
    backend_queue* queue;
};

// The report of the GPU's failure on the driver task, kept in the engines
struct failure_operation : operation {
    engines* owner;
};

// A channel error's attribution to the queues whose channels faulted, on the driver task, kept in the engines
struct error_operation : operation {
    engines* owner;
    uint32_t reason; // what the GPU core is told each faulted queue's context is lost for, a GPU_LOST_* value
};

// A hardware queue the engines created for the GPU core: an unprivileged channel in its context's space, allocated on
// the unprivileged heap, since the lowered driver task writes it
struct backend_queue : gpu::queue {
    backend_queue(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0)
        : user_channel(fw, objects, memory, bar0) {}

    channel         user_channel;
    backend_space*  space = nullptr;
    uint32_t        kind = GPU_QUEUE_COPY;
    dma::buffer     push_buffer = {};       // a release sequence per submission the queue may hold, and one more
    uint32_t        slot = 0;               // its semaphore, its window and a graphics queue's own buffers in the space
    uint32_t        channel_id = 0;         // on its engine's runlist
    queue_operation task_operation = {};    // the creation, then the stop
    queue_operation destroy_operation = {}; // the destroy, kept apart from the stop's, which may still be queued
    bool            is_attributed = false;  // an error's attribution lost its context, so later ones skip the queue

    channel_context_buffers own_buffers = {}; // a graphics queue's, each range empty until allocated

    gpu::core_queue*       core = nullptr;
    list::node             link;              // on the handler's list until its stop or the failure's report
    sync::atomic<uint64_t> last_run{0};       // the sequence number `run` got last
    uint64_t               last_reported = 0; // the last finished one reported to the core, under the list's lock
};

using backend_queue_list = list::head<backend_queue, &backend_queue::link>;

/**
 * The GPU's engines as the driver task brings them up once `attach()` has
 * returned `OK`: the engine table, the kernel's copy channel on the first
 * asynchronous copy engine, the graphics engine's global context buffers and
 * golden context (the state every graphics channel starts from), and both
 * engines' nonstall vectors. The driver object holds one for its life, and
 * once `register_gpu` succeeds the GPU core drives the engines as the card's
 * `gpu::backend`.
 *
 * `bring_up` waits for the firmware and for video memory, so the driver task
 * must call it, and only once.
 *
 * After a timeout the hung channel and the memory it fills stay for good. A
 * hung zeroing for the GPU core, an unmap the page tables refuse or a failed
 * release of a queue's channel fails the core's device while the display runs
 * on. Once that device or the GPU failed, every backend call that can fail,
 * except a wrap the card cannot reach coherently or the heap refuses, returns
 * `gpu::ERR_DEVICE`, a release or a space's destroy frees only a record that
 * holds no pin, and the driver task reports the failure to the core. After
 * another error, only what a refused release or a refused take-back of a page
 * directory leaves the firmware or the GPU able to reach stays.
 */
class engines : public gpu::backend, public firmware_error_sink {
public:
    // Engines holding nothing, using `fw`, `objects`, `memory`, `operations` and `failed`, which must outlive them
    engines(
        firmware& fw,
        const gpu_objects& objects,
        video_memory& memory,
        operation_queue& operations,
        failed_mark& failed
    );

    engines(const engines&) = delete;
    engines& operator=(const engines&) = delete;

    /**
     * @brief Read the engine table, create the kernel's copy channel on the
     * first asynchronous copy engine with `bar0` for its doorbell, enable the
     * engine's nonstall vector in `chip`'s leaves, and check the channel by
     * having it zero and then fill video memory, each read back through BAR1.
     * The engines keep `chip`, which must outlive them.
     * @param bar0_bytes How many bytes of BAR0 `bar0` maps.
     * @return `OK`, `ERR_BAD_STATE` when no asynchronous copy engine exists,
     * its channel RAM is unaligned or ends past `bar0_bytes`, or a word reads
     * back wrong, `ERR_TIMEOUT` when a copy hangs the channel, or the failing
     * step's error. Every error is logged and leaves the vector masked.
     */
    int32_t bring_up(registers& bar0, uint64_t bar0_bytes, const dma::device_profile& card, const chip_info& chip);

    /**
     * @brief Allocate the graphics engine's global context buffers, capture its
     * golden context and enable its nonstall vector. The driver task must call
     * it once, after `bring_up` returned `OK` and before `register_gpu`.
     * @return `OK`, `ERR_BAD_STATE` without a graphics engine, a copy engine on
     * its runlist, the graphics engine's channel RAM aligned inside BAR0 or a
     * free channel id, or the failing step's error, each logged. An error
     * leaves `bring_up`'s engines up and gives back the context buffers but
     * those the firmware or the GPU may still reach, kept for good.
     */
    int32_t bring_up_graphics();

    /**
     * @brief Register the engines with the GPU core as the next /dev/gpuN,
     * describing the card by `identity`. The driver task must call it once,
     * after `bring_up` returned `OK`. The core keeps the engines for good.
     * @param identity The card's identity, which `read_facts` reports.
     * @return `OK`, or `ERR_NO_MEMORY`, logged, when the core cannot publish
     * the device, which leaves the display running without one.
     */
    int32_t register_gpu(const card_identity& identity);

    /**
     * @brief When the GPU's failed mark is set, mark the copy channel hung, so
     * no create waits for its zeroing, and have the failed GPU reported to the
     * GPU core, once, as the engines' own calls do for a failure they meet.
     * The driver task calls it after each wake's work, for a mark set outside
     * those calls, as by a reply the firmware missed for the display. It does
     * nothing before `register_gpu` returns `OK`.
     */
    void report_marked_failure();

    /**
     * @brief Find the video memory `view` describes when these engines created
     * it with `GPU_BUFFER_SCANOUT`. The display engine scans out no other GPU
     * memory.
     * @return Whether they did, `out` then holding the memory's whole range,
     * which covers `view.size` bytes.
     */
    bool find_scanout_range(const gpu::memory_view& view, vram_range& out) const;

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

    /**
     * @brief Clear each enabled nonstall vector that is pending, the copy
     * engine's and the graphics engine's, then have the copy channel read its
     * semaphore when the copy engine's was, and report to the GPU core each
     * listed queue's last finished submission, from the card's interrupt
     * handler. Does nothing while neither vector is enabled and pending. Takes
     * the copy channel's and the queue list's spinlocks, calls `gpu::progress`
     * under the list's, and never sleeps.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void service_interrupt(registers& bar0);

    /**
     * @brief Once `bring_up` succeeds, have the driver task lose the GPU
     * core's context of each listed queue whose channel faulted, unless the
     * GPU or the GPU core's device failed. After an MMU fault, the context of
     * a queue whose channel's fault record names a fault is lost for
     * `GPU_LOST_FAULT` at the record's address, and that of a queue only
     * channel RAM marks at address 0. After an engine error, that of a queue
     * that channel RAM marks is lost for `GPU_LOST_ENGINE`. An error reported
     * while an attribution of its kind is still queued is attributed with it.
     * @param error The channel error the firmware reported.
     */
    void on_channel_error(channel_error error) override;

private:
    friend struct engines_test_access;

    // Fills the check's memory, reads it back and logs each copy's time, keeping the memory for good after a timeout
    int32_t check_copy_channel();

    // Has the copy channel fill the checked bytes with `value`, then reads them and the page after them back
    int32_t fill_and_compare(const vram_range& range, uintptr_t virt, uint32_t value, uint64_t& took_ns);

    // Whether the GPU failed for the whole driver or for the GPU core's device alone
    bool has_failed() const;

    // Allocates and zeroes `mem`'s video memory, mapped for the CPU when `flags` hold `GPU_BUFFER_MAPPABLE`
    int32_t create_video_memory(uint64_t size, uint32_t flags, backend_memory& mem);

    // Has the copy channel zero `range`, in as few fills as their size limit allows
    int32_t zero_video_memory(const vram_range& range);

    // Allocates and zeroes `buffer`'s video memory as `layout` sizes it, setting its range only on success and keeping
    // the memory for good after a timeout
    int32_t allocate_context_buffer(const context_buffer_layout& layout, context_buffer& buffer);

    // Allocates `buffer` for bring-up as `allocate_context_buffer` does, logging every failure
    int32_t allocate_bring_up_buffer(const context_buffer_layout& layout, context_buffer& buffer);

    // Captures the golden context on a channel of `graphics`'s runlist in an address space of its own, setting
    // `is_reachable` when an error leaves the firmware or the GPU able to reach the global buffers
    int32_t capture_golden_context(const engine_info& graphics, bool& is_reachable);

    // Fails the GPU core's device, logging `reason` once, and has the driver task report it
    void fail_device(const char* reason);

    // Marks the copy channel hung, so no create waits for its zeroing, and has the driver task report the failed GPU
    // to the core, once, from any thread
    void report_failure();

    // The driver task's half of `report_failure()`: has the firmware free the user channels unless the GPU failed,
    // masks both engines' vectors, then empties the handler's list before `gpu::device_failed`
    static void report_failure_on_task(operation& op);

    // Has the firmware free each listed queue's channel, keeping its memory, until the GPU failed
    void free_user_channels();

    // The driver task's half of `on_channel_error()`, given the error's attribution record
    static void attribute_error_on_task(operation& op);

    // A creation's error once the driver task refused it: `gpu::ERR_DEVICE`, reported, once the GPU failed, else
    // `gpu::ERR_NO_MEMORY`
    int32_t refused_creation();

    // The driver task's half of `release_memory()`, given the memory's `task_operation`
    static void release_on_task(operation& op);

    // The reaper's half of a wrapped memory's release, which frees the record and so releases its pin
    static rc::reaper::cleanup_result release_on_reaper(backend_memory* mem);

    // The driver task's halves of `create_space()` and `destroy_space()`, given the space's `task_operation`
    static void create_space_on_task(operation& op);
    static void destroy_space_on_task(operation& op);

    // The driver task's halves of `create_queue()` and `stop_queue()`, given the queue's `task_operation`, and of
    // `destroy_queue()`, given its `destroy_operation`
    static void create_queue_on_task(operation& op);
    static void stop_queue_on_task(operation& op);
    static void destroy_queue_on_task(operation& op);

    // Creates `queue`'s channel and push buffer in its space on the driver task, giving back what it holds on a failure
    // unless the firmware or the GPU may still reach it
    int32_t start_queue(backend_queue& queue);

    // Maps the queue's own context buffers in its slot, after the global ones unless its space maps them already, then
    // creates its channel on the graphics engine's runlist, promoted, with its 3D, compute and copy objects
    int32_t start_graphics_channel(backend_queue& queue);

    // Allocates and zeroes a graphics queue's own context buffers, failing the device when a zeroing hangs and giving
    // back a zeroed main buffer on a failure
    int32_t allocate_own_buffers(backend_queue& queue);

    // Unmaps and gives back a queue's own context buffers, returning false when the GPU may still reach them
    bool release_own_buffers(backend_queue& queue);

    // Maps `space`'s semaphores writable, allocating their page for the space's first queue
    int32_t map_semaphores(backend_space& space);

    // Gives back the push buffer, own context buffers, slot and id of a queue left without a channel, unmapping the
    // push buffer first when `is_mapped`, unless the GPU failed or the firmware or the GPU may still reach them
    void release_queue(backend_queue& queue, bool is_mapped);

    // Takes the lowest id free above the kernel's copy channel's in the id space all runlists share, or returns false
    bool take_channel_id(uint32_t& out);
    void give_back_channel_id(uint32_t id);

    firmware&          m_firmware;
    const gpu_objects& m_objects;
    video_memory&      m_memory;
    operation_queue&   m_operations;
    failed_mark&       m_failed;
    engine_table       m_table;

    registers           m_bar0{0};                          // the card's registers, set by `bring_up`
    uint64_t            m_bar0_bytes = 0;                   // how many bytes of BAR0 `m_bar0` maps, set by `bring_up`
    const chip_info*    m_chip = nullptr;                   // set by `bring_up`
    dma::device_profile m_card = dma::uncached_profile(32); // replaced by the card's in `bring_up`
    card_identity       m_identity = {};                    // set by `register_gpu`
    gpu::device*        m_device = nullptr;                 // the core's device once `register_gpu` succeeds
    sync::atomic<bool>  m_is_device_failed{false};          // set by `fail_device`

    failure_operation  m_failure_report = {};        // posted once by `report_failure()`
    sync::atomic<bool> m_is_failure_reported{false}; // set once `m_failure_report` is posted

    // Posted by `on_channel_error()` for an MMU fault and for an engine error
    error_operation m_fault_attribution = {};
    error_operation m_engine_error_attribution = {};

    // The kernel's copy channel in `m_copy_channel_bytes`, never destructed, since the handler may still reach it
    copy_channel*                 m_copy_channel = nullptr;
    alignas(copy_channel) uint8_t m_copy_channel_bytes[sizeof(copy_channel)];

    uint32_t           m_copy_vector = 0;
    sync::atomic<bool> m_is_copy_vector_on{false}; // stored with release once the channel exists, loaded with acquire

    uint32_t           m_graphics_vector = 0;
    sync::atomic<bool> m_is_graphics_vector_on{false}; // stored with release after the capture, loaded with acquire

    // The graphics engine's context buffer layouts, its global buffers, kept for good once the golden context is
    // captured, and where queue slot 0's own buffers map, all set by `bring_up_graphics`
    graphics_buffer_layouts m_graphics_layouts = {};
    global_context_buffers  m_global_buffers = {};
    uint64_t                m_own_buffers_start = 0;

    // A bit per channel id held in the id space all runlists share, which only the driver task touches
    uint64_t m_channel_ids[CHANNEL_IDS / CHANNEL_IDS_PER_WORD] = {};

    // The queues whose semaphores the handler reads, from their creation until their stop or the failure's report. Only
    // the driver task changes the list, under the spinlock taken with interrupts off, so the task reads it unlocked
    sync::spinlock     m_queues_lock = sync::SPINLOCK_INIT;
    backend_queue_list m_queues;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
