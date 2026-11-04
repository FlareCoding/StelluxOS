#include "gpu/core.h"
#include "clock/clock.h"
#include "common/string.h"
#include "fs/devfs/devfs.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "resource/resource.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/mutex.h"

namespace gpu {

// Room for "gpu", the ten digits of the largest 32-bit number and the terminator
constexpr size_t NODE_NAME_BYTES = 16;

static_assert(GPU_RIGHT_READ == resource::RIGHT_READ && GPU_RIGHT_WRITE == resource::RIGHT_WRITE);
static_assert(resource::RIGHT_MASK == (GPU_RIGHT_READ | GPU_RIGHT_WRITE));

__PRIVILEGED_DATA static sync::mutex g_registration_lock;
__PRIVILEGED_BSS static uint32_t g_next_device_number;
static sync::atomic<bool> g_fail_next_allocation;

device::device(const char* name, backend* b, const device_facts& facts)
    : fs::node(fs::node_type::char_device, nullptr, name), m_backend(b) {
    string::memcpy(&m_facts, &facts, sizeof(m_facts));
}

void* allocate(size_t bytes) {
    if (g_fail_next_allocation.exchange_relaxed(false)) {
        return nullptr;
    }

    return heap::uzalloc(bytes);
}

void __dbg_test_fail_next_allocation() {
    g_fail_next_allocation.store_relaxed(true);
}

static int32_t get_info(device& dev, uint64_t arg) {
    return copy_out(arg, &dev.info(), sizeof(gpu_info));
}

static int32_t get_memory(device& dev, uint64_t arg) {
    gpu_memory report = {};
    dev.get_backend().read_memory(report);

    return copy_out(arg, &report, sizeof(report));
}

static int32_t read_timestamp(device& dev, uint64_t arg) {
    gpu_timestamp stamp = {};
    int32_t rc = dev.get_backend().read_timestamp(stamp.gpu_ns);
    if (rc != OK) {
        return to_fs_error(rc);
    }

    stamp.cpu_ns = clock::now_ns();

    return copy_out(arg, &stamp, sizeof(stamp));
}

// The device a context or buffer belongs to, null for a timeline
static const device* device_of(const resource::resource_object& obj) {
    if (obj.type == resource::resource_type::GPU_CONTEXT) {
        return &static_cast<const context*>(obj.impl)->dev;
    }

    if (obj.type == resource::resource_type::GPU_BUFFER) {
        return &static_cast<const buffer*>(obj.impl)->owner();
    }

    return nullptr;
}

static int32_t restrict_handle(device& dev, uint64_t arg) {
    gpu_restrict_args args = {};
    if (copy_in(&args, arg, sizeof(args)) != fs::OK) {
        return fs::ERR_INVAL;
    }

    if (args.padding != 0 || args.rights == 0) {
        return fs::ERR_INVAL;
    }

    sched::task* task = sched::current();
    resource::resource_object* obj = nullptr;
    uint32_t held = 0;
    if (resource::get_handle_object(task->handles, args.handle, 0, &obj, nullptr, &held) != resource::HANDLE_OK) {
        return fs::ERR_BADF;
    }

    bool is_context = obj->type == resource::resource_type::GPU_CONTEXT;
    bool is_buffer = obj->type == resource::resource_type::GPU_BUFFER;
    bool is_timeline = obj->type == resource::resource_type::GPU_TIMELINE;
    bool narrows = (args.rights & ~held) == 0;

    int32_t rc = fs::OK;
    if (!is_context && !is_buffer && !is_timeline) {
        rc = fs::ERR_BADF;
    } else if (!is_timeline && device_of(*obj) != &dev) {
        rc = fs::ERR_INVAL;
    } else if (!narrows) {
        rc = fs::ERR_INVAL;
    } else {
        rc = install_handle(obj, args.rights, arg + __builtin_offsetof(gpu_restrict_args, restricted));
    }

    resource::resource_release(obj);

    return rc;
}

int32_t device::ioctl(fs::file*, uint32_t cmd, uint64_t arg) {
    switch (cmd) {
    case GPU_GET_INFO:        return get_info(*this, arg);
    case GPU_GET_MEMORY:      return get_memory(*this, arg);
    case GPU_READ_TIMESTAMP:  return read_timestamp(*this, arg);
    case GPU_CONTEXT_CREATE:  return create_context(*this, arg);
    case GPU_CONTEXT_QUERY:   return query_context(*this, arg);
    case GPU_QUEUE_CREATE:    return create_queue(*this, arg);
    case GPU_QUEUE_DESTROY:   return destroy_queue(*this, arg);
    case GPU_BUFFER_CREATE:   return create_buffer(*this, arg);
    case GPU_MAP:             return map_buffer(*this, arg);
    case GPU_UNMAP:           return unmap_range(*this, arg);
    case GPU_TIMELINE_CREATE: return create_timeline(arg);
    case GPU_TIMELINE_SIGNAL: return signal_timeline(arg);
    case GPU_TIMELINE_QUERY:  return query_timeline(arg);
    case GPU_TIMELINE_WAIT:   return wait_timelines(arg);
    case GPU_HANDLE_RESTRICT: return restrict_handle(*this, arg);
    default:                  return fs::ERR_NOSYS;
    }
}

int32_t to_fs_error(int32_t rc) {
    switch (rc) {
    case OK:              return fs::OK;
    case ERR_NO_MEMORY:   return fs::ERR_NOMEM;
    case ERR_UNSUPPORTED: return fs::ERR_NOSYS;
    case ERR_DEVICE:      return fs::ERR_IO;
    case ERR_REFUSED:     return fs::ERR_INVAL;
    default:              return fs::ERR_IO;
    }
}

__PRIVILEGED_CODE int32_t copy_in(void* dst, uint64_t src, size_t bytes) {
    bool copied = mm::uaccess::copy_from_user(dst, reinterpret_cast<const void*>(src), bytes) == mm::uaccess::OK;
    return copied ? fs::OK : fs::ERR_INVAL;
}

__PRIVILEGED_CODE int32_t copy_out(uint64_t dst, const void* src, size_t bytes) {
    bool copied = mm::uaccess::copy_to_user(reinterpret_cast<void*>(dst), src, bytes) == mm::uaccess::OK;
    return copied ? fs::OK : fs::ERR_INVAL;
}

__PRIVILEGED_CODE int32_t lookup_object(
    int32_t handle,
    resource::resource_type type,
    uint32_t rights,
    resource::resource_object** out
) {
    resource::resource_object* obj = nullptr;
    if (resource::get_handle_object(sched::current()->handles, handle, rights, &obj) != resource::HANDLE_OK) {
        return fs::ERR_BADF;
    }

    if (obj->type != type) {
        resource::resource_release(obj);
        return fs::ERR_BADF;
    }

    *out = obj;

    return fs::OK;
}

__PRIVILEGED_CODE int32_t lookup_on_device(
    device& dev,
    int32_t handle,
    resource::resource_type type,
    uint32_t rights,
    resource::resource_object** out
) {
    int32_t rc = lookup_object(handle, type, rights, out);
    if (rc == fs::OK && device_of(**out) != &dev) {
        resource::resource_release(*out);
        return fs::ERR_INVAL;
    }

    return rc;
}

__PRIVILEGED_CODE int32_t install_handle(resource::resource_object* obj, uint32_t rights, uint64_t user_handle) {
    sched::task* task = sched::current();
    resource::handle_t handle = -1;
    if (resource::alloc_task_handle(task, obj, obj->type, rights, &handle) != resource::HANDLE_OK) {
        return fs::ERR_NOMEM;
    }

    if (copy_out(user_handle, &handle, sizeof(handle)) != fs::OK) {
        resource::close(task, handle);
        return fs::ERR_INVAL;
    }

    return fs::OK;
}

__PRIVILEGED_CODE void init() {
    g_registration_lock.init();
}

__PRIVILEGED_CODE static device* publish(backend* b, const device_facts& facts) {
    char name[NODE_NAME_BYTES] = {};
    size_t pos = devfs::append_str(name, NODE_NAME_BYTES - 1, 0, "gpu");
    devfs::append_u64(name, NODE_NAME_BYTES - 1, pos, g_next_device_number);

    device* dev = heap::kalloc_new<device>(name, b, facts);
    if (!dev) {
        return nullptr;
    }

    if (devfs::add_char_device(name, dev) != devfs::OK) {
        if (dev->release()) {
            fs::node::ref_destroy(dev);
        }

        return nullptr;
    }

    // The number advances only once the node is published, so a failed registration leaves no gap in the numbers
    g_next_device_number++;

    return dev;
}

__PRIVILEGED_CODE int32_t register_backend(backend* b, device** out) {
    device_facts facts = {};
    b->read_facts(facts);
    facts.info.name[sizeof(facts.info.name) - 1] = '\0';

    sync::mutex_lock(g_registration_lock);
    device* dev = publish(b, facts);
    sync::mutex_unlock(g_registration_lock);

    if (!dev) {
        return ERR_NO_MEMORY;
    }

    *out = dev;

    return OK;
}

} // namespace gpu
