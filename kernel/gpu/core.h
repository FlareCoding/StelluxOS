#ifndef STELLUX_GPU_CORE_H
#define STELLUX_GPU_CORE_H

#include "common/rb_tree.h"
#include "common/types.h"
#include "fs/node.h"
#include "gpu/gpu.h"
#include "mm/device_backing.h"
#include "rc/reaper.h"
#include "rc/ref_counted.h"
#include "resource/resource.h"
#include "sync/atomic.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace gpu {

class buffer;

/**
 * A context's queue on one of the backend's hardware queues. Its holders are GPU_QUEUE_CREATE until it returns, its
 * context's slot, the backend until its stop, and each path that asks it to stop. It lives on the unprivileged heap
 * until its last reference drops.
 */
struct core_queue : rc::ref_counted<core_queue> {
    core_queue(device& owner, context& in, uint32_t queue_kind);

    device&                dev;
    context&               ctx; // reached only under `lock` before the queue is asked to stop, or in its stop
    uint32_t               kind;
    uint32_t               index = 0;
    queue*                 backend_queue = nullptr;
    sync::spinlock         lock = sync::SPINLOCK_INIT; // the marks below, taken with interrupts off
    bool                   asked_to_stop = false;
    bool                   stopped = false;
    bool                   destroying = false;
    rc::reaper::dead_node  cleanup_node; // GPU_QUEUE_DESTROY's cleanup

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(core_queue* self);
};

// One mapping in a context's range tree, holding a reference to its buffer
struct mapping {
    rbt::node link;
    uint64_t  va;   // the GPU address, or the buffer's device id on a device without user addresses
    uint64_t  size; // bytes, or 1 for a device id
    buffer*   buf;
};

struct mapping_order {
    bool operator()(const mapping& a, const mapping& b) const { return a.va < b.va; }
};

enum class slot_state : uint8_t {
    FREE,
    RESERVED,   // a GPU_QUEUE_CREATE is creating its queue
    PUBLISHED,
    DESTROYING, // GPU_QUEUE_DESTROY named it, and its cleanup frees the slot
};

struct queue_slot {
    core_queue* queue = nullptr; // set while published or destroying, with a reference
    slot_state  state = slot_state::FREE;
};

/**
 * The core's side of one backend address space, with its mappings and up to GPU_QUEUES_MAX queues. Its handles, each
 * call naming it and GPU_QUEUE_DESTROY's cleanups hold references. The last one starts teardown on the reaper: the
 * first stage asks every queue to stop, and the second, once every queue stopped, destroys the queues and the space,
 * drops the mappings' buffer references and frees the context. It lives on the unprivileged heap.
 */
struct context : rc::ref_counted<context> {
    explicit context(device& owner);

    device&                dev;
    space*                 backend_space = nullptr;
    sync::mutex            lock; // the space and the mappings, held across create_queue, map and unmap
    rbt::tree<mapping, &mapping::link, mapping_order> mappings;
    sync::spinlock         slot_lock = sync::SPINLOCK_INIT; // the slots
    queue_slot             slots[GPU_QUEUES_MAX];
    sync::atomic<uint32_t> stop_count{1}; // one for the first stage and one per queue until it stops
    rc::reaper::dead_node  first_stage;
    rc::reaper::dead_node  second_stage;

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(context* self);
};

/**
 * One registered GPU, published as the device node /dev/gpuN, whose file takes every ioctl call gpu_abi.h defines. It
 * lives on the privileged heap for good, since no backend unregisters.
 */
class device final : public fs::node {
public:
    device(const char* name, backend* b, const device_facts& facts);

    int32_t ioctl(fs::file* f, uint32_t cmd, uint64_t arg) override;

    backend& get_backend() const { return *m_backend; }
    const gpu_info& info() const { return m_facts.info; }

private:
    backend*     m_backend;
    device_facts m_facts;
};

/**
 * GPU memory the core counts as one object. The resource object its handles name and each CPU and GPU mapping of it
 * hold a reference, and after the last one drops the reaper returns the memory to the device's backend and frees the
 * buffer. It lives on the unprivileged heap, and its device outlives it.
 */
class buffer final : public mm::device_backing {
public:
    buffer(device& owner, uint64_t size, uint32_t flags) : m_device(owner), m_size(size), m_flags(flags) {}

    device& owner() const { return m_device; }
    uint64_t size() const { return m_size; }
    uint32_t flags() const { return m_flags; }
    memory* backend_memory() const { return m_memory; }
    uint32_t device_id() const { return m_device_id; }

    // Hands the buffer the memory its backend created for it, returned to the backend after the last reference drops
    void take(memory* mem, uint32_t device_id) {
        m_memory = mem;
        m_device_id = device_id;
    }

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void release_memory() override;

    device&  m_device;
    memory*  m_memory = nullptr;
    uint64_t m_size;
    uint32_t m_flags;
    uint32_t m_device_id = 0;
};

/**
 * A 64-bit counter that only rises, from the value its creator chose. The resource object its handles name holds a
 * reference. It lives on the unprivileged heap and belongs to no device, so a call on any GPU's file may name it.
 */
struct timeline : rc::ref_counted<timeline> {
    sync::spinlock   lock;    // guards the value, taken with interrupts off
    uint64_t         value;
    sync::wait_queue waiters; // CPU waiters, woken at each rise

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(timeline* self);
};

/**
 * @brief Translate a backend's code into the file layer's, any code outside the backend's contract as ERR_IO.
 */
int32_t to_fs_error(int32_t rc);

/**
 * @brief Allocate `bytes` of zeroed unprivileged memory for one of the core's objects, freed with `heap::ufree`.
 */
void* allocate(size_t bytes);

/**
 * @brief For tests only: makes the core's next `allocate()` fail.
 */
void __dbg_test_fail_next_allocation();

template <typename T, typename... Args>
T* make(Args&&... args) {
    void* p = allocate(sizeof(T));
    return p ? new (p) T(static_cast<Args&&>(args)...) : nullptr;
}

/**
 * @brief Copy `bytes` in from the user address `src`.
 * @return fs::OK, or fs::ERR_INVAL when the kernel cannot read them.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_in(void* dst, uint64_t src, size_t bytes);

/**
 * @brief Copy `bytes` out to the user address `dst`.
 * @return fs::OK, or fs::ERR_INVAL when the kernel cannot write them.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t copy_out(uint64_t dst, const void* src, size_t bytes);

/**
 * @brief Look `handle` up in the calling process as an object of `type` whose handle holds `rights`, taking a
 * reference the caller must release.
 * @return fs::OK with the object in `*out`, or fs::ERR_BADF.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t lookup_object(
    int32_t handle,
    resource::resource_type type,
    uint32_t rights,
    resource::resource_object** out
);

/**
 * @brief Look a context or buffer handle up as `lookup_object()` does, refusing one of another device than `dev`.
 * @return fs::OK with the object in `*out`, fs::ERR_BADF, or fs::ERR_INVAL for another device's object.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t lookup_on_device(
    device& dev,
    int32_t handle,
    resource::resource_type type,
    uint32_t rights,
    resource::resource_object** out
);

/**
 * @brief Install a handle to `obj` with `rights` in the calling process and write it to the user address
 * `user_handle`, closing the handle again when the write fails. The handle takes a reference of its own.
 * @return fs::OK, fs::ERR_NOMEM when the handle table is full or cannot grow, or fs::ERR_INVAL when the write fails.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t install_handle(resource::resource_object* obj, uint32_t rights, uint64_t user_handle);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_buffer(buffer* buf);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_queue(core_queue* q);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_context(context* c);

/**
 * @brief Ask `q` to stop unless it was asked already, calling the backend's `stop_queue()`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ask_to_stop(core_queue& q);

/**
 * @brief Drop one of `c`'s stop counts, deferring teardown's second stage at the last.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_stop_count(context& c);

/**
 * @brief Reserve a free slot of `c` for a queue under creation and count that queue's stop.
 * @return fs::OK with the slot in `*out`, or fs::ERR_BUSY.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reserve_slot(context& c, uint32_t* out);

/**
 * @brief Free a reserved slot whose queue was not created, with its stop count.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unreserve_slot(context& c, uint32_t index);

/**
 * @brief Publish `q` in its reserved slot with a reference of the slot's.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void publish_queue(context& c, core_queue& q);

/**
 * @brief Mark the published queue at `index` destroying, taking a queue reference for the caller and a context
 * reference for the queue's cleanup.
 * @return the queue, or null for none.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE core_queue* take_queue_for_destroy(context& c, uint32_t index);

/**
 * @brief Free the slot of a destroyed queue.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void free_slot(context& c, uint32_t index);

// Each call below takes the user address of its argument and returns a file layer code, as gpu_abi.h's rules give

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_context(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t query_context(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_queue(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t destroy_queue(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_buffer(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t map_buffer(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unmap_range(device& dev, uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create_timeline(uint64_t arg);

/**
 * @brief `GPU_TIMELINE_SIGNAL`: raise the value, waking every CPU waiter, or return fs::ERR_INVAL for a value at or
 * below the current one.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t signal_timeline(uint64_t arg);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t query_timeline(uint64_t arg);

/**
 * @brief `GPU_TIMELINE_WAIT`: sleep until every point, or any one, is reached, returning fs::ERR_TIMEDOUT at the
 * deadline or fs::ERR_INTR when a signal interrupts the caller.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t wait_timelines(uint64_t arg);

} // namespace gpu

#endif // STELLUX_GPU_CORE_H
