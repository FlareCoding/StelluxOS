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
#include "syscall/syscall_table.h"

namespace gpu_test {

constexpr uint32_t VIDEO_AND_SYSTEM = (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM);
constexpr uint32_t SYSTEM_ONLY = 1u << GPU_PLACEMENT_SYSTEM;
constexpr uint16_t FAKE_VENDOR_ID = 0x1D17;
constexpr uint64_t FAKE_VIDEO_SIZE = 64ull * 1024 * 1024;
constexpr size_t   PATH_BYTES = 32;
constexpr size_t   ARGS_AT = 0;

// One buffer's memory, physically contiguous pages, so its CPU view is one contiguous range
struct fake_memory : gpu::memory {
    uintptr_t        kernel_va;
    pmm::phys_addr_t phys;
    uint64_t         size;
    uint32_t         placement;
    uint32_t         flags;
};

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

    // What the core asked of it
    uint32_t               facts_reads = 0;
    uint32_t               create_calls = 0; // refused calls included, and each id given is the count so far
    fake_memory*           last_created = nullptr;
    sync::atomic<uint32_t> released_count{0}; // counted on the reaper, after `last_released` is written
    gpu::memory*           last_released = nullptr;

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
        gpu::memory** out,
        uint32_t* out_device_id
    ) override {
        create_calls++;
        if (create_result != gpu::OK) {
            return create_result;
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

    void release_memory(gpu::memory* mem) override {
        auto* fake = static_cast<fake_memory*>(mem);
        (void)vmm::free(fake->kernel_va);
        heap::ufree_delete(fake);

        last_released = mem;
        released_count.fetch_add_release(1);
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

inline gpu_restrict_args restrict_args(int32_t handle, uint32_t rights) {
    return {.handle = handle, .rights = rights, .restricted = -1, .padding = 0};
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

    int32_t restrict_to(int32_t handle, uint32_t rights) {
        gpu_restrict_args args = restrict_args(handle, rights);
        int64_t rc = call(GPU_HANDLE_RESTRICT, args);

        return rc == 0 ? args.restricted : static_cast<int32_t>(rc);
    }
};

} // namespace gpu_test

#endif // STELLUX_TESTS_GPU_FAKE_BACKEND_H
