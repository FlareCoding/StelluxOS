#include "drivers/graphics/virtio_gpu_backend.h"
#include "drivers/graphics/virtio_gpu.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "pci/pci.h"

namespace drivers {

using namespace virtio;

constexpr char DEVICE_NAME[] = "virtio-gpu";

// The facts hold a bit per capset id below this, so the backend reads no more capsets than this
constexpr uint32_t CAPSET_IDS = 64;

// Guest blobs and their page lists may use any address
constexpr uint8_t GUEST_PAGE_ADDRESS_BITS = 64;

// The most bytes a page list entry's 32-bit length covers in whole pages
constexpr uint64_t RUN_BYTES_MAX = 0xFFFFF000;

// The most entries QEMU accepts in a guest blob's page list
constexpr size_t PAGE_LIST_ENTRIES_MAX = 16384;

// Four of the largest command lists
constexpr uint64_t RUNNING_BYTES_MAX = 4ULL * GPU_COMMANDS_BYTES_MAX;

// The header's context id is 32 bits
constexpr uint32_t LAST_CONTEXT_ID = 0xFFFFFFFF;

constexpr uint64_t BYTES_PER_MIB = 1ULL << 20;

// Venus retires a ring 0 fence once it decoded the commands before it, and rings past 0 belong to VkQueues its own
// commands bind, so every list's fence goes on ring 0, where a context's fences end in order
constexpr uint8_t SUBMIT_RING_INDEX = 0;

// The bytes the core may have running on a queue, each list after its SUBMIT_3D header
constexpr uint32_t SEND_RING_BYTES =
    static_cast<uint32_t>(RUNNING_BYTES_MAX + GPU_QUEUE_PENDING_MAX * sizeof(virtio_gpu_cmd_submit));

constexpr size_t QUEUE_PAGES =
    (SEND_RING_BYTES + GPU_QUEUE_PENDING_MAX * sizeof(virtio_gpu_ctrl_hdr) + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

// The descriptors a full batch leaves free on a control queue of `VIRTQ_MAX_SIZE`, the size registration asks for
constexpr uint32_t CHAIN_DESCRIPTORS_MAX = 128;

// A submission chain's descriptors, the header with its list and the response, and one more when the list wraps
constexpr uint8_t SUBMIT_DESCRIPTORS = 2;

static bool is_before(const region_range* a, const region_range* b) {
    return a->offset < b->offset;
}

void region_allocator::init(uint64_t size) {
    m_ranges.init();
    m_size = size;
}

bool region_allocator::allocate(uint64_t size, region_range& out) {
    uint64_t start = 0;
    for (region_range& taken : m_ranges) {
        if (taken.offset - start >= size) {
            break;
        }

        start = taken.offset + taken.size;
    }

    if (m_size - start < size) {
        return false;
    }

    out.offset = start;
    out.size = size;
    m_ranges.insert_sorted(&out, is_before);

    return true;
}

void region_allocator::release(region_range& range) {
    m_ranges.remove(&range);
}

void context_id_allocator::init(uint32_t last) {
    m_held.init();
    m_next = 1;
    m_last = last;
}

bool context_id_allocator::take(context_id& out) {
    // Each held id blocks at most one of these consecutive candidates, so one of them is free unless every id is held
    size_t candidates = m_held.size() + 1;
    for (size_t i = 0; i < candidates; i++) {
        uint32_t candidate = m_next;
        m_next = m_next == m_last ? 1 : m_next + 1;

        if (!is_held(candidate)) {
            out.value = candidate;
            m_held.push_back(&out);
            return true;
        }
    }

    return false;
}

void context_id_allocator::give_back(context_id& id) {
    m_held.remove(&id);
}

void send_ring::init(uint32_t capacity) {
    m_capacity = capacity;
    m_oldest = 0;
    m_used = 0;
}

bool send_ring::has_room(uint32_t bytes) const {
    return bytes <= m_capacity - m_used;
}

bool send_ring::wraps(uint32_t bytes) const {
    uint32_t next = (m_oldest + m_used) % m_capacity;
    return bytes > m_capacity - next;
}

ring_placement send_ring::place(uint32_t bytes) {
    uint32_t next = (m_oldest + m_used) % m_capacity;
    uint32_t first_bytes = wraps(bytes) ? m_capacity - next : bytes;
    m_used += bytes;

    return {next, first_bytes, bytes - first_bytes};
}

void send_ring::free_oldest(uint32_t bytes) {
    m_oldest = (m_oldest + bytes) % m_capacity;
    m_used -= bytes;
}

void write_placed(
    uint8_t* ring,
    const ring_placement& placement,
    uint32_t at,
    const void* source,
    uint32_t bytes
) {
    auto* from = static_cast<const uint8_t*>(source);
    uint32_t first_bytes = 0;
    if (at < placement.first_bytes) {
        first_bytes = placement.first_bytes - at < bytes ? placement.first_bytes - at : bytes;
        string::memcpy(ring + placement.offset + at, from, first_bytes);
    }

    if (first_bytes < bytes) {
        string::memcpy(ring + (at + first_bytes - placement.first_bytes), from + first_bytes, bytes - first_bytes);
    }
}

uint64_t free_returned_lists(
    virtio_gpu_submission* slots,
    send_ring& ring,
    uint64_t last_reported,
    uint64_t last_copied,
    bool& out_is_refused
) {
    uint64_t finished = last_reported;
    out_is_refused = false;
    while (finished < last_copied) {
        virtio_gpu_submission& sub = slots[(finished + 1) % GPU_QUEUE_PENDING_MAX];
        if (!sub.has_returned) {
            break;
        }

        // Lists after a refused one stay unfinished, so the space's loss ends them failed
        if (!sub.is_answered_ok) {
            out_is_refused = true;
            break;
        }

        ring.free_oldest(sub.ring_bytes);
        sub.has_returned = false;
        finished++;
    }

    return finished;
}

bool context_id_allocator::is_held(uint32_t value) {
    for (context_id& held : m_held) {
        if (held.value == value) {
            return true;
        }
    }

    return false;
}

size_t list_page_runs(
    const pmm::phys_addr_t* pages,
    size_t count,
    virtio_gpu_mem_entry* entries,
    size_t capacity
) {
    size_t runs = 0;
    for (size_t i = 0; i < count; i++) {
        if (runs > 0) {
            virtio_gpu_mem_entry& last = entries[runs - 1];
            bool is_contiguous = last.addr + last.length == pages[i];
            bool has_room = last.length <= RUN_BYTES_MAX - pmm::PAGE_SIZE;
            if (is_contiguous && has_room) {
                last.length += pmm::PAGE_SIZE;
                continue;
            }
        }

        if (runs == capacity) {
            return 0;
        }

        entries[runs++] = {pages[i], pmm::PAGE_SIZE, 0};
    }

    return runs;
}

// On the reaper: frees a wrapped guest blob's record, whose destructor releases the pin
static rc::reaper::cleanup_result release_pin(virtio_gpu_memory* mem) {
    heap::ufree_delete(mem);
    return rc::reaper::DONE;
}

virtio_gpu_backend::virtio_gpu_backend(virtio_gpu_driver& driver) : m_driver(driver) {
    m_region.init(0);
    m_context_ids.init(LAST_CONTEXT_ID);
    m_queues.init();

    m_releases.init();
    m_destroys.init();
    m_queue_destroys.init();
    m_pending.init();
    m_stops.init();
}

void virtio_gpu_backend::register_with_core() {
    uint64_t needed_features = VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_RESOURCE_BLOB | VIRTIO_GPU_F_CONTEXT_INIT;
    if ((m_driver.m_features & needed_features) != needed_features) {
        return;
    }

    bool has_region = m_driver.find_shared_memory(VIRTIO_GPU_SHM_ID_HOST_VISIBLE, &m_region_phys, &m_region_size);
    if (!has_region) {
        return;
    }

    int32_t capsets_rc = read_capsets();
    if (capsets_rc == gpu::ERR_NO_MEMORY) {
        log::warn("virtio-gpu: the host refused to list its capsets, so the 3D path stays unregistered");
    }

    if (capsets_rc != gpu::OK) {
        return;
    }

    if ((m_capsets & (1ULL << VIRTIO_GPU_CAPSET_VENUS)) == 0) {
        return;
    }

    // Submission chains take the descriptors a full batch leaves, and between batches only an interrupt tells the
    // task one returned
    static_assert(
        virtio_gpu_driver::MAX_BATCH_DESCRIPTORS + CHAIN_DESCRIPTORS_MAX <= VIRTQ_MAX_SIZE,
        "a full batch and the submission chains fit the largest control queue"
    );
    uint32_t descriptors_needed = virtio_gpu_driver::MAX_BATCH_DESCRIPTORS + CHAIN_DESCRIPTORS_MAX;
    if (m_driver.m_control_queue.size() < descriptors_needed) {
        log::warn(
            "virtio-gpu: the control queue holds fewer than %u descriptors, so the 3D path stays unregistered",
            descriptors_needed
        );
        return;
    }

    if (!m_driver.m_has_completion_interrupt) {
        log::warn("virtio-gpu: no completion interrupt, so the 3D path stays unregistered");
        return;
    }

    // A process maps the region's ranges by whole pages, which would reach memory before the region otherwise
    if (m_region_phys % pmm::PAGE_SIZE != 0) {
        log::warn("virtio-gpu: the host-visible region starts off a page boundary, so the 3D path stays unregistered");
        return;
    }

    if (m_region_size < pmm::PAGE_SIZE) {
        log::warn("virtio-gpu: the host-visible region is smaller than a page, so the 3D path stays unregistered");
        return;
    }

    probe_answer probe = probe_guest_blobs();
    if (probe == probe_answer::broken) {
        return;
    }

    m_offers_system = probe == probe_answer::created;
    m_region.init(m_region_size);
    RUN_ELEVATED(m_pci_revision = m_driver.m_dev->config_read8(pci::CFG_REVISION));

    int32_t rc = gpu::OK;
    RUN_ELEVATED(rc = gpu::register_backend(this, &m_device));
    if (rc != gpu::OK) {
        log::error("virtio-gpu: the GPU core had no memory to publish the 3D path, so it stays unregistered");
        return;
    }

    log::info(
        "virtio-gpu: registered the 3D path with the GPU core, capset mask 0x%lx, %lu MiB host-visible, guest blobs %s",
        m_capsets,
        m_region_size / BYTES_PER_MIB,
        m_offers_system ? "offered" : "not offered"
    );
}

bool virtio_gpu_backend::has_task_work() {
    bool has_unreported_failure = m_is_failed && !m_is_failure_reported;
    bool has_queued = !m_releases.empty() || !m_destroys.empty() || !m_queue_destroys.empty();

    uint64_t deadline_ns = 0;
    bool has_passed_deadline = find_stop_deadline(deadline_ns) && clock::now_ns() >= deadline_ns;

    return has_queued || m_has_submission_work || has_unreported_failure || has_passed_deadline;
}

void virtio_gpu_backend::serve_task_work() {
    virtio_gpu_memory* released = nullptr;
    virtio_gpu_space* destroyed = nullptr;
    virtio_gpu_queue* queue_destroyed = nullptr;
    bool has_submission_work = false;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        released = m_releases.pop_front();
        destroyed = m_destroys.pop_front();
        queue_destroyed = m_queue_destroys.pop_front();

        has_submission_work = m_has_submission_work;
        m_has_submission_work = false;
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    if (has_submission_work) {
        serve_submissions();
    }

    if (released) {
        release_on_task(released);
    }

    if (destroyed) {
        destroy_on_task(destroyed);
    }

    if (queue_destroyed) {
        destroy_queue_on_task(queue_destroyed);
    }

    check_stop_deadlines();
    report_failure();
}

bool virtio_gpu_backend::find_stop_deadline(uint64_t& out_ns) {
    if (m_is_failed) {
        return false;
    }

    // Only a stop with a chain still out can fail its deadline, since the task's next pass reports the others
    bool has_deadline = false;
    for (virtio_gpu_queue& queue : m_stops) {
        bool is_earliest = !has_deadline || queue.stop_deadline_ns < out_ns;
        if (queue.chains_out != 0 && is_earliest) {
            out_ns = queue.stop_deadline_ns;
            has_deadline = true;
        }
    }

    return has_deadline;
}

void virtio_gpu_backend::check_stop_deadlines() {
    bool has_passed = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        uint64_t deadline_ns = 0;
        has_passed = find_stop_deadline(deadline_ns) && clock::now_ns() >= deadline_ns;
        m_is_failed = m_is_failed || has_passed;
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    if (has_passed) {
        log::error("virtio-gpu: a venus queue's lists outlived its stop's deadline, so the 3D path failed");
        report_failure();
    }
}

void virtio_gpu_backend::report_failure() {
    bool reports_failure = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        reports_failure = m_is_failed && !m_is_failure_reported;
        m_is_failure_reported = m_is_failed;
        if (reports_failure) {
            while (!m_pending.empty()) {
                m_pending.pop_front();
            }

            while (!m_stops.empty()) {
                m_stops.pop_front();
            }
        }

        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    // A registration that broke the device left no device to report to
    if (reports_failure && m_device) {
        RUN_ELEVATED(gpu::device_failed(m_device));
    }
}

bool virtio_gpu_backend::record_returned_chain(uint16_t head) {
    virtio_gpu_submission* sub = m_chains[head];
    if (!sub) {
        return false;
    }

    virtio_gpu_queue& queue = *sub->queue;
    uint32_t slot = static_cast<uint32_t>(sub->sequence % GPU_QUEUE_PENDING_MAX);
    auto* response = reinterpret_cast<const virtio_gpu_ctrl_hdr*>(
        queue.memory.virt + SEND_RING_BYTES + slot * sizeof(virtio_gpu_ctrl_hdr)
    );

    m_chains[head] = nullptr;
    m_chain_descriptors -= sub->descriptors;
    sub->is_answered_ok = response->type == VIRTIO_GPU_RESP_OK_NODATA;
    sub->has_returned = true;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        queue.chains_out--;
        m_has_submission_work = true;
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    return true;
}

void virtio_gpu_backend::wake_task() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_has_submission_work = true;
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

__PRIVILEGED_CODE void virtio_gpu_backend::wake_for_completion() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
    if (m_has_chains_out) {
        m_has_submission_work = true;
        sync::wake_one(m_driver.m_work_wq);
    }

    sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
}

void virtio_gpu_backend::read_facts(gpu::device_facts& out) {
    out.info.backend = GPU_BACKEND_VIRTIO;
    out.info.queue_kinds = 1u << GPU_QUEUE_GRAPHICS;
    string::memcpy(out.info.name, DEVICE_NAME, sizeof(DEVICE_NAME));

    out.info.vendor_id = m_driver.m_dev->vendor_id();
    out.info.device_id = m_driver.m_dev->device_id();
    out.info.pci_bus = m_driver.m_dev->bus();
    out.info.pci_slot = m_driver.m_dev->slot();
    out.info.pci_function = m_driver.m_dev->func();
    out.info.pci_revision = m_pci_revision;

    out.info.page_size = pmm::PAGE_SIZE;
    out.info.placements = 1u << GPU_PLACEMENT_CONTEXT;
    if (m_offers_system) {
        out.info.placements |= 1u << GPU_PLACEMENT_SYSTEM;
    }

    out.info.virtio.capsets = m_capsets;
    out.info.virtio.host_visible_size = m_region_size - m_region_size % pmm::PAGE_SIZE;

    out.submit_timeout_ns = GPU_SUBMIT_TIMEOUT_NS;
    out.running_bytes_max = RUNNING_BYTES_MAX;
}

// The host reports no size for the memory behind its blobs
void virtio_gpu_backend::read_memory(gpu_memory& out) {
    out.video_free = 0;
    out.mappable_video_free = 0;
}

// virtio-gpu offers no clock of the host's GPU
int32_t virtio_gpu_backend::read_timestamp(uint64_t&) {
    return has_failed() ? gpu::ERR_DEVICE : gpu::ERR_UNSUPPORTED;
}

int32_t virtio_gpu_backend::create_memory(
    uint64_t size,
    uint32_t placement,
    uint32_t flags,
    gpu::space* s,
    uint64_t backend_id,
    gpu::memory** out,
    uint32_t* out_device_id
) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* mem = heap::ualloc_new<virtio_gpu_memory>();
    if (!mem) {
        return gpu::ERR_NO_MEMORY;
    }

    mem->placement = placement;

    int32_t rc = gpu::OK;
    if (placement == GPU_PLACEMENT_SYSTEM) {
        rc = create_system_memory(size, *mem);
    } else {
        rc = create_context_memory(size, flags, *static_cast<virtio_gpu_space*>(s), backend_id, *mem);
    }

    // After `gpu::ERR_DEVICE` the host may hold what the record names, which then stays for good
    if (rc == gpu::ERR_NO_MEMORY) {
        heap::ufree_delete(mem);
    }

    if (rc == gpu::OK) {
        *out = mem;
        *out_device_id = mem->resource_id;
    }

    return rc;
}

int32_t virtio_gpu_backend::wrap_pages(
    const pmm::phys_addr_t* pages,
    size_t count,
    mm::shmem_mapping&& pin,
    gpu::memory** out,
    uint32_t* out_device_id
) {
    auto* mem = heap::ualloc_new<virtio_gpu_memory>();
    if (!mem) {
        return gpu::ERR_NO_MEMORY;
    }

    // The GPU core counts the pin as taken when a wrap returns `gpu::ERR_DEVICE`, so the record keeps it for good
    if (has_failed()) {
        mem->pin = static_cast<mm::shmem_mapping&&>(pin);
        return gpu::ERR_DEVICE;
    }

    size_t capacity = count < PAGE_LIST_ENTRIES_MAX ? count : PAGE_LIST_ENTRIES_MAX;
    size_t list_pages = (capacity * sizeof(virtio_gpu_mem_entry) + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

    dma::buffer list = {};
    if (allocate_pages(list_pages, paging::PAGE_USER, list) != gpu::OK) {
        heap::ufree_delete(mem);
        return gpu::ERR_NO_MEMORY;
    }

    auto* entries = reinterpret_cast<virtio_gpu_mem_entry*>(list.virt);
    size_t runs = list_page_runs(pages, count, entries, capacity);
    if (runs == 0) {
        RUN_ELEVATED(dma::free_pages(list));
        heap::ufree_delete(mem);
        return gpu::ERR_NO_MEMORY;
    }

    mem->placement = GPU_PLACEMENT_SYSTEM;
    int32_t rc = create_guest_blob(*mem, list, static_cast<uint32_t>(runs), count * pmm::PAGE_SIZE);
    if (rc == gpu::ERR_NO_MEMORY) {
        heap::ufree_delete(mem);
        return rc;
    }

    // The host may hold the pages from the create on, so the pin stays with them, for good after a failure
    mem->pin = static_cast<mm::shmem_mapping&&>(pin);
    if (rc != gpu::OK) {
        return rc;
    }

    *out = mem;
    *out_device_id = mem->resource_id;

    return gpu::OK;
}

int32_t virtio_gpu_backend::view_for_cpu(gpu::memory* mem, gpu::cpu_view& out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* memory = static_cast<virtio_gpu_memory*>(mem);
    if (memory->placement == GPU_PLACEMENT_SYSTEM) {
        out = {.phys = memory->pages.phys, .size = memory->pages.size, .cacheable = memory->pages.cached};
    } else {
        out = {
            .phys = m_region_phys + memory->range.offset,
            .size = memory->range.size,
            .cacheable = memory->is_cacheable,
        };
    }

    return gpu::OK;
}

void virtio_gpu_backend::release_memory(gpu::memory* mem) {
    auto* memory = static_cast<virtio_gpu_memory*>(mem);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_releases.push_back(memory);
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

int32_t virtio_gpu_backend::create_space(gpu::context* core, gpu::space** out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* space = heap::ualloc_new<virtio_gpu_space>();
    if (!space) {
        return gpu::ERR_NO_MEMORY;
    }

    space->core = core;

    virtio_gpu_ctx_create create = {};
    create.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    create.context_init = VIRTIO_GPU_CAPSET_VENUS;

    int32_t rc = gpu::ERR_NO_MEMORY;
    m_driver.lock_batch();
    if (m_context_ids.take(space->id)) {
        create.hdr.ctx_id = space->id.value;
        rc = send(create);
        if (rc == gpu::ERR_NO_MEMORY) {
            m_context_ids.give_back(space->id);
        }
    }

    m_driver.unlock_batch();

    // The host may have made a context of a create it left unanswered, whose id then stays held for good
    if (rc == gpu::ERR_NO_MEMORY) {
        heap::ufree_delete(space);
    }

    if (rc == gpu::OK) {
        *out = space;
    }

    return rc;
}

void virtio_gpu_backend::destroy_space(gpu::space* s) {
    auto* space = static_cast<virtio_gpu_space*>(s);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_destroys.push_back(space);
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

int32_t virtio_gpu_backend::map(
    gpu::space* s,
    uint64_t,
    gpu::memory* mem,
    uint64_t,
    uint64_t,
    uint32_t,
    bool read_only
) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    // The host attaches a resource with no access mode, so a context can write whatever it reaches
    if (read_only) {
        return gpu::ERR_REFUSED;
    }

    virtio_gpu_ctx_resource attach = {};
    attach.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    attach.hdr.ctx_id = static_cast<virtio_gpu_space*>(s)->id.value;
    attach.resource_id = static_cast<virtio_gpu_memory*>(mem)->resource_id;

    m_driver.lock_batch();
    int32_t rc = send(attach);
    m_driver.unlock_batch();

    return rc;
}

int32_t virtio_gpu_backend::unmap(gpu::space* s, uint64_t va, uint64_t) {
    virtio_gpu_ctx_resource detach = {};
    detach.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    detach.hdr.ctx_id = static_cast<virtio_gpu_space*>(s)->id.value;
    detach.resource_id = static_cast<uint32_t>(va);

    m_driver.lock_batch();
    int32_t rc = send(detach);

    // The context may still reach a resource the host refused to detach, which only a failed backend makes safe
    if (rc == gpu::ERR_NO_MEMORY) {
        log::error(
            "virtio-gpu: the host refused to detach resource %u from context %u, so the 3D path failed for good",
            detach.resource_id,
            detach.hdr.ctx_id
        );
        fail();
        rc = gpu::ERR_DEVICE;
    }

    m_driver.unlock_batch();

    return rc;
}

// The facts offer graphics queues only, which the core checks before creating one
int32_t virtio_gpu_backend::create_queue(gpu::space* s, uint32_t, gpu::core_queue* core, gpu::queue** out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* queue = heap::ualloc_new<virtio_gpu_queue>();
    if (!queue) {
        return gpu::ERR_NO_MEMORY;
    }

    if (allocate_pages(QUEUE_PAGES, paging::PAGE_USER, queue->memory) != gpu::OK) {
        heap::ufree_delete(queue);
        return gpu::ERR_NO_MEMORY;
    }

    queue->core = core;
    queue->space = static_cast<virtio_gpu_space*>(s);
    queue->ring.init(SEND_RING_BYTES);

    m_driver.lock_batch();
    bool is_sendable = can_send();
    if (is_sendable) {
        m_queues.push_back(queue);
    }

    m_driver.unlock_batch();

    if (!is_sendable) {
        RUN_ELEVATED(dma::free_pages(queue->memory));
        heap::ufree_delete(queue);
        return gpu::ERR_DEVICE;
    }

    *out = queue;

    return gpu::OK;
}

void virtio_gpu_backend::stop_queue(gpu::queue* q) {
    auto* queue = static_cast<virtio_gpu_queue*>(q);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        if (!m_is_failed && !queue->is_stopping) {
            queue->is_stopping = true;
            queue->stop_deadline_ns = clock::now_ns() + GPU_SUBMIT_TIMEOUT_NS;
            m_stops.push_back(queue);
            m_has_submission_work = true;
            sync::wake_one(m_driver.m_work_wq);
        }

        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

void virtio_gpu_backend::destroy_queue(gpu::queue* q) {
    auto* queue = static_cast<virtio_gpu_queue*>(q);

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_queue_destroys.push_back(queue);
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

// The host reads and checks each list, so the backend refuses none
int32_t virtio_gpu_backend::check_commands(const void*, size_t) {
    return has_failed() ? gpu::ERR_DEVICE : gpu::OK;
}

void virtio_gpu_backend::run(gpu::queue* q, uint64_t sequence, const void* commands, size_t size) {
    auto* queue = static_cast<virtio_gpu_queue*>(q);
    virtio_gpu_submission& sub = queue->slots[sequence % GPU_QUEUE_PENDING_MAX];

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        if (!m_is_failed) {
            sub.queue = queue;
            sub.sequence = sequence;
            sub.commands = commands;
            sub.size = static_cast<uint32_t>(size);

            m_pending.push_back(&sub);
            m_has_submission_work = true;
            sync::wake_one(m_driver.m_work_wq);
        }

        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

int32_t virtio_gpu_backend::read_capsets() {
    volatile virtio_gpu_config* config = m_driver.device_config<virtio_gpu_config>();
    uint32_t listed = config ? config->num_capsets : 0;
    uint32_t count = listed < CAPSET_IDS ? listed : CAPSET_IDS;

    int32_t rc = gpu::OK;
    m_driver.lock_batch();
    for (uint32_t index = 0; index < count; index++) {
        virtio_gpu_get_capset_info request = {};
        request.hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        request.capset_index = index;

        uint32_t response_offset = 0;
        uint32_t response_size = sizeof(virtio_gpu_resp_capset_info);
        rc = send_command(&request, sizeof(request), response_size, &response_offset, nullptr);
        if (rc != gpu::OK) {
            break;
        }

        uintptr_t response = m_driver.m_command_page.virt + response_offset;
        auto* info = reinterpret_cast<const virtio_gpu_resp_capset_info*>(response);
        if (info->capset_id < CAPSET_IDS) {
            m_capsets |= 1ULL << info->capset_id;
        }
    }

    m_driver.unlock_batch();

    return rc;
}

virtio_gpu_backend::probe_answer virtio_gpu_backend::probe_guest_blobs() {
    auto* probe = heap::ualloc_new<virtio_gpu_memory>();
    if (!probe) {
        log::warn("virtio-gpu: no memory for the guest blob probe, so guest blobs are not offered");
        return probe_answer::refused;
    }

    dma::buffer list = {};
    bool has_page = allocate_pages(1, 0, probe->pages) == gpu::OK;
    bool has_list = has_page && allocate_pages(1, paging::PAGE_USER, list) == gpu::OK;
    if (!has_list) {
        RUN_ELEVATED(dma::free_pages(probe->pages));
        heap::ufree_delete(probe);
        log::warn("virtio-gpu: no memory for the guest blob probe, so guest blobs are not offered");
        return probe_answer::refused;
    }

    probe->placement = GPU_PLACEMENT_SYSTEM;
    *reinterpret_cast<virtio_gpu_mem_entry*>(list.virt) = {probe->pages.phys, pmm::PAGE_SIZE, 0};

    int32_t rc = create_guest_blob(*probe, list, 1, pmm::PAGE_SIZE);
    if (rc == gpu::ERR_DEVICE) {
        return probe_answer::broken;
    }

    // Every error answer to a create leaves no resource in the host, so no release follows
    if (rc != gpu::OK) {
        RUN_ELEVATED(dma::free_pages(probe->pages));
        heap::ufree_delete(probe);
        return probe_answer::refused;
    }

    release_on_task(probe);
    return has_failed() ? probe_answer::broken : probe_answer::created;
}

int32_t virtio_gpu_backend::allocate_pages(size_t count, paging::page_flags_t access, dma::buffer& out) {
    int32_t rc = dma::OK;
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            m_driver.dma_profile(GUEST_PAGE_ADDRESS_BITS),
            count,
            dma::caching::cached_when_coherent,
            access,
            out
        )
    );

    return rc == dma::OK ? gpu::OK : gpu::ERR_NO_MEMORY;
}

int32_t virtio_gpu_backend::create_guest_blob(
    virtio_gpu_memory& mem,
    dma::buffer& list,
    uint32_t count,
    uint64_t size
) {
    virtio_gpu_resource_create_blob create = {};
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    create.blob_mem = VIRTIO_GPU_BLOB_MEM_GUEST;
    create.nr_entries = count;
    create.size = size;

    chain_buffer list_buffer = {list.phys, static_cast<uint32_t>(count * sizeof(virtio_gpu_mem_entry)), 0};

    m_driver.lock_batch();
    mem.resource_id = m_driver.m_next_resource_id++;
    create.resource_id = mem.resource_id;
    int32_t rc = send_command(&create, sizeof(create), sizeof(virtio_gpu_ctrl_hdr), nullptr, &list_buffer);
    m_driver.unlock_batch();

    // The device may still read the list of a create it never answered
    if (rc != gpu::ERR_DEVICE) {
        RUN_ELEVATED(dma::free_pages(list));
    }

    return rc;
}

int32_t virtio_gpu_backend::create_system_memory(uint64_t size, virtio_gpu_memory& mem) {
    if (size > RUN_BYTES_MAX) {
        return gpu::ERR_NO_MEMORY;
    }

    if (allocate_pages(size / pmm::PAGE_SIZE, 0, mem.pages) != gpu::OK) {
        return gpu::ERR_NO_MEMORY;
    }

    dma::buffer list = {};
    if (allocate_pages(1, paging::PAGE_USER, list) != gpu::OK) {
        RUN_ELEVATED(dma::free_pages(mem.pages));
        return gpu::ERR_NO_MEMORY;
    }

    *reinterpret_cast<virtio_gpu_mem_entry*>(list.virt) = {mem.pages.phys, static_cast<uint32_t>(size), 0};

    int32_t rc = create_guest_blob(mem, list, 1, size);
    if (rc == gpu::ERR_NO_MEMORY) {
        RUN_ELEVATED(dma::free_pages(mem.pages));
    }

    return rc;
}

int32_t virtio_gpu_backend::create_context_memory(
    uint64_t size,
    uint32_t flags,
    const virtio_gpu_space& space,
    uint64_t backend_id,
    virtio_gpu_memory& mem
) {
    bool is_mappable = (flags & GPU_BUFFER_MAPPABLE) != 0;

    virtio_gpu_resource_create_blob create = {};
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    create.hdr.ctx_id = space.id.value;

    create.blob_mem = VIRTIO_GPU_BLOB_MEM_HOST3D;
    create.blob_flags = is_mappable ? VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE : 0;
    create.blob_id = backend_id;
    create.size = size;

    m_driver.lock_batch();

    // Refused before any command, so a full region costs the host nothing
    if (is_mappable && !m_region.allocate(size, mem.range)) {
        m_driver.unlock_batch();
        return gpu::ERR_NO_MEMORY;
    }

    mem.resource_id = m_driver.m_next_resource_id++;
    create.resource_id = mem.resource_id;

    int32_t rc = send(create);
    if (rc == gpu::OK && is_mappable) {
        rc = map_host_blob(mem);
    } else if (rc == gpu::ERR_NO_MEMORY && is_mappable) {
        m_region.release(mem.range);
    }

    m_driver.unlock_batch();

    return rc;
}

int32_t virtio_gpu_backend::map_host_blob(virtio_gpu_memory& mem) {
    virtio_gpu_resource_map_blob map_blob = {};
    map_blob.hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
    map_blob.resource_id = mem.resource_id;
    map_blob.offset = mem.range.offset;

    uint32_t response_offset = 0;
    uint32_t response_size = sizeof(virtio_gpu_resp_map_info);
    int32_t rc = send_command(&map_blob, sizeof(map_blob), response_size, &response_offset, nullptr);
    if (rc == gpu::OK) {
        uintptr_t response = m_driver.m_command_page.virt + response_offset;
        auto* info = reinterpret_cast<const virtio_gpu_resp_map_info*>(response);
        mem.is_mapped = true;
        mem.is_cacheable = (info->map_info & VIRTIO_GPU_MAP_CACHE_MASK) == VIRTIO_GPU_MAP_CACHE_CACHED;
        return gpu::OK;
    }

    if (rc == gpu::ERR_DEVICE) {
        return rc;
    }

    virtio_gpu_resource_unref unref = {};
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = mem.resource_id;

    // The host mapped nothing at the range, so it is freed whatever the release answers, unless the backend failed
    if (send(unref) == gpu::ERR_DEVICE) {
        return gpu::ERR_DEVICE;
    }

    m_region.release(mem.range);

    return gpu::ERR_NO_MEMORY;
}

void virtio_gpu_backend::release_on_task(virtio_gpu_memory* mem) {
    virtio_gpu_resource_unref unref = {};
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = mem->resource_id;

    m_driver.lock_batch();
    int32_t rc = send(unref);
    if (rc == gpu::OK && mem->is_mapped) {
        m_region.release(mem->range);
    }

    m_driver.unlock_batch();

    // Unless the host answered the release OK, it may still hold the memory, which then stays for good
    if (rc != gpu::OK) {
        return;
    }

    if (mem->pin.ptr()) {
        mem->reaper_node.init(rc::reaper::reaper_thunk<virtio_gpu_memory, release_pin>);
        RUN_ELEVATED(rc::reaper::defer(&mem->reaper_node));
        return;
    }

    RUN_ELEVATED(dma::free_pages(mem->pages));
    heap::ufree_delete(mem);
}

void virtio_gpu_backend::destroy_on_task(virtio_gpu_space* space) {
    virtio_gpu_ctrl_hdr destroy = {};
    destroy.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    destroy.ctx_id = space->id.value;

    m_driver.lock_batch();
    int32_t rc = send(destroy);
    if (rc == gpu::OK) {
        m_context_ids.give_back(space->id);
    }

    m_driver.unlock_batch();

    // Unless the host answered the destroy OK, the context may live on under its id, which then stays held for good
    if (rc == gpu::OK) {
        heap::ufree_delete(space);
    }
}

void virtio_gpu_backend::destroy_queue_on_task(virtio_gpu_queue* queue) {
    m_driver.lock_batch();
    bool is_stopped = queue->is_stopped;
    if (is_stopped) {
        m_queues.remove(queue);
    }

    m_driver.unlock_batch();

    // A queue the core destroys once the backend failed, its stop unreported, may still have chains the host reads
    if (!is_stopped) {
        return;
    }

    RUN_ELEVATED(dma::free_pages(queue->memory));
    heap::ufree_delete(queue);
}

void virtio_gpu_backend::serve_submissions() {
    m_driver.lock_batch();
    m_driver.drain_returned_chains();

    if (can_send()) {
        for (virtio_gpu_queue& queue : m_queues) {
            if (!queue.is_stopped) {
                report_returns(queue);
            }
        }

        serve_stops();
        hand_over_lists();
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_has_chains_out = m_chain_descriptors != 0;
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    m_driver.unlock_batch();
}

void virtio_gpu_backend::report_returns(virtio_gpu_queue& queue) {
    bool is_refused = false;
    uint64_t finished = free_returned_lists(
        queue.slots,
        queue.ring,
        queue.last_reported,
        queue.last_copied,
        is_refused
    );

    if (finished != queue.last_reported) {
        queue.last_reported = finished;
        RUN_ELEVATED(gpu::progress(queue.core, finished));
    }

    if (is_refused && !queue.is_loss_reported) {
        queue.is_loss_reported = true;
        RUN_ELEVATED(gpu::space_lost(queue.space->core, GPU_LOST_ENGINE, 0));
    }
}

void virtio_gpu_backend::serve_stops() {
    stop_list stopped;
    stopped.init();

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        size_t pending_count = m_pending.size();
        for (size_t i = 0; i < pending_count; i++) {
            virtio_gpu_submission* sub = m_pending.pop_front();
            if (!sub->queue->is_stopping) {
                m_pending.push_back(sub);
            }
        }

        size_t stop_count = m_stops.size();
        for (size_t i = 0; i < stop_count; i++) {
            virtio_gpu_queue* queue = m_stops.pop_front();
            if (queue->chains_out == 0) {
                stopped.push_back(queue);
            } else {
                m_stops.push_back(queue);
            }
        }

        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    while (virtio_gpu_queue* queue = stopped.pop_front()) {
        queue->is_stopped = true;
        RUN_ELEVATED(gpu::queue_stopped(queue->core));
    }
}

void virtio_gpu_backend::hand_over_lists() {
    bool has_handed = false;
    while (true) {
        virtio_gpu_submission* sub = nullptr;
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
            while (m_pending.front() && m_pending.front()->queue->is_stopping) {
                m_pending.pop_front();
            }

            // Later lists wait for one that does not fit, keeping `run()` order across the device
            sub = m_pending.front();
            if (sub && fits(*sub)) {
                m_pending.pop_front();
                sub->queue->chains_out++;
                m_has_chains_out = true;
            } else {
                sub = nullptr;
            }

            sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
        });

        if (!sub) {
            break;
        }

        hand_over(*sub);
        has_handed = true;
    }

    if (has_handed) {
        m_driver.m_control_queue.kick(m_driver.m_control_notify);
    }
}

bool virtio_gpu_backend::fits(const virtio_gpu_submission& sub) {
    uint32_t bytes = sizeof(virtio_gpu_cmd_submit) + sub.size;
    const send_ring& ring = sub.queue->ring;
    uint32_t descriptors = SUBMIT_DESCRIPTORS + (ring.wraps(bytes) ? 1 : 0);

    bool fits_chain_budget = m_chain_descriptors + descriptors <= CHAIN_DESCRIPTORS_MAX;
    bool has_free_descriptors = m_driver.m_control_queue.free_count() >= descriptors;

    return fits_chain_budget && has_free_descriptors && ring.has_room(bytes);
}

void virtio_gpu_backend::hand_over(virtio_gpu_submission& sub) {
    virtio_gpu_queue& queue = *sub.queue;

    virtio_gpu_cmd_submit header = {};
    header.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    header.hdr.flags = VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX;
    header.size = sub.size;

    header.hdr.fence_id = ++m_last_fence;
    header.hdr.ctx_id = queue.space->id.value;
    header.hdr.ring_idx = SUBMIT_RING_INDEX;

    sub.ring_bytes = sizeof(header) + sub.size;
    ring_placement placement = queue.ring.place(sub.ring_bytes);
    auto* ring_memory = reinterpret_cast<uint8_t*>(queue.memory.virt);
    write_placed(ring_memory, placement, 0, &header, sizeof(header));
    write_placed(ring_memory, placement, sizeof(header), sub.commands, sub.size);

    auto slot = static_cast<uint32_t>(sub.sequence % GPU_QUEUE_PENDING_MAX);
    uint32_t response_offset = SEND_RING_BYTES + slot * sizeof(virtio_gpu_ctrl_hdr);
    string::memset(ring_memory + response_offset, 0, sizeof(virtio_gpu_ctrl_hdr));

    chain_buffer chain[SUBMIT_DESCRIPTORS + 1];
    uint8_t count = 0;
    chain[count++] = {queue.memory.phys + placement.offset, placement.first_bytes, 0};
    if (placement.second_bytes != 0) {
        chain[count++] = {queue.memory.phys, placement.second_bytes, 0};
    }

    chain[count++] = {queue.memory.phys + response_offset, sizeof(virtio_gpu_ctrl_hdr), VRING_DESC_F_WRITE};

    // Cannot fail, since `fits()` counted these descriptors free under the batch lock the task still holds
    int32_t head = m_driver.m_control_queue.add_buf_chain(chain, count);

    sub.descriptors = count;
    sub.is_answered_ok = false;
    m_chains[head] = &sub;
    m_chain_descriptors += count;
    queue.last_copied = sub.sequence;
}

bool virtio_gpu_backend::can_send() {
    if (m_driver.m_is_broken) {
        fail();
        return false;
    }

    return !has_failed();
}

int32_t virtio_gpu_backend::send_command(
    const void* request,
    uint32_t request_size,
    uint32_t response_size,
    uint32_t* out_response_offset,
    const chain_buffer* list
) {
    if (!can_send()) {
        return gpu::ERR_DEVICE;
    }

    int32_t rc = m_driver.queue_command(request, request_size, response_size, out_response_offset, list);
    if (rc == OK) {
        rc = m_driver.run_unlogged_batch();
    }

    // The task fails the backend at a passed stop deadline without the batch lock, so the mark may change in a batch
    if (rc == OK) {
        return has_failed() ? gpu::ERR_DEVICE : gpu::OK;
    }

    // A refusal and a device that stopped answering can return the same code, but only the latter is broken
    if (m_driver.m_is_broken) {
        fail();
        return gpu::ERR_DEVICE;
    }

    return gpu::ERR_NO_MEMORY;
}

int32_t virtio_gpu_backend::send(const void* request, uint32_t request_size) {
    return send_command(request, request_size, sizeof(virtio_gpu_ctrl_hdr), nullptr, nullptr);
}

bool virtio_gpu_backend::has_failed() {
    bool is_failed = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        is_failed = m_is_failed;
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    return is_failed;
}

void virtio_gpu_backend::fail() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        m_is_failed = true;
        sync::wake_one(m_driver.m_work_wq);
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });
}

} // namespace drivers
