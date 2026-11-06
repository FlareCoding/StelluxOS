#include "drivers/graphics/virtio_gpu_backend.h"
#include "drivers/graphics/virtio_gpu.h"
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

// The header's context id is 32 bits
constexpr uint32_t LAST_CONTEXT_ID = 0xFFFFFFFF;

constexpr uint64_t BYTES_PER_MIB = 1ULL << 20;

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
    m_releases.init();
    m_destroys.init();
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

bool virtio_gpu_backend::has_task_work() const {
    bool has_unreported_failure = m_is_failed && !m_is_failure_reported;
    return !m_releases.empty() || !m_destroys.empty() || has_unreported_failure;
}

void virtio_gpu_backend::serve_task_work() {
    virtio_gpu_memory* released = nullptr;
    virtio_gpu_space* destroyed = nullptr;
    bool reports_failure = false;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_driver.m_work_lock);
        released = m_releases.pop_front();
        destroyed = m_destroys.pop_front();
        reports_failure = m_is_failed && !m_is_failure_reported;
        m_is_failure_reported = m_is_failed;
        sync::spin_unlock_irqrestore(m_driver.m_work_lock, irq);
    });

    if (released) {
        release_on_task(released);
    }

    if (destroyed) {
        destroy_on_task(destroyed);
    }

    // A registration that broke the device left no device to report to
    if (reports_failure && m_device) {
        RUN_ELEVATED(gpu::device_failed(m_device));
    }
}

void virtio_gpu_backend::read_facts(gpu::device_facts& out) {
    out.info.backend = GPU_BACKEND_VIRTIO;
    out.info.queue_kinds = 0;
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
    out.running_bytes_max = GPU_COMMANDS_BYTES_MAX;
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

int32_t virtio_gpu_backend::create_space(gpu::context*, gpu::space** out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* space = heap::ualloc_new<virtio_gpu_space>();
    if (!space) {
        return gpu::ERR_NO_MEMORY;
    }

    virtio_gpu_ctx_create create = {};
    create.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    create.context_init = VIRTIO_GPU_CAPSET_VENUS;

    int32_t rc = gpu::ERR_NO_MEMORY;
    lock_batch();
    if (m_context_ids.take(space->id)) {
        create.hdr.ctx_id = space->id.value;
        rc = send(create);
        if (rc == gpu::ERR_NO_MEMORY) {
            m_context_ids.give_back(space->id);
        }
    }

    unlock_batch();

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

    lock_batch();
    int32_t rc = send(attach);
    unlock_batch();

    return rc;
}

int32_t virtio_gpu_backend::unmap(gpu::space* s, uint64_t va, uint64_t) {
    virtio_gpu_ctx_resource detach = {};
    detach.hdr.type = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    detach.hdr.ctx_id = static_cast<virtio_gpu_space*>(s)->id.value;
    detach.resource_id = static_cast<uint32_t>(va);

    lock_batch();
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

    unlock_batch();

    return rc;
}

// The facts offer no queue kind, so the core asks for no queue, and every command list is refused
int32_t virtio_gpu_backend::create_queue(gpu::space*, uint32_t, gpu::core_queue*, gpu::queue**) {
    return has_failed() ? gpu::ERR_DEVICE : gpu::ERR_NO_MEMORY;
}

void virtio_gpu_backend::stop_queue(gpu::queue*) {}

void virtio_gpu_backend::destroy_queue(gpu::queue*) {}

int32_t virtio_gpu_backend::check_commands(const void*, size_t) {
    return has_failed() ? gpu::ERR_DEVICE : gpu::ERR_REFUSED;
}

void virtio_gpu_backend::run(gpu::queue*, uint64_t, const void*, size_t) {}

int32_t virtio_gpu_backend::read_capsets() {
    volatile virtio_gpu_config* config = m_driver.device_config<virtio_gpu_config>();
    uint32_t listed = config ? config->num_capsets : 0;
    uint32_t count = listed < CAPSET_IDS ? listed : CAPSET_IDS;

    int32_t rc = gpu::OK;
    lock_batch();
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

    unlock_batch();

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

    lock_batch();
    mem.resource_id = m_driver.m_next_resource_id++;
    create.resource_id = mem.resource_id;
    int32_t rc = send_command(&create, sizeof(create), sizeof(virtio_gpu_ctrl_hdr), nullptr, &list_buffer);
    unlock_batch();

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

    lock_batch();

    // Refused before any command, so a full region costs the host nothing
    if (is_mappable && !m_region.allocate(size, mem.range)) {
        unlock_batch();
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

    unlock_batch();

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

    lock_batch();
    int32_t rc = send(unref);
    if (rc == gpu::OK && mem->is_mapped) {
        m_region.release(mem->range);
    }

    unlock_batch();

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

    lock_batch();
    int32_t rc = send(destroy);
    if (rc == gpu::OK) {
        m_context_ids.give_back(space->id);
    }

    unlock_batch();

    // Unless the host answered the destroy OK, the context may live on under its id, which then stays held for good
    if (rc == gpu::OK) {
        heap::ufree_delete(space);
    }
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

    if (rc == OK) {
        return gpu::OK;
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

void virtio_gpu_backend::lock_batch() {
    RUN_ELEVATED(sync::mutex_lock(m_driver.m_batch_lock));
}

void virtio_gpu_backend::unlock_batch() {
    RUN_ELEVATED(sync::mutex_unlock(m_driver.m_batch_lock));
}

} // namespace drivers
