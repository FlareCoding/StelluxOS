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
#include "mm/paging_types.h"
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

// The GPU addresses a context's own mappings may use, the range NVK's heaps use, with the kernel's mappings above it
constexpr uint64_t USER_VA_START = 4096;
constexpr uint64_t USER_VA_END   = 1ULL << 39;

// The command list bytes one GPFIFO entry runs, one range of method words
constexpr uint64_t COMMAND_RANGE_BYTES = 16;

// Each queue's GPFIFO keeps one entry per running submission for its release sequence, and one entry empty
constexpr uint64_t RUNNING_BYTES_MAX = (GPFIFO_ENTRIES - GPU_QUEUE_PENDING_MAX - 1) * COMMAND_RANGE_BYTES;

// A context's part above its user range: a page with a progress semaphore per queue slot, then a window per slot
// holding its queue's GPFIFO and then its push buffer
constexpr uint64_t SEMAPHORES_ADDRESS    = USER_VA_END;
constexpr uint64_t QUEUE_WINDOW_BYTES    = 256 * BYTES_PER_KIB;
constexpr uint64_t QUEUE_WINDOWS_ADDRESS = SEMAPHORES_ADDRESS + QUEUE_WINDOW_BYTES;

// A release sequence per submission a queue may hold and one more, so a sequence is rewritten only once a later
// release showed the GPU fetched it
constexpr uint64_t RELEASE_SLOTS      = GPU_QUEUE_PENDING_MAX + 1;
constexpr uint64_t RELEASE_SLOT_BYTES = 128;
constexpr size_t   PUSH_BUFFER_PAGES  = (RELEASE_SLOTS * RELEASE_SLOT_BYTES + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

static_assert(RELEASE_WORDS * sizeof(uint32_t) <= RELEASE_SLOT_BYTES, "a release sequence fits its slot");
static_assert(GPFIFO_BYTES + PUSH_BUFFER_PAGES * pmm::PAGE_SIZE <= QUEUE_WINDOW_BYTES, "the rings fit the window");
static_assert(GPU_QUEUES_MAX < 32, "the shift building a space's slot mask stays defined");

// Where an address space holding a graphics channel maps the graphics engine's context buffers, above the queue windows
constexpr uint64_t GRAPHICS_BUFFERS_ADDRESS = USER_VA_END + (1ULL << 30);

static_assert(
    QUEUE_WINDOWS_ADDRESS + GPU_QUEUES_MAX * QUEUE_WINDOW_BYTES <= GRAPHICS_BUFFERS_ADDRESS,
    "the context buffers lie above the queue windows"
);
static_assert(QUEUE_WINDOWS_ADDRESS >= 1ULL << 38, "a split space maps nothing below its directory 1's 256 GiB");

// A global context buffer and the layout it is allocated by
struct global_buffer {
    context_buffer_layout graphics_buffer_layouts::* layout;
    context_buffer global_context_buffers::*         buffer;
};

static const global_buffer GLOBAL_BUFFERS[] = {
    {&graphics_buffer_layouts::bundle, &global_context_buffers::bundle},
    {&graphics_buffer_layouts::page_pool, &global_context_buffers::page_pool},
    {&graphics_buffer_layouts::attribute, &global_context_buffers::attribute},
    {&graphics_buffer_layouts::rtv, &global_context_buffers::rtv},
    {&graphics_buffer_layouts::fecs_event, &global_context_buffers::fecs_event},
    {&graphics_buffer_layouts::priv_access_map, &global_context_buffers::priv_access_map},
    {&graphics_buffer_layouts::priv_access_map, &global_context_buffers::unrestricted_priv_access_map},
};

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

static uint64_t slot_semaphore_address(uint32_t slot) {
    return SEMAPHORES_ADDRESS + slot * sizeof(uint32_t);
}

static uint64_t slot_gpfifo_address(uint32_t slot) {
    return QUEUE_WINDOWS_ADDRESS + slot * QUEUE_WINDOW_BYTES;
}

static uint64_t slot_push_buffer_address(uint32_t slot) {
    return slot_gpfifo_address(slot) + GPFIFO_BYTES;
}

static uintptr_t semaphore_virt(const backend_queue& queue) {
    return queue.space->semaphores.virt + queue.slot * sizeof(uint32_t);
}

// Reports to the GPU core the last submission `queue`'s semaphore shows finished, under the queue list's lock
__PRIVILEGED_CODE static void report_progress(backend_queue& queue) {
    // Read before `last_run`, which `run` stores before its doorbell, so a release seen never meets an older last run
    uint32_t released = mmio::read32(semaphore_virt(queue));
    uint64_t last_run = queue.last_run.load_relaxed();

    // The low 32 bits of a sequence number at most GPU_QUEUE_PENDING_MAX behind the last run. Widening against the last
    // run rather than the last reported keeps a value the queue's own commands wrote at or below the last run
    uint32_t behind = static_cast<uint32_t>(last_run) - released;
    if (behind >= last_run - queue.last_reported) {
        return;
    }

    queue.last_reported = last_run - behind;
    gpu::progress(queue.core, queue.last_reported);
}

// Copies out range `index` of a command list, which the GPU core hands over with no promise of alignment
static gpu_nvidia_range range_at(const void* commands, size_t index) {
    gpu_nvidia_range range;
    string::memcpy(&range, static_cast<const uint8_t*>(commands) + index * sizeof(range), sizeof(range));
    return range;
}

// Whether `check_commands` accepts `range`: whole words one GPFIFO entry counts, in the user range, known flags
static bool is_runnable(const gpu_nvidia_range& range) {
    uint32_t words = range.size / sizeof(uint32_t);
    bool is_counted = range.size % sizeof(uint32_t) == 0 && words != 0 && words <= GPFIFO_ENTRY_WORDS_MAX;
    bool is_in_user_range = range.va % sizeof(uint32_t) == 0 && range.va >= USER_VA_START &&
                            range.va <= USER_VA_END && range.size <= USER_VA_END - range.va;

    return is_counted && is_in_user_range && (range.flags & ~GPU_COMMANDS_GPU_WRITTEN) == 0;
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
    m_queues.init();

    m_fault_attribution.work = attribute_error_on_task;
    m_fault_attribution.owner = this;
    m_fault_attribution.reason = GPU_LOST_FAULT;

    m_engine_error_attribution.work = attribute_error_on_task;
    m_engine_error_attribution.owner = this;
    m_engine_error_attribution.reason = GPU_LOST_ENGINE;
}

// Refuses `engine`'s channel RAM unless it is 4-byte aligned inside BAR0, logging `name` and `outcome`: its channels
// read their fault bits there, and a read past the mapping faults the CPU, as an unaligned one does on aarch64
static int32_t check_channel_ram(
    const engine_info& engine,
    uint64_t bar0_bytes,
    const char* name,
    const char* outcome
) {
    if (engine.channel_ram % CHANNEL_RAM_ENTRY_BYTES != 0) {
        log::error(
            "nvidia: %s type 0x%X's channel RAM at BAR0 offset 0x%X is not 4-byte aligned, so %s",
            name,
            engine.type,
            engine.channel_ram,
            outcome
        );
        return ERR_BAD_STATE;
    }

    uint64_t channel_ram_bytes = static_cast<uint64_t>(CHANNEL_IDS) * CHANNEL_RAM_ENTRY_BYTES;
    if (!lies_inside(engine.channel_ram, channel_ram_bytes, bar0_bytes)) {
        log::error(
            "nvidia: %s type 0x%X's channel RAM of 0x%lX bytes at BAR0 offset 0x%X ends past BAR0's 0x%lX bytes, "
            "so %s",
            name,
            engine.type,
            channel_ram_bytes,
            engine.channel_ram,
            bar0_bytes,
            outcome
        );
        return ERR_BAD_STATE;
    }

    return OK;
}

int32_t engines::bring_up(
    registers& bar0,
    uint64_t bar0_bytes,
    const dma::device_profile& card,
    const chip_info& chip
) {
    m_bar0 = bar0;
    m_bar0_bytes = bar0_bytes;
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

    rc = check_channel_ram(*copy_engine, bar0_bytes, "copy engine", "the kernel has no copy channel");
    if (rc != OK) {
        return rc;
    }

    uint32_t vector = 0;
    rc = read_nonstall_vector(m_firmware, copy_engine->type, chip.interrupt_leaf_count, vector);
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
        m_firmware.set_error_sink(this);
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

void engines::report_marked_failure() {
    if (m_device != nullptr && m_failed.is_set()) {
        report_failure();
    }
}

void engines::read_facts(gpu::device_facts& out) {
    memory_report memory = {};
    m_memory.report(memory);

    out.info.backend = GPU_BACKEND_NVIDIA;
    out.info.queue_kinds = (1u << GPU_QUEUE_GRAPHICS) | (1u << GPU_QUEUE_COPY);
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

    out.info.nvidia.class_2d = static_cast<uint16_t>(m_chip->graphics.two_d_class);
    out.info.nvidia.class_3d = static_cast<uint16_t>(m_chip->graphics.three_d_class);
    out.info.nvidia.class_m2mf = static_cast<uint16_t>(m_chip->graphics.inline_to_memory_class);
    out.info.nvidia.class_compute = static_cast<uint16_t>(m_chip->graphics.compute_class);

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

bool engines::find_scanout_range(const gpu::memory_view& view, vram_range& out) const {
    bool is_created_here = view.owner == this;
    bool is_scanout = view.placement == GPU_PLACEMENT_VIDEO && (view.flags & GPU_BUFFER_SCANOUT) != 0;
    if (!is_created_here || !is_scanout) {
        return false;
    }

    out = static_cast<const backend_memory*>(view.mem)->range;
    return true;
}

void engines::release_memory(gpu::memory* mem) {
    auto* memory = static_cast<backend_memory*>(mem);
    RUN_ELEVATED(m_operations.post_to_task(memory->task_operation));
}

int32_t engines::create_space(gpu::context* core, gpu::space** out) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    auto* space = heap::ualloc_new<backend_space>(m_firmware, m_objects, m_memory, m_failed);
    if (space == nullptr) {
        return gpu::ERR_NO_MEMORY;
    }

    space->core = core;
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

int32_t engines::create_queue(gpu::space* s, uint32_t kind, gpu::core_queue* core, gpu::queue** out) {
    if (has_failed()) {
        report_failure();
        return gpu::ERR_DEVICE;
    }

    auto* queue = heap::ualloc_new<backend_queue>(m_firmware, m_objects, m_memory, m_bar0);
    if (queue == nullptr) {
        return gpu::ERR_NO_MEMORY;
    }

    queue->space = static_cast<backend_space*>(s);
    queue->kind = kind;
    queue->core = core;
    queue->task_operation.work = create_queue_on_task;
    queue->task_operation.owner = this;
    queue->task_operation.queue = queue;

    // Zeroed before the driver task's part, as created memory is, so the task never waits for a copy
    int32_t rc = kind == GPU_QUEUE_GRAPHICS ? allocate_own_buffers(*queue) : OK;
    if (rc == OK) {
        RUN_ELEVATED(rc = m_operations.run_on_task(queue->task_operation));
    }

    if (rc != OK) {
        heap::ufree_delete(queue);
        return refused_creation();
    }

    *out = queue;

    return gpu::OK;
}

void engines::stop_queue(gpu::queue* q) {
    auto* queue = static_cast<backend_queue*>(q);
    queue->task_operation.work = stop_queue_on_task;
    RUN_ELEVATED(m_operations.post_to_task(queue->task_operation));
}

void engines::destroy_queue(gpu::queue* q) {
    auto* queue = static_cast<backend_queue*>(q);
    queue->destroy_operation.work = destroy_queue_on_task;
    queue->destroy_operation.owner = this;
    queue->destroy_operation.queue = queue;
    RUN_ELEVATED(m_operations.post_to_task(queue->destroy_operation));
}

int32_t engines::check_commands(const void* commands, size_t size) {
    if (has_failed()) {
        return gpu::ERR_DEVICE;
    }

    if (size % sizeof(gpu_nvidia_range) != 0) {
        return gpu::ERR_REFUSED;
    }

    for (size_t i = 0; i < size / sizeof(gpu_nvidia_range); i++) {
        if (!is_runnable(range_at(commands, i))) {
            return gpu::ERR_REFUSED;
        }
    }

    return gpu::OK;
}

void engines::run(gpu::queue* q, uint64_t sequence, const void* commands, size_t size) {
    if (has_failed()) {
        return;
    }

    auto* queue = static_cast<backend_queue*>(q);
    for (size_t i = 0; i < size / sizeof(gpu_nvidia_range); i++) {
        gpu_nvidia_range range = range_at(commands, i);
        bool is_gpu_written = (range.flags & GPU_COMMANDS_GPU_WRITTEN) != 0;
        gpfifo_fetch fetch = is_gpu_written ? gpfifo_fetch::after_earlier : gpfifo_fetch::ahead;

        queue->user_channel.write_entry(make_gpfifo_entry(range.va, range.size / sizeof(uint32_t), fetch));
    }

    // A graphics queue's commands may leave 3D and compute work running at once, and its release must follow both
    idle_scope scope = queue->kind == GPU_QUEUE_GRAPHICS ? idle_scope::every_pipe : idle_scope::current_pipe;
    uint64_t slot_offset = (sequence % RELEASE_SLOTS) * RELEASE_SLOT_BYTES;
    uint64_t slot_address = slot_push_buffer_address(queue->slot) + slot_offset;
    uint32_t* words = reinterpret_cast<uint32_t*>(queue->push_buffer.virt + slot_offset);
    write_release(words, slot_semaphore_address(queue->slot), static_cast<uint32_t>(sequence), scope);

    queue->user_channel.write_entry(make_gpfifo_entry(slot_address, RELEASE_WORDS, gpfifo_fetch::ahead));

    // Stored before the doorbell, so the handler never reads this release with an older last run
    queue->last_run.store_relaxed(sequence);
    queue->user_channel.kick();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void engines::service_interrupt(registers& bar0) {
    // Both vectors are cleared before any semaphore is read, so a release landing after a read raises its vector again
    bool is_copy_pending = m_is_copy_vector_on.load_acquire() && acknowledge_nonstall_interrupt(bar0, m_copy_vector);
    bool is_graphics_pending =
        m_is_graphics_vector_on.load_acquire() && acknowledge_nonstall_interrupt(bar0, m_graphics_vector);
    if (!is_copy_pending && !is_graphics_pending) {
        return;
    }

    if (is_copy_pending) {
        m_copy_channel->service();
    }

    sync::irq_state irq = sync::spin_lock_irqsave(m_queues_lock);
    for (backend_queue& queue : m_queues) {
        report_progress(queue);
    }

    sync::spin_unlock_irqrestore(m_queues_lock, irq);
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

        // The GPU's failure may have ended the zeroing, and its cause is logged where it happened
        if (has_failed()) {
            report_failure();
        } else {
            fail_device("the kernel's copy channel hung zeroing video memory");
        }

        log::error(
            "nvidia: video memory at 0x%lX stays allocated for good, since its zeroing may still write it",
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

int32_t engines::bring_up_graphics() {
    const engine_info* graphics = m_table.graphics_engine();
    if (graphics == nullptr) {
        log::error(
            "nvidia: the device info table lists %u %s and no graphics engine, so the GPU core gets no device",
            m_table.count(),
            m_table.count() == 1 ? "engine" : "engines"
        );
        return ERR_BAD_STATE;
    }

    if (m_table.graphics_copy_engine() == nullptr) {
        log::error(
            "nvidia: the device info table lists no copy engine on the graphics engine's runlist %u, so the GPU core "
            "gets no device",
            graphics->runlist
        );
        return ERR_BAD_STATE;
    }

    int32_t rc = check_channel_ram(*graphics, m_bar0_bytes, "graphics engine", "the GPU core gets no device");
    if (rc != OK) {
        return rc;
    }

    uint32_t vector = 0;
    rc = read_nonstall_vector(m_firmware, graphics->type, m_chip->interrupt_leaf_count, vector);
    if (rc != OK) {
        log::error("nvidia: reading the graphics engine's nonstall vector failed: %d", rc);
        return rc;
    }

    rc = read_graphics_buffer_layouts(m_firmware, m_graphics_layouts);
    if (rc != OK) {
        log::error("nvidia: reading the graphics engine's context buffer layouts failed: %d", rc);
        return rc;
    }

    m_own_buffers_start = place_global_buffers(m_graphics_layouts, GRAPHICS_BUFFERS_ADDRESS, m_global_buffers);

    size_t allocated = 0;
    for (const global_buffer& global : GLOBAL_BUFFERS) {
        rc = allocate_bring_up_buffer(m_graphics_layouts.*global.layout, m_global_buffers.*global.buffer);
        if (rc != OK) {
            break;
        }

        allocated++;
    }

    bool is_reachable = false;
    if (rc == OK) {
        rc = capture_golden_context(*graphics, is_reachable);
    }

    if (rc != OK && !is_reachable) {
        for (size_t i = 0; i < allocated; i++) {
            m_memory.release((m_global_buffers.*GLOBAL_BUFFERS[i].buffer).range);
        }
    }

    if (rc != OK) {
        log::error("nvidia: bringing up the graphics engine failed: %d, so the GPU core gets no device", rc);
        return rc;
    }

    // Published before the enable, since a release pending already raises the interrupt at once
    m_graphics_vector = vector;
    m_is_graphics_vector_on.store_release(true);
    enable_vector(m_bar0, vector);

    log::info(
        "nvidia: graphics channels' releases raise GPU interrupt vector %u, bit %u of the CPU interrupt tree's leaf %u",
        vector,
        vector % VECTORS_PER_LEAF,
        vector / VECTORS_PER_LEAF
    );
    return OK;
}

int32_t engines::allocate_context_buffer(const context_buffer_layout& layout, context_buffer& buffer) {
    uint64_t alignment = layout.alignment > SMALL_PAGE_BYTES ? layout.alignment : SMALL_PAGE_BYTES;
    vram_range range = {};
    int32_t rc = m_memory.allocate(layout.bytes, alignment, vram_use::general, range);
    if (rc != OK) {
        return rc;
    }

    rc = zero_video_memory(range);
    if (rc == ERR_TIMEOUT) {
        log::error(
            "nvidia: video memory at 0x%lX stays allocated for good, since the hung copy may still write it",
            range.offset
        );
        return rc;
    }

    if (rc != OK) {
        m_memory.release(range);
        return rc;
    }

    buffer.range = range;
    return OK;
}

int32_t engines::allocate_bring_up_buffer(const context_buffer_layout& layout, context_buffer& buffer) {
    int32_t rc = allocate_context_buffer(layout, buffer);
    if (rc != OK && rc != ERR_TIMEOUT) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory for a graphics context buffer failed: %d",
            layout.bytes,
            rc
        );
    }

    return rc;
}

int32_t engines::capture_golden_context(const engine_info& graphics, bool& is_reachable) {
    channel_context_buffers golden = {};
    place_own_buffers(m_graphics_layouts, m_own_buffers_start, 0, golden);

    int32_t rc = allocate_bring_up_buffer(m_graphics_layouts.main, golden.main);
    if (rc != OK) {
        return rc;
    }

    rc = allocate_bring_up_buffer(m_graphics_layouts.patch, golden.patch);
    if (rc != OK) {
        m_memory.release(golden.main.range);
        return rc;
    }

    // The firmware refuses the golden context's promotion in an externally owned space
    address_space space(m_firmware, m_objects, m_memory, m_failed);
    rc = space.create_split();
    if (rc == OK) {
        rc = map_global_buffers(space.page_tables(), m_global_buffers);
    }

    if (rc == OK) {
        rc = map_own_buffers(space.page_tables(), golden);
    }

    // Channel ids are one space across runlists, so the golden channel's must differ from the copy channel's
    uint32_t channel_id = 0;
    bool has_id = take_channel_id(channel_id);
    if (rc == OK && !has_id) {
        rc = ERR_BAD_STATE;
    }

    channel golden_channel(m_firmware, m_objects, m_memory, m_bar0);
    channel_setup setup = {
        .engine = graphics,
        .id = channel_id,
        .gpfifo_address = slot_gpfifo_address(0),
        .is_privileged = false,
    };

    bool is_allocated = false;
    if (rc == OK) {
        rc = golden_channel.allocate(m_card, *m_chip, space, setup);
        is_allocated = rc == OK;
    }

    if (rc == OK) {
        rc = promote_graphics_context(
            m_firmware,
            m_objects.subdevice,
            golden_channel.handle(),
            m_global_buffers,
            golden,
            true
        );
    }

    // The firmware captures the golden context when the channel's first 3D object is allocated
    if (rc == OK) {
        rc = golden_channel.add_object(m_chip->graphics.three_d_class, nullptr, 0);
    }

    int32_t release_rc = is_allocated ? golden_channel.destroy() : OK;
    if (release_rc == OK && has_id) {
        give_back_channel_id(channel_id);
    }

    // A split space whose release the firmware refused keeps tables that map every context buffer
    if (release_rc == OK && space.handle() != 0) {
        release_rc = space.destroy();
    }

    if (release_rc != OK) {
        log::error(
            "nvidia: the graphics engine's context buffers stay allocated for good, since the firmware may still reach "
            "them"
        );
        is_reachable = true;
        return rc != OK ? rc : release_rc;
    }

    m_memory.release(golden.patch.range);
    m_memory.release(golden.main.range);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the graphics engine's golden context was captured on channel %u of runlist %u, main buffer %lu KiB",
        channel_id,
        graphics.runlist,
        golden.main.range.size / BYTES_PER_KIB
    );

    return OK;
}

void engines::fail_device(const char* reason) {
    if (!m_is_device_failed.exchange_acq_rel(true)) {
        log::error("nvidia: %s, so the GPU core's device failed while the display runs on", reason);
    }

    report_failure();
}

void engines::report_failure() {
    if (m_is_failure_reported.exchange_acq_rel(true)) {
        return;
    }

    if (m_copy_channel != nullptr) {
        m_copy_channel->mark_hung();
    }

    RUN_ELEVATED(m_operations.post_to_task(m_failure_report));
}

void engines::report_failure_on_task(operation& op) {
    engines& gpu_engines = *static_cast<failure_operation&>(op).owner;
    gpu_engines.free_user_channels();

    if (gpu_engines.m_is_copy_vector_on.exchange_acq_rel(false)) {
        mask_vector(gpu_engines.m_bar0, gpu_engines.m_copy_vector);
    }

    if (gpu_engines.m_is_graphics_vector_on.exchange_acq_rel(false)) {
        mask_vector(gpu_engines.m_bar0, gpu_engines.m_graphics_vector);
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(gpu_engines.m_queues_lock);
        while (!gpu_engines.m_queues.empty()) {
            gpu_engines.m_queues.pop_front();
        }

        sync::spin_unlock_irqrestore(gpu_engines.m_queues_lock, irq);
    });

    RUN_ELEVATED(gpu::device_failed(gpu_engines.m_device));
}

void engines::free_user_channels() {
    uint32_t freed = 0;
    for (backend_queue& queue : m_queues) {
        // A failed GPU's firmware is sent no further request, so its user channels run on
        if (m_failed.is_set()) {
            break;
        }

        if (queue.user_channel.destroy_keeping_memory() == OK) {
            freed++;
        }
    }

    if (freed != 0) {
        log::info(
            "nvidia: the firmware freed %u user %s once the GPU core's device failed",
            freed,
            freed == 1 ? "channel" : "channels"
        );
    }
}

void engines::on_channel_error(channel_error error) {
    error_operation* attribution = nullptr;
    switch (error) {
        case channel_error::fault:
            attribution = &m_fault_attribution;
            break;
        case channel_error::engine:
            attribution = &m_engine_error_attribution;
            break;
    }

    RUN_ELEVATED(m_operations.post_unless_queued(*attribution));
}

void engines::attribute_error_on_task(operation& op) {
    auto& attribution = static_cast<error_operation&>(op);
    engines& gpu_engines = *attribution.owner;

    // The failure's report ends every queue, and a failed GPU's channel RAM may read anything
    if (gpu_engines.has_failed()) {
        return;
    }

    bool is_mmu_fault = attribution.reason == GPU_LOST_FAULT;
    uint32_t faulted = 0;
    uint32_t attributed_before = 0;
    for (backend_queue& queue : gpu_engines.m_queues) {
        // A report the firmware sends during a record's read queues this attribution again before the stops it asks
        // for, and the lost queue's record and channel RAM still read faulted until its stop
        if (queue.is_attributed) {
            attributed_before++;
            continue;
        }

        // Only the fault record names the graphics engine's MMU fault, which sets neither fault bit in channel RAM. A
        // record's reply missing its deadline fails the GPU, and the failure's report ends every queue
        uint64_t fault_va = 0;
        bool is_recorded = is_mmu_fault && queue.user_channel.read_mmu_fault(fault_va);
        if (gpu_engines.has_failed()) {
            gpu_engines.report_failure();
            return;
        }

        if (is_recorded || queue.user_channel.has_faulted()) {
            RUN_ELEVATED(gpu::space_lost(queue.space->core, attribution.reason, fault_va));
            queue.is_attributed = true;
            faulted++;
        }
    }

    const char* reported = is_mmu_fault ? "an MMU fault" : "an engine error";
    if (faulted == 0 && attributed_before > 0) {
        return;
    }

    if (faulted == 0) {
        log::warn(
            "nvidia: the firmware reported %s, but no queue's channel reads faulted, so the GPU core lost no context",
            reported
        );
        return;
    }

    log::info(
        "nvidia: the firmware reported %s, and the GPU core lost the context of each faulted queue, %u in all",
        reported,
        faulted
    );
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

    // A failed GPU may still walk the tables and write the semaphores, so they stay with the firmware's object
    bool is_destroyed = !gpu_engines.has_failed() && space->addresses.destroy() == OK;
    if (is_destroyed) {
        RUN_ELEVATED(dma::free_pages(space->semaphores));
    }

    heap::ufree_delete(space);

    if (gpu_engines.has_failed()) {
        gpu_engines.report_failure();
    }
}

void engines::create_queue_on_task(operation& op) {
    auto& creation = static_cast<queue_operation&>(op);
    engines& gpu_engines = *creation.owner;

    // A queue listed after the failure's report would stay on the handler's list for good, and its own buffers were
    // never mapped
    if (gpu_engines.has_failed()) {
        gpu_engines.release_own_buffers(*creation.queue);
        op.result = ERR_BAD_STATE;
        return;
    }

    op.result = gpu_engines.start_queue(*creation.queue);
    if (op.result != OK) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(gpu_engines.m_queues_lock);
        gpu_engines.m_queues.push_back(creation.queue);
        sync::spin_unlock_irqrestore(gpu_engines.m_queues_lock, irq);
    });
}

void engines::stop_queue_on_task(operation& op) {
    auto& stop = static_cast<queue_operation&>(op);
    engines& gpu_engines = *stop.owner;
    backend_queue* queue = stop.queue;

    // The failure's report ends the queue, as the GPU may still run it
    if (gpu_engines.has_failed()) {
        gpu_engines.report_failure();
        return;
    }

    // A channel whose release failed may still run, so the device's failure ends the queue in place of its stop
    if (queue->user_channel.destroy() != OK) {
        if (gpu_engines.has_failed()) {
            gpu_engines.report_failure();
        } else {
            gpu_engines.fail_device("releasing a queue's channel failed");
        }

        return;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(gpu_engines.m_queues_lock);
        gpu_engines.m_queues.remove(queue);
        sync::spin_unlock_irqrestore(gpu_engines.m_queues_lock, irq);
    });

    RUN_ELEVATED(gpu::queue_stopped(queue->core));
}

void engines::destroy_queue_on_task(operation& op) {
    auto& destroy = static_cast<queue_operation&>(op);
    engines& gpu_engines = *destroy.owner;
    backend_queue* queue = destroy.queue;

    // The GPU may still reach the queue's memory, and a stop racing the failure may still be queued in `task_operation`
    if (gpu_engines.has_failed()) {
        return;
    }

    gpu_engines.release_queue(*queue, true);
    heap::ufree_delete(queue);

    if (gpu_engines.has_failed()) {
        gpu_engines.report_failure();
    }
}

int32_t engines::start_queue(backend_queue& queue) {
    backend_space& space = *queue.space;
    int32_t rc = map_semaphores(space);
    if (rc != OK) {
        release_own_buffers(queue);
        return rc;
    }

    uint32_t free_slots = ~space.queue_slots & ((1u << GPU_QUEUES_MAX) - 1);
    if (free_slots == 0 || !take_channel_id(queue.channel_id)) {
        release_own_buffers(queue);
        return ERR_NO_MEMORY;
    }

    queue.slot = static_cast<uint32_t>(__builtin_ctz(free_slots));
    space.queue_slots |= 1u << queue.slot;

    // The slot's semaphore may hold a value written before this queue took the slot
    mmio::write32(semaphore_virt(queue), 0);

    int32_t dma_rc = dma::OK;
    RUN_ELEVATED({
        dma_rc = dma::alloc_pages(
            m_card, PUSH_BUFFER_PAGES, dma::caching::cached_when_coherent, paging::PAGE_USER, queue.push_buffer
        );
    });

    rc = dma_rc == dma::OK ? OK : ERR_NO_MEMORY;
    bool is_mapped = false;
    if (rc == OK) {
        memory_run push_buffer = {.address = queue.push_buffer.phys, .bytes = PUSH_BUFFER_PAGES * pmm::PAGE_SIZE};
        page_mapping read_only = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = true};
        rc = space.addresses.page_tables().map(slot_push_buffer_address(queue.slot), &push_buffer, 1, read_only);
        is_mapped = rc == OK;
    }

    if (rc == OK && queue.kind == GPU_QUEUE_GRAPHICS) {
        rc = start_graphics_channel(queue);
    } else if (rc == OK) {
        channel_setup setup = {
            .engine = *m_table.first_async_copy_engine(),
            .id = queue.channel_id,
            .gpfifo_address = slot_gpfifo_address(queue.slot),
            .is_privileged = false,
        };

        rc = queue.user_channel.create(m_card, *m_chip, space.addresses, setup);
    }

    if (rc != OK) {
        release_queue(queue, is_mapped);
    }

    return rc;
}

int32_t engines::start_graphics_channel(backend_queue& queue) {
    backend_space& space = *queue.space;
    context_page_tables& tables = space.addresses.page_tables();
    if (!space.are_global_buffers_mapped) {
        int32_t rc = map_global_buffers(tables, m_global_buffers);
        if (rc != OK) {
            return rc;
        }

        space.are_global_buffers_mapped = true;
    }

    place_own_buffers(m_graphics_layouts, m_own_buffers_start, queue.slot, queue.own_buffers);
    int32_t rc = map_own_buffers(tables, queue.own_buffers);

    channel_setup setup = {
        .engine = *m_table.graphics_engine(),
        .id = queue.channel_id,
        .gpfifo_address = slot_gpfifo_address(queue.slot),
        .is_privileged = false,
    };

    bool is_allocated = false;
    if (rc == OK) {
        rc = queue.user_channel.allocate(m_card, *m_chip, space.addresses, setup);
        is_allocated = rc == OK;
    }

    if (rc == OK) {
        rc = promote_graphics_context(
            m_firmware,
            m_objects.subdevice,
            queue.user_channel.handle(),
            m_global_buffers,
            queue.own_buffers,
            false
        );
    }

    if (rc == OK) {
        rc = queue.user_channel.add_object(m_chip->graphics.three_d_class, nullptr, 0);
    }

    if (rc == OK) {
        rc = queue.user_channel.add_object(m_chip->graphics.compute_class, nullptr, 0);
    }

    if (rc == OK) {
        rc = queue.user_channel.add_copy_object(*m_chip, m_table.graphics_copy_engine()->type);
    }

    if (rc == OK) {
        rc = queue.user_channel.schedule();
    }

    if (rc != OK && is_allocated) {
        queue.user_channel.destroy();
    }

    return rc;
}

int32_t engines::map_semaphores(backend_space& space) {
    if (space.semaphores.virt != 0) {
        return OK;
    }

    int32_t dma_rc = dma::OK;
    RUN_ELEVATED({
        dma_rc = dma::alloc_pages(m_card, 1, dma::caching::cached_when_coherent, paging::PAGE_USER, space.semaphores);
    });

    if (dma_rc != dma::OK) {
        return ERR_NO_MEMORY;
    }

    memory_run page = {.address = space.semaphores.phys, .bytes = pmm::PAGE_SIZE};
    page_mapping writable = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = false};
    int32_t rc = space.addresses.page_tables().map(SEMAPHORES_ADDRESS, &page, 1, writable);

    // A failed update set the failed mark, and the card may then write the page
    if (rc != OK && !has_failed()) {
        RUN_ELEVATED(dma::free_pages(space.semaphores));
    }

    return rc;
}

void engines::release_queue(backend_queue& queue, bool is_mapped) {
    bool gpu_may_reach = queue.user_channel.keeps_memory() || has_failed();
    if (is_mapped && !gpu_may_reach) {
        uint64_t address = slot_push_buffer_address(queue.slot);
        uint64_t bytes = PUSH_BUFFER_PAGES * pmm::PAGE_SIZE;
        gpu_may_reach = queue.space->addresses.page_tables().unmap(address, bytes) != OK;
    }

    if (!gpu_may_reach) {
        gpu_may_reach = !release_own_buffers(queue);
    }

    if (gpu_may_reach) {
        return;
    }

    RUN_ELEVATED(dma::free_pages(queue.push_buffer));
    queue.space->queue_slots &= ~(1u << queue.slot);
    give_back_channel_id(queue.channel_id);
}

int32_t engines::allocate_own_buffers(backend_queue& queue) {
    int32_t rc = allocate_context_buffer(m_graphics_layouts.main, queue.own_buffers.main);
    if (rc == OK) {
        rc = allocate_context_buffer(m_graphics_layouts.patch, queue.own_buffers.patch);
    }

    // The GPU's failure may have ended the zeroing, and its cause is logged where it happened
    if (rc == ERR_TIMEOUT && has_failed()) {
        report_failure();
    } else if (rc == ERR_TIMEOUT) {
        fail_device("the kernel's copy channel hung zeroing a graphics context buffer");
    }

    // A main buffer whose zeroing ended is reached by no copy, even when the patch buffer's hung
    if (rc != OK && queue.own_buffers.main.range.size != 0) {
        m_memory.release(queue.own_buffers.main.range);
        queue.own_buffers.main.range = {};
    }

    return rc;
}

bool engines::release_own_buffers(backend_queue& queue) {
    context_buffer* own[] = {&queue.own_buffers.main, &queue.own_buffers.patch};
    for (context_buffer* buffer : own) {
        if (buffer->range.size == 0) {
            continue;
        }

        // A buffer is placed only once its queue holds a slot, so one at address 0 was never mapped
        bool is_unmapped = buffer->gpu_address == 0
            || queue.space->addresses.page_tables().unmap(buffer->gpu_address, buffer->range.size) == OK;
        if (!is_unmapped) {
            return false;
        }

        m_memory.release(buffer->range);
        buffer->range = {};
    }

    return true;
}

bool engines::take_channel_id(uint32_t& out) {
    for (uint32_t id = COPY_CHANNEL_ID + 1; id < CHANNEL_IDS; id++) {
        uint64_t bit = 1ULL << (id % CHANNEL_IDS_PER_WORD);
        if ((m_channel_ids[id / CHANNEL_IDS_PER_WORD] & bit) == 0) {
            m_channel_ids[id / CHANNEL_IDS_PER_WORD] |= bit;
            out = id;
            return true;
        }
    }

    return false;
}

void engines::give_back_channel_id(uint32_t id) {
    m_channel_ids[id / CHANNEL_IDS_PER_WORD] &= ~(1ULL << (id % CHANNEL_IDS_PER_WORD));
}

} // namespace drivers::nvidia
