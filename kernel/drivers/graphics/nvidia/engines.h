#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H

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
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/operation_queue.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "gpu/gpu.h"
#include "mm/shmem.h"
#include "rc/reaper.h"
#include "sync/atomic.h"

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
    dma::buffer     semaphores = {};     // a progress semaphore per queue slot, allocated for the first queue
    uint32_t        queue_slots = 0;     // a bit per slot a queue holds, each a semaphore and a window for its rings
    space_operation task_operation = {}; // the creation, then the destroy
};

// A queue's creation on the driver task, kept in the queue
struct queue_operation : operation {
    engines*       owner;
    backend_queue* queue;
};

// The report of the GPU's failure on the driver task, kept in the engines
struct failure_operation : operation {
    engines* owner;
};

// A hardware queue the engines created for the GPU core: an unprivileged channel in its context's space, allocated on
// the unprivileged heap, since the lowered driver task writes it
struct backend_queue : gpu::queue {
    backend_queue(firmware& fw, const gpu_objects& objects, video_memory& memory, registers bar0)
        : user_channel(fw, objects, memory, bar0) {}

    channel         user_channel;
    backend_space*  space = nullptr;
    dma::buffer     push_buffer = {};    // a release sequence per submission the queue may hold, and one more
    uint32_t        slot = 0;            // its semaphore and window in the space
    uint32_t        channel_id = 0;      // on the copy engine's runlist
    queue_operation task_operation = {}; // the creation
};

/**
 * The GPU's engines as the driver task brings them up once `attach()` has
 * returned `OK`: the engine table, the kernel's copy channel on the first
 * asynchronous copy engine, and that engine's nonstall vector. The driver
 * object holds one for its life, and once `register_gpu` succeeds the GPU core
 * drives the engines as the card's `gpu::backend`.
 *
 * `bring_up` waits for the firmware and for video memory, so the driver task
 * must call it, and only once.
 *
 * After a timeout the hung channel and the memory it fills stay for good. A
 * hung zeroing for the GPU core or an unmap the page tables refuse fails the
 * core's device while the display runs on. Once that device or the GPU failed,
 * every backend call that can fail, except a wrap the card cannot reach
 * coherently or the heap refuses, returns `gpu::ERR_DEVICE`, a release or a
 * space's destroy frees only a record that holds no pin, and the driver task
 * reports the failure to the core. After another error, only what a refused
 * release or a refused take-back of a page directory leaves the firmware or
 * the GPU able to reach stays.
 */
class engines : public gpu::backend {
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
     * @return `OK`, `ERR_BAD_STATE` when no asynchronous copy engine exists or
     * a word reads back wrong, `ERR_TIMEOUT` when a copy hangs the channel, or
     * the failing step's error. Every error is logged and leaves the vector
     * masked.
     */
    int32_t bring_up(registers& bar0, const dma::device_profile& card, const chip_info& chip);

    /**
     * @brief Register the engines with the GPU core as the next /dev/gpuN,
     * describing the card by `identity`. The driver task must call it once,
     * after `bring_up` returned `OK`. The core keeps the engines for good.
     * @param identity The card's identity, which `read_facts` reports.
     * @return `OK`, or `ERR_NO_MEMORY`, logged, when the core cannot publish
     * the device, which leaves the display running without one.
     */
    int32_t register_gpu(const card_identity& identity);

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
     * @brief Clear the copy engine's nonstall vector when it is pending and
     * have the copy channel read its semaphore, from the card's interrupt
     * handler. Does nothing before `bring_up` creates the copy channel or after
     * it masks the vector. Takes the copy channel's spinlock and never sleeps.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void service_interrupt(registers& bar0);

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

    // Fails the GPU core's device, logging `reason` once, and has the driver task report it
    void fail_device(const char* reason);

    // Has the driver task report the failed GPU to the core, once, from any thread
    void report_failure();

    // The driver task's half of `report_failure()`
    static void report_failure_on_task(operation& op);

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

    // The driver task's half of `create_queue()`, given the queue's `task_operation`
    static void create_queue_on_task(operation& op);

    // Creates `queue`'s channel and push buffer in its space, on the driver task
    int32_t start_queue(backend_queue& queue);

    // Maps `space`'s semaphores writable, allocating their page for the space's first queue
    int32_t map_semaphores(backend_space& space);

    // Gives back the push buffer, slot and id of a queue whose channel was not created, unmapping the push buffer
    // first when `is_mapped`, unless the firmware or the GPU may still reach them
    void release_queue(backend_queue& queue, bool is_mapped);

    // Takes the lowest id free on the copy engine's runlist above the kernel's copy channel's, or returns false
    bool take_channel_id(uint32_t& out);
    void give_back_channel_id(uint32_t id);

    firmware&          m_firmware;
    const gpu_objects& m_objects;
    video_memory&      m_memory;
    operation_queue&   m_operations;
    failed_mark&       m_failed;
    engine_table       m_table;

    registers           m_bar0{0};                          // the card's registers, set by `bring_up`
    const chip_info*    m_chip = nullptr;                   // set by `bring_up`
    dma::device_profile m_card = dma::uncached_profile(32); // replaced by the card's in `bring_up`
    card_identity       m_identity = {};                    // set by `register_gpu`
    gpu::device*        m_device = nullptr;                 // the core's device once `register_gpu` succeeds
    sync::atomic<bool>  m_is_device_failed{false};          // set by `fail_device`

    failure_operation  m_failure_report = {};        // posted once by `report_failure()`
    sync::atomic<bool> m_is_failure_reported{false}; // set once `m_failure_report` is posted

    // The kernel's copy channel in `m_copy_channel_bytes`, never destructed, since the handler may still reach it
    copy_channel*                 m_copy_channel = nullptr;
    alignas(copy_channel) uint8_t m_copy_channel_bytes[sizeof(copy_channel)];

    uint32_t           m_copy_vector = 0;
    sync::atomic<bool> m_is_copy_vector_on{false}; // stored with release once the channel exists, loaded with acquire

    // A bit per channel id held on the copy engine's runlist, which only the driver task touches
    uint64_t m_channel_ids[CHANNEL_IDS / CHANNEL_IDS_PER_WORD] = {};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_ENGINES_H
