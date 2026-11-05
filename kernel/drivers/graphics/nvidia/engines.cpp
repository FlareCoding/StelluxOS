#include "drivers/graphics/nvidia/engines.h"
#include "drivers/graphics/nvidia/channel_methods.h"
#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/barrier.h"
#include "hw/mmio.h"
#include "mm/heap.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

// The kernel's copy channel's id, the lowest of its runlist, where no other channel of the driver runs
constexpr uint32_t COPY_CHANNEL_ID = 0;

// The video memory the check fills, followed by a page the fills must leave as the CPU wrote it
constexpr uint64_t CHECK_BYTES       = 64 * 1024;
constexpr uint64_t CHECK_GUARD_BYTES = SMALL_PAGE_BYTES;

// What the CPU writes through BAR1 before the copies, and what the second copy fills. Neither is 0 or all ones, which
// BAR1 reads where no page is mapped and on a card that stopped responding
constexpr uint32_t CHECK_CPU_PATTERN  = 0x5A5A5A5A;
constexpr uint32_t CHECK_FILL_PATTERN = 0xC3C3C3C3;

constexpr uint64_t BYTES_PER_KIB = 1024;

// The GPU addresses a context's own mappings may use, the range NVK's heaps use
constexpr uint64_t USER_VA_START = 4096;
constexpr uint64_t USER_VA_END   = 1ULL << 39;

// The command list bytes one GPFIFO entry runs, one range of method words
constexpr uint64_t COMMAND_RANGE_BYTES = 16;

// Each queue's GPFIFO keeps one entry per running submission for its release sequence, and one entry empty
constexpr uint64_t RUNNING_BYTES_MAX = (GPFIFO_ENTRIES - GPU_QUEUE_PENDING_MAX - 1) * COMMAND_RANGE_BYTES;

// The GPU's nanosecond clock: NVIDIA's NV_VIRTUAL_FUNCTION_TIME_0 is its low word and TIME_1's bits 28:0 its high word
constexpr uint32_t TIME_LOW       = 0xBB0080;
constexpr uint32_t TIME_HIGH      = 0xBB0084;
constexpr uint32_t TIME_HIGH_MASK = 0x1FFFFFFF;

// Finds the first 4-byte word in [first, end) at `virt` reading other than `expected`, or returns `end`
static uint64_t find_differing_word(uintptr_t virt, uint64_t first, uint64_t end, uint32_t expected) {
    for (uint64_t i = first; i < end; i++) {
        if (mmio::read32(virt + i * sizeof(uint32_t)) != expected) {
            return i;
        }
    }

    return end;
}

// Whether `card` reaches each page and is coherent, as it must be to share pages that processes map cached
static bool reaches_coherently(const dma::device_profile& card, const pmm::phys_addr_t* pages, size_t count) {
    if (!card.coherent()) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (!card.reaches(pages[i] + pmm::PAGE_SIZE)) {
            return false;
        }
    }

    return true;
}

engines::engines(
    firmware& fw,
    const gpu_objects& objects,
    video_memory& memory,
    operation_queue& operations,
    failed_mark& failed
)
    : m_firmware(fw), m_objects(objects), m_memory(memory), m_operations(operations), m_failed(failed) {
    m_failure_report.work = report_failure_on_task;
    m_failure_report.owner = this;
}

int32_t engines::bring_up(registers& bar0, const dma::device_profile& card, const chip_info& chip) {
    m_bar0 = bar0;
    m_chip = &chip;
    m_card = card;

    int32_t rc = m_table.read(m_firmware, m_objects.subdevice);
    if (rc != OK) {
        return rc;
    }

    const engine_info* copy_engine = m_table.first_async_copy_engine();
    if (copy_engine == nullptr) {
        log::error(
            "nvidia: the device info table lists %u engines and no asynchronous copy engine, so the kernel has no "
            "copy channel",
            m_table.count()
        );
        return ERR_BAD_STATE;
    }

    uint32_t vector = 0;
    rc = read_copy_vector(m_firmware, copy_engine->type, chip.interrupt_leaf_count, vector);
    if (rc != OK) {
        return rc;
    }

    m_copy_channel = new (m_copy_channel_bytes) copy_channel(m_firmware, m_objects, m_memory, bar0, m_failed);
    rc = m_copy_channel->create(card, chip, *copy_engine, COPY_CHANNEL_ID);
    if (rc != OK) {
        log::error("nvidia: creating the kernel's copy channel failed: %d", rc);
        return rc;
    }

    // Published before the enable, since a release pending already raises the interrupt at once
    m_copy_vector = vector;
    m_is_copy_vector_on.store_release(true);
    enable_vector(bar0, vector);

    rc = check_copy_channel();
    if (rc == OK) {
        log::info(
            "nvidia: the kernel's copy channel runs as channel %u on runlist %u of copy engine type 0x%X, and its "
            "releases raise GPU interrupt vector %u, bit %u of the CPU interrupt tree's leaf %u",
            COPY_CHANNEL_ID,
            copy_engine->runlist,
            copy_engine->type,
            vector,
            vector % VECTORS_PER_LEAF,
            vector / VECTORS_PER_LEAF
        );
        return OK;
    }

    m_is_copy_vector_on.store_release(false);
    mask_vector(bar0, vector);
    if (rc != ERR_TIMEOUT) {
        m_copy_channel->destroy();
    }

    return rc;
}

int32_t engines::register_gpu(const card_identity& identity) {
    m_identity = identity;

    int32_t rc = gpu::OK;
    RUN_ELEVATED(rc = gpu::register_backend(this, &m_device));
    if (rc != gpu::OK) {
        log::error(
            "nvidia: the GPU core cannot publish a device for the GPU (%d), so the display runs on without one",
            rc
        );
        return ERR_NO_MEMORY;
    }

    log::info(
        "nvidia: %s, with %u GPCs and %u TPCs, is registered with the GPU core",
        m_identity.name,
        m_identity.gpc_count,
        m_identity.tpc_count
    );
    return OK;
}

void engines::read_facts(gpu::device_facts& out) {
    memory_report memory = {};
    m_memory.report(memory);

    out.info.backend = GPU_BACKEND_NVIDIA;
    out.info.queue_kinds = 0;
    string::memcpy(out.info.name, m_identity.name, sizeof(out.info.name));

    out.info.vendor_id = m_identity.vendor_id;
    out.info.device_id = m_identity.device_id;
    out.info.pci_bus = m_identity.bus;
    out.info.pci_slot = m_identity.slot;
    out.info.pci_function = m_identity.function;
    out.info.pci_revision = m_identity.revision;

    out.info.video_size = memory.video_bytes;
    out.info.mappable_video_size = memory.window_bytes;
    out.info.va_start = USER_VA_START;
    out.info.va_end = USER_VA_END;
    out.info.page_size = pmm::PAGE_SIZE;
    out.info.placements = (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM);

    out.info.nvidia.chipset = m_chip->id;
    out.info.nvidia.class_gpfifo = static_cast<uint16_t>(m_chip->channel_class);
    out.info.nvidia.class_copy = static_cast<uint16_t>(m_chip->copy_class);
    out.info.nvidia.gpc_count = static_cast<uint16_t>(m_identity.gpc_count);
    out.info.nvidia.tpc_count = static_cast<uint16_t>(m_identity.tpc_count);

    out.submit_timeout_ns = GPU_SUBMIT_TIMEOUT_NS;
    out.running_bytes_max = RUNNING_BYTES_MAX;
}

void engines::read_memory(gpu_memory& out) {
    memory_report memory = {};
    m_memory.report(memory);

    out.video_free = memory.video_free_bytes;
    out.mappable_video_free = memory.window_free_bytes;
}

int32_t engines::read_timestamp(uint64_t& gpu_ns) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    // A carry into the high word between the reads moves it, so the read repeats until both high reads match
    uint32_t high = 0;
    uint32_t low = 0;
    uint32_t high_again = m_bar0.read(TIME_HIGH);
    do {
        high = high_again;
        low = m_bar0.read(TIME_LOW);
        high_again = m_bar0.read(TIME_HIGH);
    } while (high_again != high);

    gpu_ns = (static_cast<uint64_t>(high & TIME_HIGH_MASK) << 32) | low;
    return gpu::OK;
}

int32_t engines::create_memory(
    uint64_t size,
    uint32_t placement,
    uint32_t flags,
    gpu::space*,
    uint64_t,
    gpu::memory** out,
    uint32_t* out_device_id
) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* mem = heap::ualloc_new<backend_memory>();
    if (mem == nullptr) {
        return gpu::ERR_NO_MEMORY;
    }

    mem->placement = placement;
    mem->task_operation.work = release_on_task;
    mem->task_operation.owner = this;
    mem->task_operation.memory = mem;

    int32_t rc = gpu::OK;
    if (placement == GPU_PLACEMENT_SYSTEM) {
        size_t pages = size / pmm::PAGE_SIZE;
        int32_t dma_rc = dma::OK;
        RUN_ELEVATED(dma_rc = dma::alloc_pages(m_card, pages, dma::caching::cached_when_coherent, 0, mem->pages));
        rc = dma_rc == dma::OK ? gpu::OK : gpu::ERR_NO_MEMORY;
    } else {
        rc = create_video_memory(size, flags, *mem);
    }

    if (rc != gpu::OK) {
        heap::ufree_delete(mem);
        return rc;
    }

    *out = mem;
    *out_device_id = 0;

    return gpu::OK;
}

int32_t engines::wrap_pages(
    const pmm::phys_addr_t* pages,
    size_t count,
    mm::shmem_mapping&& pin,
    gpu::memory** out,
    uint32_t* out_device_id
) {
    if (!reaches_coherently(m_card, pages, count)) {
        return gpu::ERR_NO_MEMORY;
    }

    auto* mem = heap::ualloc_new<backend_memory>();
    if (mem == nullptr) {
        return gpu::ERR_NO_MEMORY;
    }

    mem->wrapped_pages = static_cast<memory_run*>(heap::ualloc(count * sizeof(memory_run)));
    if (mem->wrapped_pages == nullptr) {
        heap::ufree_delete(mem);
        return gpu::ERR_NO_MEMORY;
    }

    // The GPU core counts the pin as taken when a wrap returns `gpu::ERR_DEVICE`, so the record keeps it for good
    mem->pin = static_cast<mm::shmem_mapping&&>(pin);
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    for (size_t i = 0; i < count; i++) {
        mem->wrapped_pages[i] = {.address = pages[i], .bytes = pmm::PAGE_SIZE};
    }

    mem->placement = GPU_PLACEMENT_SYSTEM;
    mem->task_operation.work = release_on_task;
    mem->task_operation.owner = this;
    mem->task_operation.memory = mem;
    mem->pin_release.init(rc::reaper::reaper_thunk<backend_memory, release_on_reaper, &backend_memory::pin_release>);

    *out = mem;
    *out_device_id = 0;

    return gpu::OK;
}

int32_t engines::view_for_cpu(gpu::memory* mem, gpu::cpu_view& out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* memory = static_cast<backend_memory*>(mem);
    if (memory->placement == GPU_PLACEMENT_SYSTEM) {
        out = {.phys = memory->pages.phys, .size = memory->pages.size, .cacheable = memory->pages.cached};
    } else {
        out = {.phys = memory->mapping.phys, .size = memory->range.size, .cacheable = false};
    }

    return gpu::OK;
}

void engines::release_memory(gpu::memory* mem) {
    auto* memory = static_cast<backend_memory*>(mem);
    RUN_ELEVATED(m_operations.post_to_task(memory->task_operation));
}

int32_t engines::create_space(gpu::context*, gpu::space** out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* space = heap::ualloc_new<backend_space>(m_firmware, m_objects, m_memory, m_failed);
    if (space == nullptr) {
        return gpu::ERR_NO_MEMORY;
    }

    space->task_operation.work = create_space_on_task;
    space->task_operation.owner = this;
    space->task_operation.space = space;

    int32_t rc = OK;
    RUN_ELEVATED(rc = m_operations.run_on_task(space->task_operation));
    if (rc != OK) {
        heap::ufree_delete(space);
        return refused_creation();
    }

    *out = space;

    return gpu::OK;
}

void engines::destroy_space(gpu::space* s) {
    auto* space = static_cast<backend_space*>(s);
    space->task_operation.work = destroy_space_on_task;
    RUN_ELEVATED(m_operations.post_to_task(space->task_operation));
}

int32_t engines::map(
    gpu::space* s,
    uint64_t va,
    gpu::memory* mem,
    uint64_t offset,
    uint64_t size,
    uint32_t kind,
    bool read_only
) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    if (kind > LAST_UNCOMPRESSED_KIND) {
        return gpu::ERR_REFUSED;
    }

    auto* space = static_cast<backend_space*>(s);
    auto* memory = static_cast<backend_memory*>(mem);
    bool is_system = memory->placement == GPU_PLACEMENT_SYSTEM;
    uint64_t start = is_system ? memory->pages.phys : memory->range.offset;
    memory_run run = {.address = start + offset, .bytes = size};
    page_mapping mapping = {
        .aperture = is_system ? page_aperture::system_memory : page_aperture::video_memory,
        .kind = static_cast<uint8_t>(kind),
        .read_only = read_only,
    };

    // The GPU core maps whole pages inside the memory, so the range lies inside the runs
    const memory_run* runs = &run;
    size_t run_count = 1;
    if (memory->wrapped_pages != nullptr) {
        runs = memory->wrapped_pages + offset / pmm::PAGE_SIZE;
        run_count = size / pmm::PAGE_SIZE;
    }

    int32_t rc = space->addresses.page_tables().map(va, runs, run_count, mapping);
    if (rc == OK) {
        return gpu::OK;
    }

    // A failed update set the failed mark, and the card may then translate the range
    if (has_failed()) {
        report_failure();
        return gpu::ERR_DEVICE;
    }

    return rc == ERR_NO_MEMORY ? gpu::ERR_NO_MEMORY : gpu::ERR_REFUSED;
}

int32_t engines::unmap(gpu::space* s, uint64_t va, uint64_t size) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* space = static_cast<backend_space*>(s);
    if (space->addresses.page_tables().unmap(va, size) == OK) {
        return gpu::OK;
    }

    // The range may stay translated, so an unmap the tables refuse fails the device unless the GPU already failed
    if (has_failed()) {
        report_failure();
    } else {
        fail_device("the page tables refused an unmap");
    }

    return gpu::ERR_DEVICE;
}

// The facts offer no queue kind, so the core creates no queue, and every command list is refused
int32_t engines::create_queue(gpu::space*, uint32_t, gpu::core_queue*, gpu::queue**) {
    return gpu::ERR_DEVICE;
}

void engines::stop_queue(gpu::queue*) {}

void engines::destroy_queue(gpu::queue*) {}

int32_t engines::check_commands(const void*, size_t) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    return gpu::ERR_REFUSED;
}

void engines::run(gpu::queue*, uint64_t, const void*, size_t) {}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void engines::service_interrupt(registers& bar0) {
    if (!m_is_copy_vector_on.load_acquire()) {
        return;
    }

    if (acknowledge_nonstall_interrupt(bar0, m_copy_vector)) {
        m_copy_channel->service();
    }
}

int32_t engines::check_copy_channel() {
    vram_range range = {};
    int32_t rc = m_memory.allocate(CHECK_BYTES + CHECK_GUARD_BYTES, SMALL_PAGE_BYTES, vram_use::general, range);
    if (rc != OK) {
        log::error("nvidia: allocating video memory for the copy channel's check failed: %d", rc);
        return rc;
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(range, mapping);
    if (rc != OK) {
        log::error("nvidia: mapping the copy channel's check memory through BAR1 failed: %d", rc);
        m_memory.release(range);
        return rc;
    }

    uint64_t all_words = (CHECK_BYTES + CHECK_GUARD_BYTES) / sizeof(uint32_t);
    for (uint64_t i = 0; i < all_words; i++) {
        mmio::write32(mapping.virt + i * sizeof(uint32_t), CHECK_CPU_PATTERN);
    }

    // The read returns once the writes are in video memory, before any copy is submitted. On x86_64 a store fence
    // orders no read after the writes
    barrier::dma_full();
    (void)mmio::read32(mapping.virt + (all_words - 1) * sizeof(uint32_t));

    uint64_t zero_ns = 0;
    uint64_t fill_ns = 0;
    rc = fill_and_compare(range, mapping.virt, 0, zero_ns);
    if (rc == OK) {
        rc = fill_and_compare(range, mapping.virt, CHECK_FILL_PATTERN, fill_ns);
    }

    if (rc == ERR_TIMEOUT) {
        m_memory.unmap_for_cpu(range);
        log::error(
            "nvidia: the copy channel's check memory at 0x%lX in video memory stays allocated for good, since the hung "
            "copy may still write it",
            range.offset
        );
        return rc;
    }

    m_memory.release(range);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the kernel's copy channel zeroed %lu KiB of video memory in %lu us, then filled it with 0x%X in %lu "
        "us, each read back through BAR1 with the page after it unchanged",
        CHECK_BYTES / BYTES_PER_KIB,
        zero_ns / NS_PER_US,
        CHECK_FILL_PATTERN,
        fill_ns / NS_PER_US
    );
    return OK;
}

int32_t engines::fill_and_compare(const vram_range& range, uintptr_t virt, uint32_t value, uint64_t& took_ns) {
    uint64_t start_ns = clock::now_ns();
    int32_t rc = m_copy_channel->fill(range.offset, CHECK_BYTES, value);
    took_ns = clock::now_ns() - start_ns;
    if (rc != OK) {
        return rc;
    }

    // A read through the write-combining mapping could otherwise happen before the read that saw the copy finish
    barrier::dma_read();

    uint64_t filled_words = CHECK_BYTES / sizeof(uint32_t);
    uint64_t all_words = (CHECK_BYTES + CHECK_GUARD_BYTES) / sizeof(uint32_t);
    uint32_t expected = value;
    uint64_t differing = find_differing_word(virt, 0, filled_words, value);
    if (differing == filled_words) {
        expected = CHECK_CPU_PATTERN;
        differing = find_differing_word(virt, filled_words, all_words, expected);
    }

    if (differing != all_words) {
        log::error(
            "nvidia: after the kernel's copy channel filled 0x%X, word %lu of the check's memory at 0x%lX in video "
            "memory reads 0x%X through BAR1, where 0x%X is expected",
            value,
            differing,
            range.offset,
            mmio::read32(virt + differing * sizeof(uint32_t)),
            expected
        );
        return ERR_BAD_STATE;
    }

    return OK;
}

bool engines::has_failed() const {
    return m_failed.is_set() || m_is_device_failed.load_acquire();
}

int32_t engines::create_video_memory(uint64_t size, uint32_t flags, backend_memory& mem) {
    vram_use use = (flags & GPU_BUFFER_SCANOUT) != 0 ? vram_use::scanout : vram_use::general;
    if (m_memory.allocate(size, SMALL_PAGE_BYTES, use, mem.range) != OK) {
        return gpu::ERR_NO_MEMORY;
    }

    // Mapped before the zeroing, so a full window refuses the create before a long copy. The core maps the CPU's view
    // by physical address, so a mapping without one is refused
    bool is_mappable = (flags & GPU_BUFFER_MAPPABLE) != 0;
    if (is_mappable) {
        int32_t rc = m_memory.map_for_cpu(mem.range, mem.mapping);
        if (rc != OK || mem.mapping.phys == 0) {
            m_memory.release(mem.range);
            return gpu::ERR_NO_MEMORY;
        }
    }

    int32_t rc = zero_video_memory(mem.range);
    if (rc == ERR_TIMEOUT) {
        if (is_mappable) {
            m_memory.unmap_for_cpu(mem.range);
        }

        fail_device("the kernel's copy channel hung zeroing video memory");
        log::error(
            "nvidia: video memory at 0x%lX stays allocated for good, since the hung copy may still write it",
            mem.range.offset
        );
        return gpu::ERR_DEVICE;
    }

    if (rc != OK) {
        m_memory.release(mem.range);
        return gpu::ERR_NO_MEMORY;
    }

    return gpu::OK;
}

int32_t engines::zero_video_memory(const vram_range& range) {
    for (uint64_t done = 0; done < range.size; done += FILL_BYTES_MAX) {
        uint64_t left = range.size - done;
        uint64_t bytes = left < FILL_BYTES_MAX ? left : FILL_BYTES_MAX;
        int32_t rc = m_copy_channel->fill(range.offset + done, bytes, 0);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

void engines::fail_device(const char* reason) {
    if (!m_is_device_failed.exchange_acq_rel(true)) {
        log::error("nvidia: %s, so the GPU core's device failed while the display runs on", reason);
    }

    report_failure();
}

void engines::report_failure() {
    if (!m_is_failure_reported.exchange_acq_rel(true)) {
        RUN_ELEVATED(m_operations.post_to_task(m_failure_report));
    }
}

void engines::report_failure_on_task(operation& op) {
    engines& gpu_engines = *static_cast<failure_operation&>(op).owner;
    RUN_ELEVATED(gpu::device_failed(gpu_engines.m_device));
}

int32_t engines::refused_creation() {
    if (!has_failed()) {
        return gpu::ERR_NO_MEMORY;
    }

    report_failure();
    return gpu::ERR_DEVICE;
}

void engines::release_on_task(operation& op) {
    auto& release = static_cast<memory_operation&>(op);
    engines& gpu_engines = *release.owner;
    backend_memory* mem = release.memory;
    bool is_wrapped = mem->wrapped_pages != nullptr;

    // A failed GPU may still reach the memory, so it stays, and a wrapped memory's record stays to keep the pin
    if (gpu_engines.has_failed()) {
        if (!is_wrapped) {
            heap::ufree_delete(mem);
        }

        return;
    }

    // The pin's release waits for the shared memory object's mutex, which the driver task must never wait for
    if (is_wrapped) {
        RUN_ELEVATED(rc::reaper::defer(&mem->pin_release));
        return;
    }

    if (mem->placement == GPU_PLACEMENT_SYSTEM) {
        RUN_ELEVATED(dma::free_pages(mem->pages));
    } else {
        gpu_engines.m_memory.release(mem->range);
    }

    heap::ufree_delete(mem);
}

rc::reaper::cleanup_result engines::release_on_reaper(backend_memory* mem) {
    heap::ufree(mem->wrapped_pages);
    heap::ufree_delete(mem);

    return rc::reaper::DONE;
}

void engines::create_space_on_task(operation& op) {
    auto& creation = static_cast<space_operation&>(op);
    op.result = creation.space->addresses.create();
}

void engines::destroy_space_on_task(operation& op) {
    auto& destroy = static_cast<space_operation&>(op);
    engines& gpu_engines = *destroy.owner;
    backend_space* space = destroy.space;

    // A failed GPU may still walk the tables, so they stay in video memory with the firmware's object
    if (!gpu_engines.has_failed()) {
        space->addresses.destroy();
    }

    heap::ufree_delete(space);

    if (gpu_engines.has_failed()) {
        gpu_engines.report_failure();
    }
}

} // namespace drivers::nvidia
