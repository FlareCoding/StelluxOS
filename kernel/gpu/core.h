#ifndef STELLUX_GPU_CORE_H
#define STELLUX_GPU_CORE_H

#include "common/list.h"
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
#include "timer/timer.h"

namespace gpu {

class buffer;
struct submission;
struct timeline;
struct unmap_marker;

// A held submission's wait for one point, linked on its timeline's list until the point is reached
struct wait_entry {
    list::node  link;
    submission* owner;
    timeline*   tl; // a reference of the submission's
    uint64_t    value;
};

// A point a submission reaches when it ends, with a reference to its timeline
struct signal_point {
    timeline* tl;
    uint64_t  value;
};

/**
 * One GPU_SUBMIT's work, held on its queue until `run()` gets it, then running until the backend reports its sequence
 * number finished. The path that ends it frees it, with its command list, once it took it off the queue.
 */
struct submission {
    list::node             link;        // on its queue's held or running list, then on the ending path's own
    core_queue*            queue;
    uint64_t               sequence;    // the number `run()` got
    sync::atomic<uint32_t> unmet;       // waits whose point the timeline has not reached
    sync::atomic<uint32_t> wait_failed; // nonzero once a wait was reached while its timeline had the failed mark
    uint32_t               wait_count;
    uint32_t               signal_count;
    uint32_t               commands_size;
    wait_entry*            waits;
    signal_point*          signals;
    uint8_t*               commands;
};

using submission_list = list::head<submission, &submission::link>;

// An unmap marker's place on one queue, passed once the queue finished `sequence` or stopped
struct marker_join {
    list::node    link;
    unmap_marker* marker;
    uint64_t      sequence;
};

using marker_list = list::head<marker_join, &marker_join::link>;

/**
 * What one GPU_UNMAP cut out of its context: a reference to each buffer it cut, held until every queue that had work
 * running at the unmap passed the marker, so no submission still running can reach memory that was released.
 */
struct unmap_marker {
    sync::atomic<uint32_t> count; // the unmap's own and one per queue joined
    marker_join            joins[GPU_QUEUES_MAX];
    uint32_t               buffer_count;
    buffer**               buffers;
};

// The queues a path collected, each with a reference the path drops once it ran the queue
struct collected_queues {
    core_queue* first = nullptr;
};

/**
 * A context's queue, which runs its submissions in order on one of the backend's hardware queues. Its holders are
 * GPU_QUEUE_CREATE until it returns, its context's slot, the backend until its stop, each scheduled deadline, each
 * GPU_SUBMIT and GPU_UNMAP while it runs, and each path that collected it or asks it to stop. It lives on the
 * unprivileged heap until its last reference drops.
 */
struct core_queue : rc::ref_counted<core_queue> {
    core_queue(device& owner, context& in, uint32_t queue_kind);

    device&                dev;
    context&               ctx; // reached only under `lock` before the queue is asked to stop, or in its stop
    uint32_t               kind;
    uint32_t               index = 0;
    queue*                 backend_queue = nullptr;
    sync::spinlock         lock = sync::SPINLOCK_INIT; // everything below up to `deadline`, taken with interrupts off
    submission_list        held;                       // in submission order
    submission_list        running;                    // in sequence order
    marker_list            markers;                    // in sequence order
    uint64_t               running_bytes = 0;
    uint64_t               last_run = 0;      // the sequence number `run()` got last
    uint64_t               last_finished = 0; // the highest the backend reported, clamped to `last_run`
    uint64_t               deadline_ns = 0;   // when the oldest running submission passes its deadline
    bool                   asked_to_stop = false;
    bool                   stopped = false;
    bool                   destroying = false;
    timer::deadline_timer  deadline;
    list::node             device_link;    // on the device's list while the backend may name the queue
    core_queue*            collected_next = nullptr;
    sync::atomic<uint32_t> collected{0};   // nonzero while a path holds the queue collected
    rc::reaper::dead_node  cleanup_node;   // GPU_QUEUE_DESTROY's cleanup

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(core_queue* self);
};

using queue_list = list::head<core_queue, &core_queue::device_link>;

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
 * call naming it, GPU_QUEUE_DESTROY's cleanups and a deadline callback losing it hold references. The last one starts
 * teardown on the reaper: the first stage asks every queue to stop, and the second, once every queue stopped, destroys
 * the queues and the space, drops the mappings' buffer references and frees the context. It lives on the unprivileged
 * heap.
 */
struct context : rc::ref_counted<context> {
    explicit context(device& owner);

    device&                dev;
    space*                 backend_space = nullptr;
    sync::mutex            lock; // the space and the mappings, held across create_queue, map and unmap
    rbt::tree<mapping, &mapping::link, mapping_order> mappings;
    sync::spinlock         slot_lock = sync::SPINLOCK_INIT; // the slots, the lost reason and the fault address
    queue_slot             slots[GPU_QUEUES_MAX];
    uint32_t               lost_reason = 0;
    uint64_t               fault_va = 0;
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
    const device_facts& facts() const { return m_facts; }

    /**
     * @brief True once the backend reported its GPU failed.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool failed();

    /**
     * @brief List `q` as a queue the backend may name, taking the backend's reference, unless the GPU failed.
     * @return false once the GPU failed, with nothing taken.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool list_queue(core_queue& q);

    /**
     * @brief Take `q` off the list once its stop was reported.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unlist_queue(core_queue& q);

    /**
     * @brief Set the failed mark and move every listed queue, with the backend's reference, to `out`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void mark_failed(queue_list& out);

private:
    backend*       m_backend;
    device_facts   m_facts;
    sync::spinlock m_lock = sync::SPINLOCK_INIT; // the failed mark and the queue list, taken with interrupts off
    bool           m_failed = false;
    queue_list     m_queues;
};

/**
 * GPU memory the core counts as one object. The resource object its handles name, each CPU and GPU mapping of it and
 * each unmap marker that cut one hold a reference, and after the last one drops the reaper returns the memory to the
 * device's backend and frees the buffer. It lives on the unprivileged heap, and its device outlives it.
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
 * A 64-bit counter that only rises, from the value its creator chose. The resource object its handles name and each
 * submission waiting for or reaching one of its points hold a reference. It lives on the unprivileged heap and belongs
 * to no device, so a call on any GPU's file may name it.
 */
struct timeline : rc::ref_counted<timeline> {
    sync::spinlock                              lock;    // everything below, taken with interrupts off
    uint64_t                                    value;
    bool                                        failed;  // set for good once work that would reach a point failed
    list::head<wait_entry, &wait_entry::link>   entries; // held submissions' waits for points above its value
    sync::wait_queue                            waiters; // CPU waiters, woken at each rise

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
__PRIVILEGED_CODE void drop_timeline(timeline* tl);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_queue(core_queue* q);

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_context(context* c);

/**
 * @brief Raise `tl` to `value` when below it, with the failed mark when `failed`, collecting the queue of each wait
 * entry this reaches into `ready`. Takes the timeline's lock, so a caller may hold a queue's.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void reach_point(timeline& tl, uint64_t value, bool failed, collected_queues& ready);

/**
 * @brief Link `entry` on its timeline unless its point is reached already.
 * @return true when the point is reached already, with the submission's failed mark set when the timeline's is.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool link_wait(wait_entry& entry);

/**
 * @brief Unlink `entry` from its timeline if it is still linked.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unlink_wait(wait_entry& entry);

/**
 * @brief Add `q` to `ready` with the reference the caller took on it, or drop that reference when another path holds
 * the queue collected, which then runs it after the caller's change. Never sleeps.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void collect(core_queue& q, collected_queues& ready);

/**
 * @brief Run each collected queue's held head while it may run, holding no other core lock, and drop the queue.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void run_collected(collected_queues& ready);

/**
 * @brief Ask `q` to stop unless it was asked already: end its held submissions failed and, unless the GPU failed,
 * call the backend's `stop_queue()`.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void ask_to_stop(core_queue& q);

/**
 * @brief Join `join`'s marker to `q` at its last sequence number run, when `q` has work running.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void join_marker(core_queue& q, marker_join& join);

/**
 * @brief Drop one count of `marker`, and at the last its buffer references.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_marker(unmap_marker* marker);

/**
 * @brief Drop one of `c`'s stop counts, deferring teardown's second stage at the last.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drop_stop_count(context& c);

/**
 * @brief The context's lost reason, GPU_LOST_DEVICE once the GPU failed unless it was lost before, else 0. `fault_va`
 * gets the fault address that came with the reason, else 0.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t lost_state(context& c, uint64_t* fault_va);

/**
 * @brief Lose `c` for `reason` unless it is lost already, asking each of its queues to stop. Never sleeps.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void lose_context(context& c, uint32_t reason, uint64_t fault_va);

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
 * @return true when `c` was lost to a fault, an engine error or a hung submission.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool publish_queue(context& c, core_queue& q);

/**
 * @brief The published queue at `index`, with a reference, or null for none.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE core_queue* find_queue(context& c, uint32_t index);

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
 * @brief `GPU_TIMELINE_SIGNAL`: raise the value, waking every CPU waiter and running each queue a reached point
 * readied, or return fs::ERR_INVAL for a value at or below the current one.
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t submit(device& dev, uint64_t arg);

} // namespace gpu

#endif // STELLUX_GPU_CORE_H
