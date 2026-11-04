#ifndef STELLUX_TESTS_GPU_FAKE_BACKEND_H
#define STELLUX_TESTS_GPU_FAKE_BACKEND_H

#include "helpers.h"
#include "gpu/core.h"
#include "common/string.h"
#include "fs/devfs/devfs.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "mm/kva.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/atomic.h"
#include "syscall/handlers/sys_io.h"
#include "syscall/handlers/sys_mmap.h"
#include "syscall/syscall_table.h"

namespace gpu_test {

constexpr uint32_t VIDEO_AND_SYSTEM = (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM);
constexpr uint32_t SYSTEM_ONLY = 1u << GPU_PLACEMENT_SYSTEM;
constexpr uint32_t ALL_PLACEMENTS = VIDEO_AND_SYSTEM | (1u << GPU_PLACEMENT_CONTEXT);
constexpr uint32_t BOTH_QUEUE_KINDS = (1u << GPU_QUEUE_GRAPHICS) | (1u << GPU_QUEUE_COPY);
constexpr uint16_t FAKE_VENDOR_ID = 0x1D17;
constexpr uint64_t FAKE_VIDEO_SIZE = 64ull * 1024 * 1024;
constexpr uint64_t FAKE_VA_START = 1ull << 20;
constexpr uint64_t FAKE_VA_END = 1ull << 32;
constexpr uint64_t FAKE_TIMEOUT_NS = 10ull * 1000 * 1000 * 1000;
constexpr uint32_t FAKE_RANGES_MAX = 16;
constexpr uint32_t FAKE_SPACES_MAX = 4;
constexpr uint32_t FAKE_QUEUES_MAX = 2 * GPU_QUEUES_MAX;
constexpr size_t   PATH_BYTES = 32;
constexpr size_t   ARGS_AT = 0;
constexpr size_t   POINTS_AT = 512;
constexpr size_t   SIGNALS_AT = 1024;
constexpr size_t   COMMANDS_AT = 2048;
constexpr size_t   COMMANDS_ROOM = pmm::PAGE_SIZE - COMMANDS_AT;
constexpr uint64_t QUERY_FAILED = ~static_cast<uint64_t>(0);

using stop_hook = void (*)(void* arg);

// One buffer's memory, physically contiguous pages, so its CPU view is one contiguous range
struct fake_memory : gpu::memory {
    uintptr_t        kernel_va;
    pmm::phys_addr_t phys;
    uint64_t         size;
    uint32_t         placement;
    uint32_t         flags;
};

struct fake_range {
    uint64_t va;
    uint64_t size;
};

struct fake_map_call {
    gpu::memory* mem;
    uint64_t     va;
    uint64_t     offset;
    uint64_t     size;
    uint32_t     kind;
    bool         read_only;
};

// A context's address space: the ranges mapped in it, checked at each map and unmap
struct fake_space : gpu::space {
    gpu::context* core = nullptr;
    fake_range    ranges[FAKE_RANGES_MAX] = {};
    uint32_t      range_count = 0;
    bool          bad_call = false; // a map over a mapped range, or a split with no entry left
    bool          destroyed = false;
};

struct fake_queue : gpu::queue {
    gpu::core_queue* core = nullptr;
    fake_space*      space = nullptr;
    uint32_t         kind = 0;
    uint32_t         runs = 0;
    uint64_t         last_sequence = 0;
    size_t           last_size = 0;
    uint8_t          last_first_byte = 0;
    bool             stop_asked = false;
    bool             destroyed = false;
    rc::reaper::dead_node stop_node = {};
};

// A backend reports a stop later from its own task, never from inside stop_queue
inline rc::reaper::cleanup_result report_stop(fake_queue* q) {
    gpu::queue_stopped(q->core);
    return rc::reaper::DONE;
}

/**
 * A backend in RAM for the kernel tests. The core keeps each device for good, so every test registers a fake of its
 * own, a file-scope object that outlives the test.
 */
class fake_backend final : public gpu::backend {
public:
    // What the fake reports and how it fails, set by each test
    uint32_t   placements = VIDEO_AND_SYSTEM;
    bool       long_name = false; // a name that fills `name` with no terminator
    int32_t    clock_result = gpu::OK; // what `read_timestamp` returns
    uint64_t   clock_ns = 0;
    gpu_memory memory_report = {};
    int32_t    create_result = gpu::OK; // when not OK, what `create_memory` returns without creating
    int32_t    view_result = gpu::OK; // what `view_for_cpu` returns
    uint64_t   view_size = 0; // when nonzero, the size `view_for_cpu` reports instead of the memory's
    uint64_t   view_shift = 0; // bytes `view_for_cpu` adds to the memory's start
    bool       cacheable = true;
    uint32_t   queue_kinds = BOTH_QUEUE_KINDS;
    bool       user_range = true; // false gives the device-id form of map and unmap
    uint64_t   running_bytes_max = 2 * GPU_COMMANDS_BYTES_MAX;
    uint64_t   submit_timeout_ns = FAKE_TIMEOUT_NS;
    int32_t    space_result = gpu::OK; // when not OK, what `create_space` returns
    int32_t    queue_result = gpu::OK; // when not OK, what `create_queue` returns
    int32_t    map_result = gpu::OK; // when not OK, what `map` returns without mapping
    int32_t    check_result = gpu::OK; // what `check_commands` returns
    bool       fail_unmap = false; // an unmap whose invalidate fails, failing the device
    bool       fail_zeroing = false; // a create whose zeroing fails, failing the device
    bool       stops_on_reaper = true; // false leaves each stop to the test's queue_stopped
    bool       create_waits_for_a_stop = false; // `create_queue` spins at most SPIN_TIMEOUT_NS for a stop request
    stop_hook  on_stop = nullptr; // called once, from the next `stop_queue`, with `on_stop_arg`
    void*      on_stop_arg = nullptr;

    // What the core asked of it
    uint32_t               facts_reads = 0;
    uint32_t               create_calls = 0; // refused calls included, and each id given is the count so far
    fake_memory*           last_created = nullptr;
    gpu::space*            last_create_space = nullptr;
    uint64_t               last_backend_id = 0;
    sync::atomic<uint32_t> released_count{0}; // counted on the reaper, after `last_released` is written
    sync::atomic<uint32_t> freed_count{0}; // releases that freed their memory, counted before `released_count`
    gpu::memory*           last_released = nullptr;
    fake_space             spaces[FAKE_SPACES_MAX] = {};
    uint32_t               space_count = 0;
    sync::atomic<uint32_t> spaces_destroyed{0};
    fake_queue             queues[FAKE_QUEUES_MAX] = {};
    uint32_t               queue_count = 0;
    sync::atomic<uint32_t> queues_destroyed{0};
    sync::atomic<uint32_t> stops_asked{0};
    uint32_t               map_calls = 0;
    fake_map_call          last_map = {};
    uint32_t               unmap_calls = 0;
    fake_range             last_unmap = {};
    uint32_t               check_calls = 0;
    size_t                 last_check_size = 0;
    uint8_t                last_check_first_byte = 0;

    // The device it reports to, and its failure, reported once from the reaper
    gpu::device*           dev = nullptr;
    bool                   failed = false;
    rc::reaper::dead_node  fail_node = {};
    sync::atomic<uint32_t> failures_reported{0};

    void read_facts(gpu::device_facts& out) override {
        facts_reads++;
        out.info.backend = GPU_BACKEND_FAKE;
        out.info.vendor_id = FAKE_VENDOR_ID;
        if (long_name) {
            string::memset(out.info.name, 'x', sizeof(out.info.name));
        } else {
            string::memcpy(out.info.name, "fake GPU", sizeof("fake GPU"));
        }

        out.info.video_size = FAKE_VIDEO_SIZE;
        out.info.mappable_video_size = FAKE_VIDEO_SIZE;
        out.info.page_size = pmm::PAGE_SIZE;
        out.info.placements = placements;

        out.info.queue_kinds = queue_kinds;
        out.info.va_start = user_range ? FAKE_VA_START : 0;
        out.info.va_end = user_range ? FAKE_VA_END : 0;
        out.submit_timeout_ns = submit_timeout_ns;
        out.running_bytes_max = running_bytes_max;
    }

    void read_memory(gpu_memory& out) override {
        out = memory_report;
    }

    int32_t read_timestamp(uint64_t& gpu_ns) override {
        gpu_ns = clock_ns;
        return clock_result;
    }

    int32_t create_memory(
        uint64_t size,
        uint32_t placement,
        uint32_t flags,
        gpu::space* s,
        uint64_t backend_id,
        gpu::memory** out,
        uint32_t* out_device_id
    ) override {
        create_calls++;
        last_create_space = s;
        last_backend_id = backend_id;
        if (create_result != gpu::OK) {
            return create_result;
        }

        if (fail_zeroing) {
            fail_device();
            return gpu::ERR_DEVICE;
        }

        auto* mem = heap::ualloc_new<fake_memory>();
        if (!mem) {
            return gpu::ERR_NO_MEMORY;
        }

        int32_t rc = vmm::alloc_contiguous(
            size / pmm::PAGE_SIZE,
            pmm::ZONE_ANY,
            paging::PAGE_KERNEL_RW,
            vmm::ALLOC_ZERO,
            kva::tag::generic,
            mem->kernel_va,
            mem->phys
        );
        if (rc != vmm::OK) {
            heap::ufree_delete(mem);
            return gpu::ERR_NO_MEMORY;
        }

        mem->size = size;
        mem->placement = placement;
        mem->flags = flags;
        last_created = mem;
        *out = mem;
        *out_device_id = create_calls;

        return gpu::OK;
    }

    int32_t view_for_cpu(gpu::memory* mem, gpu::cpu_view& out) override {
        auto* fake = static_cast<fake_memory*>(mem);
        uint64_t size = view_size != 0 ? view_size : fake->size;
        out = {.phys = fake->phys + view_shift, .size = size, .cacheable = cacheable};

        return view_result;
    }

    // A failed fake keeps the memory, as a failed backend keeps what the GPU may still reach
    void release_memory(gpu::memory* mem) override {
        if (!failed) {
            auto* fake = static_cast<fake_memory*>(mem);
            (void)vmm::free(fake->kernel_va);
            heap::ufree_delete(fake);
            freed_count.fetch_add_release(1);
        }

        last_released = mem;
        released_count.fetch_add_release(1);
    }

    int32_t create_space(gpu::context* core, gpu::space** out) override {
        if (space_result != gpu::OK) {
            return space_result;
        }

        if (space_count == FAKE_SPACES_MAX) {
            return gpu::ERR_NO_MEMORY;
        }

        fake_space& made = spaces[space_count++];
        made.core = core;
        *out = &made;

        return gpu::OK;
    }

    void destroy_space(gpu::space* s) override {
        static_cast<fake_space*>(s)->destroyed = true;
        spaces_destroyed.fetch_add_release(1);
    }

    int32_t map(
        gpu::space* s,
        uint64_t va,
        gpu::memory* mem,
        uint64_t offset,
        uint64_t size,
        uint32_t kind,
        bool read_only
    ) override {
        map_calls++;
        last_map = {.mem = mem, .va = va, .offset = offset, .size = size, .kind = kind, .read_only = read_only};
        if (failed) {
            return gpu::ERR_DEVICE;
        }

        if (map_result != gpu::OK) {
            return map_result;
        }

        auto& space = *static_cast<fake_space*>(s);
        fake_range range = {.va = va, .size = user_range ? size : 1};
        for (uint32_t i = 0; i < space.range_count; i++) {
            if (overlap(space.ranges[i], range)) {
                space.bad_call = true;
                return gpu::ERR_REFUSED;
            }
        }

        if (space.range_count == FAKE_RANGES_MAX) {
            return gpu::ERR_NO_MEMORY;
        }

        space.ranges[space.range_count++] = range;

        return gpu::OK;
    }

    int32_t unmap(gpu::space* s, uint64_t va, uint64_t size) override {
        unmap_calls++;
        last_unmap = {.va = va, .size = size};
        cut(*static_cast<fake_space*>(s), {.va = va, .size = user_range ? size : 1});
        if (!fail_unmap) {
            return failed ? gpu::ERR_DEVICE : gpu::OK;
        }

        fail_device();
        return gpu::ERR_DEVICE;
    }

    int32_t create_queue(gpu::space* s, uint32_t kind, gpu::core_queue* core, gpu::queue** out) override {
        if (create_waits_for_a_stop) {
            (void)test_helpers::spin_wait_ge(stops_asked, 1);
        }

        if (queue_result != gpu::OK) {
            return queue_result;
        }

        if (queue_count == FAKE_QUEUES_MAX) {
            return gpu::ERR_NO_MEMORY;
        }

        fake_queue& made = queues[queue_count++];
        made.core = core;
        made.space = static_cast<fake_space*>(s);
        made.kind = kind;
        made.stop_node.init(rc::reaper::reaper_thunk<fake_queue, report_stop, &fake_queue::stop_node>);
        *out = &made;

        return gpu::OK;
    }

    void stop_queue(gpu::queue* q) override {
        auto& fake = *static_cast<fake_queue*>(q);
        fake.stop_asked = true;
        stops_asked.fetch_add_release(1);

        stop_hook hook = on_stop;
        on_stop = nullptr;
        if (hook) {
            hook(on_stop_arg);
        }

        // A failed GPU reports no stop, since its failure ends every queue
        if (stops_on_reaper && !failed) {
            rc::reaper::defer(&fake.stop_node);
        }
    }

    void destroy_queue(gpu::queue* q) override {
        static_cast<fake_queue*>(q)->destroyed = true;
        queues_destroyed.fetch_add_release(1);
    }

    int32_t check_commands(const void* commands, size_t size) override {
        check_calls++;
        last_check_size = size;
        last_check_first_byte = size != 0 ? *static_cast<const uint8_t*>(commands) : 0;

        return check_result;
    }

    void run(gpu::queue* q, uint64_t sequence, const void* commands, size_t size) override {
        auto& fake = *static_cast<fake_queue*>(q);
        fake.runs++;
        fake.last_sequence = sequence;
        fake.last_size = size;
        fake.last_first_byte = size != 0 ? *static_cast<const uint8_t*>(commands) : 0;
    }

    // The `n`th queue the core created on this fake, counting from 0, or null
    fake_queue* queue_at(uint32_t n) {
        return n < queue_count ? &queues[n] : nullptr;
    }

private:
    // Fails the fake for good, as a zeroing or an invalidate past its deadline fails a GPU
    void fail_device() {
        failed = true;
        fail_node.init(rc::reaper::reaper_thunk<fake_backend, report_failure, &fake_backend::fail_node>);
        rc::reaper::defer(&fail_node);
    }

    // A backend reports its failure later from its own task, never from inside a call the core made
    static rc::reaper::cleanup_result report_failure(fake_backend* fake) {
        gpu::device_failed(fake->dev);
        fake->failures_reported.fetch_add_release(1);
        return rc::reaper::DONE;
    }

    static bool overlap(const fake_range& a, const fake_range& b) {
        return a.va < b.va + b.size && b.va < a.va + a.size;
    }

    // Takes `range` out of every mapped range it reaches, splitting one it falls inside
    static void cut(fake_space& space, const fake_range& range) {
        uint64_t end = range.va + range.size;
        for (uint32_t i = 0; i < space.range_count;) {
            fake_range& mapped = space.ranges[i];
            uint64_t mapped_end = mapped.va + mapped.size;
            if (!overlap(mapped, range)) {
                i++;
            } else if (mapped.va >= range.va && mapped_end <= end) {
                mapped = space.ranges[--space.range_count];
            } else if (mapped.va < range.va && mapped_end > end) {
                if (space.range_count == FAKE_RANGES_MAX) {
                    space.bad_call = true;
                    return;
                }

                space.ranges[space.range_count++] = {.va = end, .size = mapped_end - end};
                mapped.size = range.va - mapped.va;
                i++;
            } else if (mapped.va < range.va) {
                mapped.size = range.va - mapped.va;
                i++;
            } else {
                mapped.size = mapped_end - end;
                mapped.va = end;
                i++;
            }
        }
    }
};

// Closes a handle of the calling task, the task whose table every session call uses
inline void close_handle(int32_t handle) {
    resource::close(sched::current(), static_cast<resource::handle_t>(handle));
}

// Argument structures with each reserved and padding field zero. The out handles of buffer_request and restrict_args
// read -1 until a call writes them
inline gpu_buffer_args buffer_request(uint64_t size, uint32_t placement, uint32_t flags) {
    return {.size = size, .placement = placement, .flags = flags, .source = 0, .buffer = -1, .device_id = 0,
            .context = 0, .backend_id = 0};
}

inline gpu_timeline_args timeline_args(int32_t timeline, uint64_t value) {
    return {.timeline = timeline, .flags = 0, .value = value};
}

inline gpu_restrict_args restrict_args(int32_t handle, uint32_t rights) {
    return {.handle = handle, .rights = rights, .restricted = -1, .padding = 0};
}

inline gpu_point point(int32_t timeline, uint64_t value) {
    return {.timeline = timeline, .padding = 0, .value = value};
}

/**
 * One test's GPU: `fake` registered as the next /dev/gpuN, the calling task's handle to a file of its node (closed with
 * the session), and a user page whose start holds each call's argument.
 */
struct session {
    gpu::device*            dev = nullptr;
    resource::handle_t      file = -1;
    char                    path[PATH_BYTES] = {};
    test_helpers::user_page page;

    explicit session(fake_backend& fake) {
        gpu::device* registered = nullptr;
        if (gpu::register_backend(&fake, &registered) != gpu::OK) {
            return;
        }

        fake.dev = registered;

        size_t pos = devfs::append_str(path, PATH_BYTES - 1, 0, "/dev/");
        devfs::append_str(path, PATH_BYTES - 1, pos, registered->name());
        if (resource::open(sched::current(), path, fs::O_RDWR, &file) == resource::OK) {
            dev = registered;
        }
    }

    ~session() {
        if (dev) {
            close_handle(file);
        }
    }

    bool ready() const { return dev && page.ready(); }

    // Sets the page's protection through the mprotect syscall
    int64_t protect_page(uint64_t prot) {
        test_helpers::user_space_scope scope(page.ctx);
        return sys_mprotect(page.addr, pmm::PAGE_SIZE, prot, 0, 0, 0);
    }

    // Issues `cmd` through the ioctl syscall, with `args` copied to the page's start and read back after
    template <typename T>
    int64_t call(uint32_t cmd, T& args) {
        string::memcpy(page.at<T>(ARGS_AT), &args, sizeof(T));
        int64_t rc = call_in_place(cmd);
        string::memcpy(&args, page.at<T>(ARGS_AT), sizeof(T));

        return rc;
    }

    // Issues `cmd` on the argument already at the page's start
    int64_t call_in_place(uint32_t cmd) {
        test_helpers::user_space_scope scope(page.ctx);
        return sys_ioctl(static_cast<uint64_t>(file), cmd, page.addr + ARGS_AT, 0, 0, 0);
    }

    // Each returns the new handle or a negative syscall code
    int32_t new_buffer(uint64_t size, uint32_t placement, uint32_t flags) {
        gpu_buffer_args args = buffer_request(size, placement, flags);
        int64_t rc = call(GPU_BUFFER_CREATE, args);

        return rc == 0 ? args.buffer : static_cast<int32_t>(rc);
    }

    int32_t new_timeline(uint64_t value) {
        gpu_timeline_args args = timeline_args(-1, value);
        int64_t rc = call(GPU_TIMELINE_CREATE, args);

        return rc == 0 ? args.timeline : static_cast<int32_t>(rc);
    }

    int32_t restrict_to(int32_t handle, uint32_t rights) {
        gpu_restrict_args args = restrict_args(handle, rights);
        int64_t rc = call(GPU_HANDLE_RESTRICT, args);

        return rc == 0 ? args.restricted : static_cast<int32_t>(rc);
    }

    // Returns 0 or a negative syscall code
    int64_t signal(int32_t timeline, uint64_t value) {
        gpu_timeline_args args = timeline_args(timeline, value);
        return call(GPU_TIMELINE_SIGNAL, args);
    }

    // QUERY_FAILED when the call fails
    uint64_t query(int32_t timeline) {
        gpu_timeline_args args = timeline_args(timeline, 0);
        return call(GPU_TIMELINE_QUERY, args) == 0 ? args.value : QUERY_FAILED;
    }

    int32_t new_context() {
        gpu_context_args args = {.context = -1, .lost = 0, .fault_va = 0};
        int64_t rc = call(GPU_CONTEXT_CREATE, args);

        return rc == 0 ? args.context : static_cast<int32_t>(rc);
    }

    // The context's GPU_LOST_* reason, or a negative syscall code
    int64_t lost(int32_t context) {
        gpu_context_args args = {.context = context, .lost = 0, .fault_va = 0};
        int64_t rc = call(GPU_CONTEXT_QUERY, args);

        return rc == 0 ? args.lost : rc;
    }

    // The created queue's index or a negative syscall code
    int64_t new_queue(int32_t context, uint32_t kind) {
        gpu_queue_args args = {.context = context, .kind = kind, .queue = ~0u, .padding = 0};
        int64_t rc = call(GPU_QUEUE_CREATE, args);

        return rc == 0 ? args.queue : rc;
    }

    int64_t destroy_queue(int32_t context, uint32_t queue) {
        gpu_queue_args args = {.context = context, .kind = 0, .queue = queue, .padding = 0};
        return call(GPU_QUEUE_DESTROY, args);
    }

    int64_t map(int32_t context, int32_t buffer, uint64_t va, uint64_t offset, uint64_t size, uint32_t flags) {
        gpu_map_args args = {.context = context, .buffer = buffer, .va = va, .offset = offset, .size = size, .kind = 0,
                             .flags = flags};
        return call(GPU_MAP, args);
    }

    int64_t unmap(int32_t context, uint64_t va, uint64_t size) {
        gpu_map_args args = {.context = context, .buffer = 0, .va = va, .offset = 0, .size = size, .kind = 0,
                             .flags = 0};
        return call(GPU_UNMAP, args);
    }

    // Submits `size` bytes whose first is `tag`, with the waits at POINTS_AT and the signals at SIGNALS_AT. A count or
    // size past its limit names only what the page holds, since the call refuses it before reading
    int64_t submit(
        int32_t context,
        uint32_t queue,
        uint8_t tag,
        uint32_t size,
        const gpu_point* waits,
        uint32_t wait_count,
        const gpu_point* signals,
        uint32_t signal_count
    ) {
        for (uint32_t i = 0; waits && i < wait_count; i++) {
            page.at<gpu_point>(POINTS_AT)[i] = waits[i];
        }

        for (uint32_t i = 0; signals && i < signal_count; i++) {
            page.at<gpu_point>(SIGNALS_AT)[i] = signals[i];
        }

        string::memset(page.at<uint8_t>(COMMANDS_AT), tag, size < COMMANDS_ROOM ? size : COMMANDS_ROOM);
        gpu_submit_args args = {.context = context, .queue = queue, .commands = page.addr + COMMANDS_AT,
                                .waits = page.addr + POINTS_AT, .signals = page.addr + SIGNALS_AT,
                                .commands_size = size, .wait_count = wait_count, .signal_count = signal_count,
                                .padding = 0};
        return call(GPU_SUBMIT, args);
    }
};

// Writes a wait for `count` points at the page's start, the points at POINTS_AT
inline gpu_wait_args* write_wait(
    test_helpers::user_page& page,
    const gpu_point* points,
    uint32_t count,
    uint32_t flags,
    uint64_t deadline
) {
    for (uint32_t i = 0; i < count; i++) {
        page.at<gpu_point>(POINTS_AT)[i] = points[i];
    }

    gpu_wait_args* args = page.at<gpu_wait_args>(ARGS_AT);
    *args = {.points = page.addr + POINTS_AT, .deadline_ns = deadline, .count = count, .flags = flags, .reached = 0,
             .padding = 0};

    return args;
}

} // namespace gpu_test

#endif // STELLUX_TESTS_GPU_FAKE_BACKEND_H
