// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/string.h"
#include "dma/dma.h"
#include "drivers/device_driver.h"
#include "drivers/graphics/nvidia/engines.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "dynpriv/dynpriv.h"
#include "gpu/core.h"
#include "mm/heap.h"
#include "mm/shmem.h"
#include "tests/framework/helpers.h"
#include "tests/graphics/nvidia_channel_test_access.h"
#include "tests/graphics/nvidia_copy_channel_test_access.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_engines);

using drivers::nvidia::backend_memory;
using drivers::nvidia::backend_queue;
using drivers::nvidia::backend_space;
using drivers::nvidia::card_identity;
using drivers::nvidia::channel_test_access;
using drivers::nvidia::chip_info;
using drivers::nvidia::context_tlbs;
using drivers::nvidia::copy_channel;
using drivers::nvidia::copy_channel_test_access;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::engine_info;
using drivers::nvidia::engines;
using drivers::nvidia::entry_run;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::failed_mark;
using drivers::nvidia::firmware;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::memory_report;
using drivers::nvidia::OK;
using drivers::nvidia::operation_queue;
using drivers::nvidia::registers;
using drivers::nvidia::table_writes;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

// NVIDIA's controls, the device info table's layout and engine types, copy engine 2's row in the kernel interrupt
// table, and BAR0's CPU interrupt tree and doorbell, restated so the tests hold the engines to NVIDIA's values
static constexpr uint32_t GET_DEVICE_INFO_TABLE        = 0x20801112;
static constexpr uint32_t GET_KERNEL_TABLE             = 0x20800A5C;
static constexpr uint32_t GET_FAULT_METHOD_BUFFER_SIZE = 0x20802A08;
static constexpr size_t   DEVICE_INFO_ENTRY_BYTES      = 100;
static constexpr size_t   DEVICE_INFO_FIRST_ENTRY      = 12;
static constexpr uint32_t ENGINE_TYPE_WORD             = 2;
static constexpr uint32_t RUNLIST_WORD                 = 3;
static constexpr uint32_t GR0                          = 0x1;
static constexpr uint32_t CE0                          = 0x9;
static constexpr uint32_t CE1                          = 0xA;
static constexpr uint32_t CE2                          = 0xB;
static constexpr uint16_t MC_ENGINE_IDX_CE2            = 17;
static constexpr uint32_t CPU_INTR_LEAF                = 0xB81000;
static constexpr uint32_t CPU_INTR_LEAF_EN_SET         = 0xB81200;
static constexpr uint32_t CPU_INTR_LEAF_EN_CLEAR       = 0xB81400;
static constexpr uint32_t VIRTUAL_FUNCTION_DOORBELL    = 0xBB0090;
static constexpr uint32_t USERD_GP_PUT                 = 0x8C;

// The methods the GPU's double runs, from NVIDIA's clc56f.h for the channel and clc7b5.h for the copy engine, each sent
// with DMA_SEC_OP INC_METHOD
static constexpr uint32_t INC_METHOD          = 1;
static constexpr uint32_t NON_STALL_INTERRUPT = 0x20;
static constexpr uint32_t SEM_PAYLOAD_LO      = 0x64;
static constexpr uint32_t SEM_EXECUTE         = 0x6C;
static constexpr uint32_t LAUNCH_DMA          = 0x300;
static constexpr uint32_t OFFSET_OUT_UPPER    = 0x408;
static constexpr uint32_t OFFSET_OUT_LOWER    = 0x40C;
static constexpr uint32_t LINE_LENGTH_IN      = 0x418;
static constexpr uint32_t SET_REMAP_CONST_A   = 0x700;

// Where the copy channel maps its push buffer, and the bytes of each of its slots
static constexpr uint64_t PUSH_BUFFER_ADDRESS = (1ULL << 39) + 0x20000;
static constexpr uint64_t SLOT_BYTES          = copy_channel_test_access::SLOT_WORDS * sizeof(uint32_t);

// The check's words, the 64 KiB the copies fill and then the page they must leave, and what the second copy fills
static constexpr uint64_t FILLED_WORDS       = 64 * 1024 / sizeof(uint32_t);
static constexpr uint64_t CHECK_WORDS        = FILLED_WORDS + 4096 / sizeof(uint32_t);
static constexpr uint32_t CHECK_FILL_PATTERN = 0xC3C3C3C3;

static constexpr gpu_objects TEST_OBJECTS    = {.device = 0x00050001, .subdevice = 0x00050002};
static constexpr rm::handle  FIRST_HANDLE    = 0x00050010;
static constexpr uint32_t    TEST_LEAF_COUNT = 8;
static constexpr uint32_t    STALE_WORD      = 0xA5A5A5A5;

// Copy engine 2's nonstall vector on the RTX 3080, bit 7 of leaf 0, and another vector of that leaf
static constexpr uint32_t COPY_VECTOR      = 7;
static constexpr uint32_t COPY_VECTOR_BIT  = 1u << 7;
static constexpr uint32_t OTHER_VECTOR_BIT = 1u << 0;

// The firmware objects the copy channel's teardown releases: its copy object, channel, channel group and address space
static constexpr uint32_t COPY_CHANNEL_OBJECTS = 4;

// Copy engines only on the graphics engine's runlist, which run only inside a graphics context
static constexpr engine_info SHARED_COPY_ENGINES[] = {
    {.type = GR0, .runlist = 0},
    {.type = CE0, .runlist = 0},
    {.type = CE1, .runlist = 0},
};

// The RTX 3080's first engines, with copy engine 2 on a runlist of its own
static constexpr engine_info RTX_3080_FIRST_ENGINES[] = {
    {.type = GR0, .runlist = 0},
    {.type = CE0, .runlist = 0},
    {.type = CE1, .runlist = 0},
    {.type = CE2, .runlist = 1},
};

namespace drivers::nvidia {

// Reaches the engines' copy channel, as the card's interrupt handler does once bring-up publishes the copy vector
struct engines_test_access {
    static copy_channel* published_copy_channel(engines& gpu_engines) {
        return gpu_engines.m_is_copy_vector_on.load_acquire() ? gpu_engines.m_copy_channel : nullptr;
    }

    static copy_channel* copy_channel_of(engines& gpu_engines) { return gpu_engines.m_copy_channel; }

    // Gives `gpu_engines` the BAR0, chip and card that bring-up and registration would, without either
    static void describe(engines& gpu_engines, registers bar0, const chip_info& chip, const card_identity& identity) {
        gpu_engines.m_bar0 = bar0;
        gpu_engines.m_chip = &chip;
        gpu_engines.m_identity = identity;
    }

    // Gives `gpu_engines` the GPU core's device that registration would
    static void register_with(engines& gpu_engines, gpu::device& dev) { gpu_engines.m_device = &dev; }

    static void hold_every_channel_id(engines& gpu_engines) {
        for (uint64_t& word : gpu_engines.m_channel_ids) {
            word = ~0ULL;
        }
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::engines_test_access;

// Acts as the GSP's firmware: answers the device info table from `engines`, the kernel interrupt table with copy engine
// 2's row unless `kernel_table_result` is an error, the fault method buffer's size, and the rest with `OK`
class engines_firmware : public firmware {
public:
    const engine_info* engines = nullptr;
    uint32_t           engine_count = 0;
    int32_t            kernel_table_result = OK;
    int32_t            allocation_result = OK; // what every allocation answers
    uint32_t           refused_class = ~0u;    // a class whose allocations the firmware refuses
    int32_t            release_result = OK;    // what every release answers
    uint32_t           table_reads = 0;
    uint32_t           other_controls = 0;
    uint32_t           allocations = 0;
    uint32_t           releases = 0;
    uint32_t           internal_controls = 0;
    uint32_t           internal_command = 0;
    uint32_t           last_class = 0;
    uint32_t           last_command = 0; // of the controls other than the table's read
    rm::channel_params last_channel = {};
    rm::handle         next_handle = FIRST_HANDLE;

    int32_t allocate(rm::handle, rm::handle, uint32_t class_id, void* params, size_t size) override {
        allocations++;
        last_class = class_id;
        if (size == sizeof(rm::channel_params)) {
            string::memcpy(&last_channel, params, size);
        }

        return class_id == refused_class ? ERR_FIRMWARE : allocation_result;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        bool is_table_read = object == TEST_OBJECTS.subdevice && command == GET_DEVICE_INFO_TABLE;
        if (is_table_read) {
            table_reads++;
            write_table(static_cast<uint8_t*>(params));
            return OK;
        }

        other_controls++;
        last_command = command;
        if (command == GET_FAULT_METHOD_BUFFER_SIZE && size == sizeof(uint32_t)) {
            uint32_t bytes = 20480;
            string::memcpy(params, &bytes, sizeof(bytes));
        }

        return OK;
    }

    int32_t release(rm::handle) override {
        releases++;
        return release_result;
    }

    rm::handle new_handle() override { return next_handle++; }

    rm::handle client() const override { return 0; }

    int32_t internal_control(uint32_t command, void* params, size_t size) override {
        internal_controls++;
        internal_command = command;
        if (kernel_table_result != OK || size != sizeof(rm::kernel_interrupt_table_params)) {
            return kernel_table_result;
        }

        auto* table = static_cast<rm::kernel_interrupt_table_params*>(params);
        table->rows[0] = {MC_ENGINE_IDX_CE2, 0, 0, COPY_VECTOR};
        table->row_count = 1;
        return OK;
    }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

private:
    void write_table(uint8_t* bytes) const {
        for (uint32_t i = 0; i < engine_count; i++) {
            uint8_t* entry = bytes + DEVICE_INFO_FIRST_ENTRY + i * DEVICE_INFO_ENTRY_BYTES;
            string::memcpy(entry + ENGINE_TYPE_WORD * 4, &engines[i].type, 4);
            string::memcpy(entry + RUNLIST_WORD * 4, &engines[i].runlist, 4);
        }

        string::memcpy(bytes + 4, &engine_count, sizeof(engine_count));
        bytes[8] = 0;
    }
};

// The check's memory as BAR1 shows it to the CPU, which the copy engine's double fills
alignas(4096) static uint32_t s_check_memory[CHECK_WORDS];

// The channel's instance block, which the channel clears through `map_for_cpu`
alignas(4096) static uint8_t s_engines_instance_block[4096];

// Where the double's BAR1 starts in physical addresses, for the memory the backend creates
static constexpr uint64_t BAR1_PHYS = 0x80000000;

// Where a test's space places its tables once the test moves the double's next offset there, past every other
// allocation, and the eight 4 KiB tables from there as the updates of GPU context tables wrote them
static constexpr uint64_t TABLES_OFFSET   = 0x40000000;
static constexpr uint64_t TABLE_WORDS     = 512;
static constexpr uint64_t IMAGE_WORDS     = 8 * TABLE_WORDS;
static constexpr uint64_t UNWRITTEN_ENTRY = 0xA5A5A5A5A5A5A5A5;
static uint64_t           s_tables_image[IMAGE_WORDS];

// Hands out video memory from 4 MiB on, maps the check's range onto `s_check_memory`, an instance block onto
// `s_engines_instance_block` and a created range at `BAR1_PHYS` past its offset, counting maps, unmaps and releases
class engines_video_memory : public video_memory {
public:
    memory_report reported = {};
    bool          refuses_allocation = false;
    bool          is_window_full = false;    // refuses to map a created range
    bool          maps_without_phys = false; // maps a created range with no physical address
    int32_t       update_result = OK;        // what table updates answer once they wrote `s_tables_image`

    uint64_t   next_offset = 0x400000;
    uint64_t   check_offset = 0;
    uint32_t   allocations = 0;
    vram_range last_allocated = {};
    vram_use   last_use = vram_use::general;
    uint32_t   check_unmaps = 0;
    uint32_t   check_releases = 0;
    uint32_t   created_maps = 0;
    uint32_t   created_unmaps = 0;
    uint32_t   created_releases = 0;
    vram_range last_released = {};

    uint32_t table_updates = 0;
    uint32_t table_releases = 0;        // of tables from `TABLES_OFFSET` on
    uint64_t invalidated_directory = 0; // the root directory of the last update that invalidated

    void report(memory_report& out) override {
        out = reported;
    }

    int32_t allocate(uint64_t size, uint64_t, vram_use use, vram_range& out) override {
        if (refuses_allocation) {
            return ERR_NO_MEMORY;
        }

        allocations++;
        out = {.offset = next_offset, .size = size};
        if (size == sizeof(s_check_memory)) {
            check_offset = next_offset;
        }

        next_offset += size;
        last_allocated = out;
        last_use = use;
        return OK;
    }

    void release(const vram_range& range) override {
        if (is_check(range)) {
            check_releases++;
        } else if (is_created(range)) {
            created_releases++;
            last_released = range;
        } else if (range.offset >= TABLES_OFFSET) {
            table_releases++;
        }
    }

    int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) override {
        if (is_check(range)) {
            out = {.virt = reinterpret_cast<uintptr_t>(s_check_memory), .phys = 0};
            return OK;
        }

        if (!is_created(range)) {
            out = {.virt = reinterpret_cast<uintptr_t>(s_engines_instance_block), .phys = 0};
            return OK;
        }

        if (is_window_full) {
            return ERR_NO_MEMORY;
        }

        created_maps++;
        out = {.virt = 0, .phys = maps_without_phys ? 0 : BAR1_PHYS + range.offset};
        return OK;
    }

    void unmap_for_cpu(const vram_range& range) override {
        if (is_check(range)) {
            check_unmaps++;
        } else if (is_created(range)) {
            created_unmaps++;
        }
    }

    int32_t update_context_tables(uint64_t directory_offset, table_writes& writes, context_tlbs tlbs) override {
        table_updates++;
        if (tlbs == context_tlbs::invalidated) {
            invalidated_directory = directory_offset;
        }

        entry_run run = {};
        while (writes.next(run)) {
            uint64_t first = (run.vram_offset - TABLES_OFFSET) / sizeof(uint64_t);
            if (run.vram_offset >= TABLES_OFFSET && first + run.count <= IMAGE_WORDS) {
                string::memcpy(&s_tables_image[first], run.entries, run.count * sizeof(uint64_t));
            }
        }

        return update_result;
    }

private:
    bool is_check(const vram_range& range) const {
        return check_offset != 0 && range.offset == check_offset && range.size == sizeof(s_check_memory);
    }

    // Not the check's range and larger than an instance block, a size only the backend's creates ask for
    bool is_created(const vram_range& range) const {
        return !is_check(range) && range.size > sizeof(s_engines_instance_block);
    }
};

// A driver whose task the tests stand in for, serving its operation queue themselves
class backend_task_driver : public drivers::device_driver {
public:
    backend_task_driver() : device_driver("backend_task") {}

    int32_t attach() override { return 0; }
    void run() override {}
};

// The operation queue every fixture's engines take and its driver, built in place at the first fixture
alignas(backend_task_driver) static uint8_t s_task_driver_bytes[sizeof(backend_task_driver)];
alignas(operation_queue) static uint8_t s_operations_bytes[sizeof(operation_queue)];
static operation_queue* s_operations = nullptr;

static operation_queue& engines_operations() {
    if (s_operations == nullptr) {
        auto* driver = new (s_task_driver_bytes) backend_task_driver();
        s_operations = new (s_operations_bytes) operation_queue(*driver);
    }

    return *s_operations;
}

static constexpr gpu::device_facts NO_FACTS = {};

// The GPU core's device each fixture's engines report to, never published, so each fixture builds it anew in place
alignas(gpu::device) static uint8_t s_core_device_bytes[sizeof(gpu::device)];

// BAR0 from the CPU interrupt tree's first leaf to the doorbell, the registers bring-up and the handler's service use
static constexpr uint32_t BAR0_WORDS = (VIRTUAL_FUNCTION_DOORBELL - CPU_INTR_LEAF) / sizeof(uint32_t) + 1;
static uint32_t s_bar0[BAR0_WORDS];

// A view of BAR0 over `s_bar0`, every word seeded with `STALE_WORD`
static registers fresh_bar0() {
    for (uint32_t i = 0; i < BAR0_WORDS; i++) {
        s_bar0[i] = STALE_WORD;
    }

    return registers(reinterpret_cast<uintptr_t>(s_bar0) - CPU_INTR_LEAF);
}

static uint32_t& bar0_word(uint32_t offset) {
    return s_bar0[(offset - CPU_INTR_LEAF) / sizeof(uint32_t)];
}

static bool is_bar0_untouched() {
    for (uint32_t i = 0; i < BAR0_WORDS; i++) {
        if (s_bar0[i] != STALE_WORD) {
            return false;
        }
    }

    return true;
}

static dma::device_profile engines_test_card() {
    return dma::device_profile_test_access::build(64, true);
}

static chip_info engines_test_chip() {
    chip_info chip = {};
    chip.interrupt_leaf_count = TEST_LEAF_COUNT;
    return chip;
}

static uint32_t put_of(const copy_channel& copier) {
    return channel_test_access::userd_word(copy_channel_test_access::queue(copier), USERD_GP_PUT);
}

// How the copy engine's double misbehaves in one fill: it leaves a word unwritten, writes one past the fill, or never
// finishes, which hangs the channel as a fill past its deadline does
enum class copy_fault : uint8_t { none, skips_word, writes_past, hangs };

// A fill the copy engine's double ran
struct ran_fill {
    uint64_t offset;
    uint32_t words;
    uint32_t value;
};

// The method values one GPFIFO entry sets, as the copy engine and the channel keep them
struct entry_methods {
    uint32_t fill_value = 0;
    uint64_t fill_offset = 0;
    uint32_t fill_words = 0;
    uint32_t payload = 0;
    bool     raises_interrupt = false;
};

static constexpr uint32_t MAX_FILLS = 8;

// The bytes a create asks for unless its test sets others, more than an instance block, so the double maps them at
// `BAR1_PHYS`
static constexpr uint64_t CREATED_BYTES = 128 * 1024;

// The engines over doubles with the test runner as the GPU, running each entry the copy channel publishes and raising
// the copy vector. Fill `faulty_fill`, counted from 1, has `fault` at word `fault_word` of the check's memory
struct engines_fixture {
    engines_firmware     fw;
    engines_video_memory memory;
    failed_mark          failed;
    registers            bar0;
    chip_info            chip = engines_test_chip(); // outlives the engines, which keep it
    dma::device_profile  card = engines_test_card();
    engines              gpu_engines;
    gpu::device&         core_device;

    copy_fault fault = copy_fault::none;
    uint32_t   faulty_fill = 0;
    uint64_t   fault_word = 0;

    ran_fill fills[MAX_FILLS] = {};
    uint32_t fill_count = 0;
    uint32_t strays = 0; // entries outside the push buffer and fills past `MAX_FILLS`, which the double skips
    uint32_t acknowledged = 0;
    uint32_t next_entry = 0; // the GPFIFO entry the GPU's double runs next
    uint64_t took_ns = 0;

    // The create `create_on_gpu` makes, and the memory and device id it returned
    uint64_t     create_size = CREATED_BYTES;
    uint32_t     create_placement = GPU_PLACEMENT_VIDEO;
    uint32_t     create_flags = 0;
    gpu::memory* created = nullptr;
    uint32_t     device_id = ~0u;

    // The space `create_space_on_gpu` creates, and the queue `create_queue_on_gpu` creates in it
    gpu::space* space = nullptr;
    gpu::queue* queue = nullptr;

    engines_fixture()
        : bar0(fresh_bar0()), gpu_engines(fw, TEST_OBJECTS, memory, engines_operations(), failed),
          core_device(*new (s_core_device_bytes) gpu::device("gpu_engines", &gpu_engines, NO_FACTS)) {
        engines_test_access::register_with(gpu_engines, core_device);
        fw.engines = RTX_3080_FIRST_ENGINES;
        fw.engine_count = 4;

        for (uint64_t i = 0; i < CHECK_WORDS; i++) {
            s_check_memory[i] = STALE_WORD;
        }

        for (uint64_t i = 0; i < IMAGE_WORDS; i++) {
            s_tables_image[i] = UNWRITTEN_ENTRY;
        }
    }

    ~engines_fixture() {
        // A record left queued would reach these engines from a later fixture's serve
        engines_operations().serve();

        copy_channel* copier = engines_test_access::copy_channel_of(gpu_engines);
        if (copier != nullptr) {
            copier->destroy();
        }
    }

    int32_t bring_up() { return gpu_engines.bring_up(bar0, card, chip); }

    int32_t create() {
        return gpu_engines.create_memory(create_size, create_placement, create_flags, nullptr, 0, &created, &device_id);
    }

    // Runs each entry the copy channel publishes and serves the driver task's operations once the task waits for them,
    // until the task ends
    bool act_as_gpu(sched::task& task, const sync::atomic<uint32_t>& is_done) {
        uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
        while (!is_done.load_acquire()) {
            if (clock::now_ns() > deadline) {
                return false;
            }

            // The channel is read only while the task waits, since a failed check destroys it
            bool is_waiting = task.state.load_acquire() == sched::TASK_STATE_BLOCKED;
            copy_channel* copier = is_waiting ? engines_test_access::published_copy_channel(gpu_engines) : nullptr;
            if (copier != nullptr && put_of(*copier) != next_entry) {
                run_entry(*copier, next_entry);
                next_entry++;
            }

            if (is_waiting) {
                engines_operations().serve();
            }
        }

        return true;
    }

    // Runs GPFIFO entry `index`, fill `index + 1`, as the GPU would, then raises the copy engine's vector as the copy's
    // release does and calls the handler's service
    void run_entry(copy_channel& copier, uint32_t index) {
        uint32_t fill_number = index + 1;
        if (fault == copy_fault::hangs && fill_number == faulty_fill) {
            copy_channel_test_access::hang(copier);
            return;
        }

        const drivers::nvidia::channel& queue = copy_channel_test_access::queue(copier);
        uint32_t low = channel_test_access::gpfifo_word(queue, index, 0);
        uint32_t high = channel_test_access::gpfifo_word(queue, index, 1);
        uint64_t address = (static_cast<uint64_t>(high & 0xFF) << 32) | (low & ~3u);
        uint32_t word_count = (high >> 10) & 0x1FFFFF;

        bool is_in_slot = address >= PUSH_BUFFER_ADDRESS && (address - PUSH_BUFFER_ADDRESS) % SLOT_BYTES == 0;
        uint64_t slot = (address - PUSH_BUFFER_ADDRESS) / SLOT_BYTES;
        if (!is_in_slot || slot >= drivers::nvidia::COPY_SLOTS || word_count > copy_channel_test_access::SLOT_WORDS) {
            strays++;
            return;
        }

        entry_methods methods = {};
        uint32_t i = 0;
        while (i < word_count) {
            uint32_t header = copy_channel_test_access::slot_word(copier, static_cast<uint32_t>(slot), i);
            uint32_t method = (header & 0xFFF) * sizeof(uint32_t);
            uint32_t count = (header >> 16) & 0x1FFF;
            bool is_incrementing = header >> 29 == INC_METHOD;
            for (uint32_t j = 0; j < count && is_incrementing && i + 1 + j < word_count; j++) {
                uint32_t data = copy_channel_test_access::slot_word(copier, static_cast<uint32_t>(slot), i + 1 + j);
                run_method(copier, method + j * sizeof(uint32_t), data, methods, fill_number);
            }

            i += 1 + count;
        }

        if (!methods.raises_interrupt) {
            return;
        }

        bar0_word(CPU_INTR_LEAF) = COPY_VECTOR_BIT | OTHER_VECTOR_BIT;
        RUN_ELEVATED(gpu_engines.service_interrupt(bar0));
        if (bar0_word(CPU_INTR_LEAF) == COPY_VECTOR_BIT) {
            acknowledged++;
        }
    }

    void run_method(copy_channel& copier, uint32_t method, uint32_t data, entry_methods& methods, uint32_t fill) {
        switch (method) {
            case SET_REMAP_CONST_A:
                methods.fill_value = data;
                break;
            case OFFSET_OUT_UPPER:
                methods.fill_offset = (static_cast<uint64_t>(data) << 32) | (methods.fill_offset & 0xFFFFFFFF);
                break;
            case OFFSET_OUT_LOWER:
                methods.fill_offset = (methods.fill_offset & ~0xFFFFFFFFULL) | data;
                break;
            case LINE_LENGTH_IN:
                methods.fill_words = data;
                break;
            case LAUNCH_DMA:
                launch_fill(methods, fill);
                break;
            case SEM_PAYLOAD_LO:
                methods.payload = data;
                break;
            case SEM_EXECUTE:
                copy_channel_test_access::release(copier, methods.payload);
                break;
            case NON_STALL_INTERRUPT:
                methods.raises_interrupt = true;
                break;
            default:
                break;
        }
    }

    // Records the fill and writes it as the copy engine would where it lies in the check's memory, with this fill's
    // fault, since the double holds no other video memory
    void launch_fill(const entry_methods& methods, uint32_t fill_number) {
        if (fill_count == MAX_FILLS) {
            strays++;
            return;
        }

        fills[fill_count] = {.offset = methods.fill_offset, .words = methods.fill_words, .value = methods.fill_value};
        fill_count++;

        uint64_t first = (methods.fill_offset - memory.check_offset) / sizeof(uint32_t);
        bool is_in_check = methods.fill_offset >= memory.check_offset && first + methods.fill_words <= CHECK_WORDS;
        if (!is_in_check) {
            return;
        }

        uint32_t skipped = s_check_memory[fault_word];
        for (uint64_t i = first; i < first + methods.fill_words; i++) {
            s_check_memory[i] = methods.fill_value;
        }

        bool is_faulty = fill_number == faulty_fill;
        if (is_faulty && fault == copy_fault::skips_word) {
            s_check_memory[fault_word] = skipped;
        }

        if (is_faulty && fault == copy_fault::writes_past) {
            s_check_memory[fault_word] = methods.fill_value;
        }
    }
};

// A step of the engines run by a task, since fills sleep and the test runner, the boot CPU's idle task, must not block
struct engines_task_state {
    engines_fixture*       fixture;
    int32_t                (*step)(engines_fixture&);
    int32_t                result;
    sync::atomic<uint32_t> is_done;
};

// What `run_on_gpu` returns for a step whose task did not finish, a value no error of the driver has
static constexpr int32_t NOT_FINISHED = 1;

static void run_engines_task(void* arg) {
    engines_task_state* state = static_cast<engines_task_state*>(arg);
    state->result = state->step(*state->fixture);
    state->is_done.store_release(1);
    sched::exit(0);
}

// Starts `step` in a task, pinned until `finish_step`
static sched::task* start_step(engines_fixture& fixture, engines_task_state& state, int32_t (*step)(engines_fixture&)) {
    state.fixture = &fixture;
    state.step = step;
    state.result = NOT_FINISHED;
    state.is_done.store_relaxed(0);

    return test_helpers::start_pinned_task(run_engines_task, &state, "engines_step");
}

// Acts as the GPU and the driver task until the step's task ends, and returns the step's result
static int32_t finish_step(engines_fixture& fixture, engines_task_state& state, sched::task* task) {
    if (task == nullptr) {
        return NOT_FINISHED;
    }

    bool is_done = fixture.act_as_gpu(*task, state.is_done);
    test_helpers::unpin(task);

    return is_done ? state.result : NOT_FINISHED;
}

// Runs `step` in a task while the test runner acts as the GPU and the driver task, and returns its result
static int32_t run_on_gpu(engines_fixture& fixture, int32_t (*step)(engines_fixture&)) {
    engines_task_state state;
    uint64_t start_ns = clock::now_ns();
    sched::task* task = start_step(fixture, state, step);
    int32_t rc = finish_step(fixture, state, task);
    fixture.took_ns = clock::now_ns() - start_ns;

    return rc;
}

// Brings the fixture's engines up in a task while the test runner acts as the GPU
static int32_t bring_up_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, [](engines_fixture& f) { return f.bring_up(); });
}

// Creates the fixture's memory in a task while the test runner acts as the GPU
static int32_t create_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, [](engines_fixture& f) { return f.create(); });
}

// Whether the driver task reported the GPU failed to the fixture's core device
static bool has_device_failed(engines_fixture& fixture) {
    bool is_failed = false;
    RUN_ELEVATED(is_failed = fixture.core_device.failed());
    return is_failed;
}

// Creates a space into `fixture.space`, as a step `run_on_gpu` or `start_step` runs
static int32_t create_space_step(engines_fixture& fixture) {
    return fixture.gpu_engines.create_space(nullptr, &fixture.space);
}

// Creates a space in a task while the test runner acts as the GPU and the driver task
static int32_t create_space_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, create_space_step);
}

// Without an asynchronous copy engine, bring-up asks for no vector and creates nothing
TEST(nvidia_engines, bring_up_refuses_a_table_without_an_asynchronous_copy_engine) {
    engines_fixture fixture;
    fixture.fw.engines = SHARED_COPY_ENGINES;
    fixture.fw.engine_count = 3;

    EXPECT_EQ(fixture.bring_up(), ERR_BAD_STATE);
    EXPECT_EQ(fixture.fw.table_reads, 1u);
    EXPECT_EQ(fixture.fw.internal_controls, 0u);
    EXPECT_EQ(fixture.fw.allocations, 0u);
    EXPECT_EQ(fixture.memory.allocations, 0u);
    EXPECT_TRUE(is_bar0_untouched());
}

// The vector is read through the internal client before anything is created, so its error leaves nothing behind
TEST(nvidia_engines, bring_up_reads_the_vector_before_creating_the_channel) {
    engines_fixture fixture;
    fixture.fw.kernel_table_result = ERR_FIRMWARE;

    EXPECT_EQ(fixture.bring_up(), ERR_FIRMWARE);
    EXPECT_EQ(fixture.fw.internal_controls, 1u);
    EXPECT_EQ(fixture.fw.internal_command, GET_KERNEL_TABLE);

    EXPECT_EQ(fixture.fw.other_controls, 0u);
    EXPECT_EQ(fixture.fw.allocations, 0u);
    EXPECT_EQ(fixture.memory.allocations, 0u);
    EXPECT_TRUE(is_bar0_untouched());
}

// Until bring-up enables the copy engine's vector, the handler's service reads and writes no register
TEST(nvidia_engines, service_interrupt_does_nothing_before_the_vector_is_enabled) {
    engines_fixture fixture;
    fixture.fw.engines = SHARED_COPY_ENGINES;
    fixture.fw.engine_count = 3;

    RUN_ELEVATED(fixture.gpu_engines.service_interrupt(fixture.bar0));
    EXPECT_TRUE(is_bar0_untouched());

    EXPECT_EQ(fixture.bring_up(), ERR_BAD_STATE);
    RUN_ELEVATED(fixture.gpu_engines.service_interrupt(fixture.bar0));
    EXPECT_TRUE(is_bar0_untouched());
}

// Bring-up zeroes and then fills the check's 64 KiB through the copy channel, each fill woken by the handler's service,
// then releases the memory and leaves the channel running with its vector enabled
TEST(nvidia_engines, bring_up_zeroes_and_fills_the_checks_memory_through_the_copy_channel) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    EXPECT_LT(fixture.took_ns, GPU_SUBMIT_TIMEOUT_NS);
    EXPECT_EQ(fixture.acknowledged, 2u);
    EXPECT_EQ(fixture.strays, 0u);

    ASSERT_EQ(fixture.fill_count, 2u);
    uint32_t values[] = {0, CHECK_FILL_PATTERN};
    for (uint32_t i = 0; i < 2; i++) {
        EXPECT_EQ(fixture.fills[i].offset, fixture.memory.check_offset);
        EXPECT_EQ(fixture.fills[i].words, FILLED_WORDS);
        EXPECT_EQ(fixture.fills[i].value, values[i]);
    }

    EXPECT_EQ(fixture.memory.check_releases, 1u);
    EXPECT_EQ(fixture.fw.releases, 0u);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_SET), COPY_VECTOR_BIT);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), STALE_WORD);
}

// Whether bring-up, with `fault` at `word` in fill `fill_number`, fails the check after that fill's read back with the
// vector masked, the copy channel destroyed and the check's memory released
static bool fails_the_check(copy_fault fault, uint32_t fill_number, uint64_t word) {
    engines_fixture fixture;
    fixture.fault = fault;
    fixture.faulty_fill = fill_number;
    fixture.fault_word = word;

    int32_t rc = bring_up_on_gpu(fixture);
    bool has_released_all = fixture.memory.check_releases == 1 && fixture.fw.releases == COPY_CHANNEL_OBJECTS;
    bool is_masked = bar0_word(CPU_INTR_LEAF_EN_CLEAR) == COPY_VECTOR_BIT;
    return rc == ERR_BAD_STATE && fixture.fill_count == fill_number && has_released_all && is_masked;
}

// A copy leaving a word unwritten fails the check, whether it is the zeroing's first word or the second fill's last
TEST(nvidia_engines, bring_up_fails_the_check_on_a_word_a_copy_left) {
    EXPECT_TRUE(fails_the_check(copy_fault::skips_word, 1, 0));
    EXPECT_TRUE(fails_the_check(copy_fault::skips_word, 2, FILLED_WORDS - 1));
}

// A copy writing past its bytes fails the check, onto the guard page's first word in the zeroing and its last in the
// second fill
TEST(nvidia_engines, bring_up_fails_the_check_on_a_guard_word_a_copy_changed) {
    EXPECT_TRUE(fails_the_check(copy_fault::writes_past, 1, FILLED_WORDS));
    EXPECT_TRUE(fails_the_check(copy_fault::writes_past, 2, CHECK_WORDS - 1));
}

// A copy that never finishes hangs the channel, so the check's memory stays allocated but unmapped from BAR1, and the
// hung channel stays with its vector masked
TEST(nvidia_engines, bring_up_keeps_the_checks_memory_when_a_copy_hangs) {
    engines_fixture fixture;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 1;

    EXPECT_EQ(bring_up_on_gpu(fixture), ERR_TIMEOUT);
    EXPECT_EQ(fixture.fill_count, 0u);
    EXPECT_EQ(fixture.memory.check_unmaps, 1u);
    EXPECT_EQ(fixture.memory.check_releases, 0u);
    EXPECT_EQ(fixture.fw.releases, 0u);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), COPY_VECTOR_BIT);
}

// GA102's chip as the driver's chip table names it, set by each facts test
static chip_info s_ga102_chip;

// Gives the fixture's engines GA102's chip and an RTX 3080's identity, without bring-up or registration
static void describe_rtx_3080(engines_fixture& fixture) {
    s_ga102_chip.id = 0x172;
    s_ga102_chip.channel_class = 0xC56F;
    s_ga102_chip.copy_class = 0xC7B5;

    card_identity identity = {};

    identity.vendor_id = 0x10DE;
    identity.device_id = 0x2206;
    identity.bus = 1;
    identity.slot = 2;
    identity.function = 3;
    identity.revision = 0xA1;

    identity.gpc_count = 6;
    identity.tpc_count = 34;
    const char name[] = "NVIDIA GeForce RTX 3080";
    string::memcpy(identity.name, name, sizeof(name));

    engines_test_access::describe(fixture.gpu_engines, fixture.bar0, s_ga102_chip, identity);
}

// A video memory report whose four figures differ, so one read for another fails
static constexpr memory_report REPORTED_MEMORY = {0x25F800000, 0x251000000, 0xEF00000, 0xD300000};

// The facts describe the card from its identity, chip and memory, with copy queues only, NVK's user range, and the
// command bytes a queue's GPFIFO holds less one entry per running submission and the entry kept empty
TEST(nvidia_engines, facts_describe_the_card) {
    engines_fixture fixture;
    fixture.memory.reported = REPORTED_MEMORY;
    describe_rtx_3080(fixture);
    gpu::device_facts facts = {};

    fixture.gpu_engines.read_facts(facts);

    EXPECT_EQ(facts.info.backend, static_cast<uint32_t>(GPU_BACKEND_NVIDIA));
    EXPECT_EQ(facts.info.queue_kinds, 1u << GPU_QUEUE_COPY);
    EXPECT_EQ(string::strcmp(facts.info.name, "NVIDIA GeForce RTX 3080"), 0);

    EXPECT_EQ(facts.info.vendor_id, static_cast<uint16_t>(0x10DE));
    EXPECT_EQ(facts.info.device_id, static_cast<uint16_t>(0x2206));
    EXPECT_EQ(facts.info.pci_bus, static_cast<uint8_t>(1));
    EXPECT_EQ(facts.info.pci_slot, static_cast<uint8_t>(2));
    EXPECT_EQ(facts.info.pci_function, static_cast<uint8_t>(3));
    EXPECT_EQ(facts.info.pci_revision, static_cast<uint8_t>(0xA1));

    EXPECT_EQ(facts.info.video_size, REPORTED_MEMORY.video_bytes);
    EXPECT_EQ(facts.info.mappable_video_size, REPORTED_MEMORY.window_bytes);
    EXPECT_EQ(facts.info.va_start, 4096u);
    EXPECT_EQ(facts.info.va_end, 1ULL << 39);
    EXPECT_EQ(facts.info.page_size, 4096u);
    EXPECT_EQ(facts.info.placements, (1u << GPU_PLACEMENT_VIDEO) | (1u << GPU_PLACEMENT_SYSTEM));

    EXPECT_EQ(facts.info.nvidia.chipset, 0x172u);
    EXPECT_EQ(facts.info.nvidia.class_gpfifo, static_cast<uint16_t>(0xC56F));
    EXPECT_EQ(facts.info.nvidia.class_copy, static_cast<uint16_t>(0xC7B5));
    EXPECT_EQ(facts.info.nvidia.gpc_count, static_cast<uint16_t>(6));
    EXPECT_EQ(facts.info.nvidia.tpc_count, static_cast<uint16_t>(34));

    EXPECT_EQ(facts.submit_timeout_ns, 7000000000ULL);
    EXPECT_EQ(facts.running_bytes_max, 261104ULL);
}

// The free memory is the video memory's report at the call, which moves as allocations come and go
TEST(nvidia_engines, free_memory_follows_the_video_memorys_report) {
    engines_fixture fixture;
    fixture.memory.reported = REPORTED_MEMORY;
    describe_rtx_3080(fixture);
    gpu_memory first = {};
    gpu_memory second = {};

    fixture.gpu_engines.read_memory(first);
    fixture.memory.reported.video_free_bytes -= 0x200000;
    fixture.memory.reported.window_free_bytes -= 0x10000;
    fixture.gpu_engines.read_memory(second);

    EXPECT_EQ(first.video_free, REPORTED_MEMORY.video_free_bytes);
    EXPECT_EQ(first.mappable_video_free, REPORTED_MEMORY.window_free_bytes);
    EXPECT_EQ(second.video_free, REPORTED_MEMORY.video_free_bytes - 0x200000);
    EXPECT_EQ(second.mappable_video_free, REPORTED_MEMORY.window_free_bytes - 0x10000);
}

// NVIDIA's NV_VIRTUAL_FUNCTION_TIME_0 and TIME_1, the GPU's clock in nanoseconds
static constexpr uint32_t GPU_TIME_LOW  = 0xBB0080;
static constexpr uint32_t GPU_TIME_HIGH = 0xBB0084;

// The clock joins the high word's 29 bits above the low word as read, dropping the high word's top three bits
TEST(nvidia_engines, timestamp_joins_the_clocks_two_words) {
    engines_fixture fixture;
    describe_rtx_3080(fixture);
    bar0_word(GPU_TIME_LOW) = 0x89ABCDE0;
    bar0_word(GPU_TIME_HIGH) = 0xE0000012;
    uint64_t gpu_ns = 0;
    uint64_t all_ones_low_ns = 0;

    EXPECT_EQ(fixture.gpu_engines.read_timestamp(gpu_ns), gpu::OK);
    bar0_word(GPU_TIME_LOW) = 0xFFFFFFFF;
    EXPECT_EQ(fixture.gpu_engines.read_timestamp(all_ones_low_ns), gpu::OK);

    EXPECT_EQ(gpu_ns, 0x1289ABCDE0ULL);
    EXPECT_EQ(all_ones_low_ns, 0x12FFFFFFFFULL);
}

// A failed GPU's clock is not read, so the call reports the loss instead of what the registers hold
TEST(nvidia_engines, timestamp_refuses_once_the_gpu_failed) {
    engines_fixture fixture;
    describe_rtx_3080(fixture);
    bar0_word(GPU_TIME_LOW) = 0x89ABCDE0;
    bar0_word(GPU_TIME_HIGH) = 0x12;
    fixture.failed.set();
    uint64_t gpu_ns = 0;

    EXPECT_EQ(fixture.gpu_engines.read_timestamp(gpu_ns), gpu::ERR_DEVICE);

    EXPECT_EQ(gpu_ns, 0u);
}

// Video memory is allocated for its use and zeroed whole by the copy channel before its create returns, with no
// mapping for the CPU unless asked, and commands reach it by address, so its device id is 0
TEST(nvidia_engines, video_memory_is_zeroed_by_the_copy_channel_before_its_create_returns) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);

    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    gpu::memory* general = fixture.created;
    vram_range general_range = fixture.memory.last_allocated;
    bool is_general = fixture.memory.last_use == vram_use::general;

    fixture.create_flags = GPU_BUFFER_SCANOUT;
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    vram_range scanout_range = fixture.memory.last_allocated;

    EXPECT_TRUE(is_general);
    EXPECT_TRUE(fixture.memory.last_use == vram_use::scanout);
    EXPECT_EQ(general_range.size, CREATED_BYTES);
    EXPECT_EQ(fixture.device_id, 0u);
    EXPECT_EQ(fixture.memory.created_maps, 0u);
    EXPECT_EQ(fixture.strays, 0u);

    ASSERT_EQ(fixture.fill_count, 4u);
    EXPECT_EQ(fixture.fills[2].offset, general_range.offset);
    EXPECT_EQ(fixture.fills[3].offset, scanout_range.offset);
    for (uint32_t i = 2; i < 4; i++) {
        EXPECT_EQ(fixture.fills[i].words, CREATED_BYTES / sizeof(uint32_t));
        EXPECT_EQ(fixture.fills[i].value, 0u);
    }

    fixture.gpu_engines.release_memory(general);
    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// Video memory past one fill's limit, 4 bytes times its 32-bit line length, is zeroed by as few fills as that allows
TEST(nvidia_engines, video_memory_past_one_fills_limit_is_zeroed_by_the_fewest_fills) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.create_size = 20ull << 30;

    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    uint64_t offset = fixture.memory.last_allocated.offset;

    ASSERT_EQ(fixture.fill_count, 4u);
    EXPECT_EQ(fixture.fills[2].offset, offset);
    EXPECT_EQ(fixture.fills[2].words, 0xFFFFFFFFu);
    EXPECT_EQ(fixture.fills[3].offset, offset + 4ull * 0xFFFFFFFF);
    EXPECT_EQ(fixture.fills[3].words, 0x40000001u);

    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// Mappable video memory stays mapped through BAR1 until its release, which waits for the driver task, and the CPU's
// view is that mapping, write-combined
TEST(nvidia_engines, mappable_video_memory_stays_mapped_until_the_driver_task_releases_it) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.create_flags = GPU_BUFFER_MAPPABLE;
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    vram_range range = fixture.memory.last_allocated;
    gpu::cpu_view view = {};

    EXPECT_EQ(fixture.gpu_engines.view_for_cpu(fixture.created, view), gpu::OK);
    fixture.gpu_engines.release_memory(fixture.created);
    uint32_t releases_before_task = fixture.memory.created_releases;
    engines_operations().serve();

    EXPECT_EQ(view.phys, BAR1_PHYS + range.offset);
    EXPECT_EQ(view.size, CREATED_BYTES);
    EXPECT_FALSE(view.cacheable);

    EXPECT_EQ(fixture.memory.created_maps, 1u);
    EXPECT_EQ(fixture.memory.created_unmaps, 0u);
    EXPECT_EQ(releases_before_task, 0u);
    EXPECT_EQ(fixture.memory.created_releases, 1u);
    EXPECT_EQ(fixture.memory.last_released.offset, range.offset);
}

// System memory comes zeroed from the DMA layer, cached on a coherent card, with no video memory or fill
TEST(nvidia_engines, system_memory_comes_zeroed_from_the_dma_layer) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    uint32_t allocations = fixture.memory.allocations;
    fixture.create_placement = GPU_PLACEMENT_SYSTEM;
    fixture.create_flags = GPU_BUFFER_MAPPABLE;

    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    const dma::buffer& pages = static_cast<backend_memory*>(fixture.created)->pages;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(pages.virt);
    gpu::cpu_view view = {};

    EXPECT_EQ(fixture.gpu_engines.view_for_cpu(fixture.created, view), gpu::OK);
    uint64_t nonzero_words = 0;
    for (uint64_t i = 0; i < CREATED_BYTES / sizeof(uint32_t); i++) {
        nonzero_words += words[i] != 0 ? 1 : 0;
    }

    EXPECT_EQ(view.phys, pages.phys);
    EXPECT_EQ(view.size, CREATED_BYTES);
    EXPECT_TRUE(view.cacheable);

    EXPECT_EQ(nonzero_words, 0u);
    EXPECT_EQ(fixture.device_id, 0u);
    EXPECT_EQ(fixture.memory.allocations, allocations);
    EXPECT_EQ(fixture.fill_count, 2u);

    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// Whether a mappable create, with a full BAR1 window or a mapping without a physical address, fails before its zeroing
// and gives its video memory back
static bool refuses_without_a_cpu_mapping(bool is_window_full) {
    engines_fixture fixture;
    fixture.memory.is_window_full = is_window_full;
    fixture.memory.maps_without_phys = !is_window_full;
    fixture.create_flags = GPU_BUFFER_MAPPABLE;
    if (bring_up_on_gpu(fixture) != OK) {
        return false;
    }

    int32_t rc = create_on_gpu(fixture);
    bool has_released = fixture.memory.created_releases == 1;
    bool is_same_range = fixture.memory.last_released.offset == fixture.memory.last_allocated.offset;
    return rc == gpu::ERR_NO_MEMORY && has_released && is_same_range && fixture.fill_count == 2;
}

// The CPU's view is a mapping's physical address, so a mappable create refuses without one
TEST(nvidia_engines, a_mappable_create_refuses_without_a_cpu_mapping) {
    EXPECT_TRUE(refuses_without_a_cpu_mapping(true));
    EXPECT_TRUE(refuses_without_a_cpu_mapping(false));
}

// A create the video memory refuses fills and maps nothing
TEST(nvidia_engines, a_create_without_video_memory_fills_nothing) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.memory.refuses_allocation = true;
    fixture.create_flags = GPU_BUFFER_MAPPABLE;

    EXPECT_EQ(create_on_gpu(fixture), gpu::ERR_NO_MEMORY);

    EXPECT_EQ(fixture.memory.created_maps, 0u);
    EXPECT_EQ(fixture.fill_count, 2u);
    EXPECT_TRUE(fixture.created == nullptr);
}

// A zeroing that hangs the copy channel fails and reports the GPU core's device but not the whole driver: its range
// stays, unmapped from BAR1, later calls that can fail refuse without touching the GPU, and a release keeps its memory
TEST(nvidia_engines, a_hung_zeroing_fails_the_device_and_keeps_its_range) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.create_flags = GPU_BUFFER_MAPPABLE;
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    gpu::memory* kept = fixture.created;
    fixture.created = nullptr;

    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 4;
    uint64_t gpu_ns = 0;
    gpu::cpu_view view = {};
    const uint8_t commands[16] = {};

    EXPECT_EQ(create_on_gpu(fixture), gpu::ERR_DEVICE);
    uint32_t allocations = fixture.memory.allocations;
    EXPECT_EQ(create_on_gpu(fixture), gpu::ERR_DEVICE);

    EXPECT_EQ(fixture.memory.created_unmaps, 1u);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_EQ(fixture.memory.allocations, allocations);

    EXPECT_EQ(fixture.gpu_engines.read_timestamp(gpu_ns), gpu::ERR_DEVICE);
    EXPECT_EQ(fixture.gpu_engines.view_for_cpu(kept, view), gpu::ERR_DEVICE);
    EXPECT_EQ(fixture.gpu_engines.check_commands(commands, sizeof(commands)), gpu::ERR_DEVICE);
    EXPECT_FALSE(fixture.failed.is_set());
    EXPECT_TRUE(fixture.created == nullptr);

    fixture.gpu_engines.release_memory(kept);
    engines_operations().serve();
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_TRUE(has_device_failed(fixture));
}

// Once the GPU failed, the CPU's view refuses and a release keeps the memory, which the GPU may still reach
TEST(nvidia_engines, memory_stays_once_the_gpu_failed) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.create_flags = GPU_BUFFER_MAPPABLE;
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    gpu::cpu_view view = {};

    fixture.failed.set();
    EXPECT_EQ(fixture.gpu_engines.view_for_cpu(fixture.created, view), gpu::ERR_DEVICE);
    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();

    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_EQ(create_on_gpu(fixture), gpu::ERR_DEVICE);
}

// A shared memory object of two pages pinned as the GPU core's import pins it, and what a wrap of them gives
struct pinned_pages {
    mm::shmem*        object = mm::shmem_create(2 * pmm::PAGE_SIZE);
    mm::shmem_mapping pin;
    pmm::phys_addr_t  pages[2] = {};
    gpu::memory*      wrapped = nullptr;
    uint32_t          device_id = 1;

    pinned_pages() {
        if (object == nullptr) {
            return;
        }

        RUN_ELEVATED(sync::mutex_lock(object->lock));
        pin = mm::shmem_mapping::start_locked(object);
        pages[0] = mm::shmem_get_page_locked(object, 0);
        pages[1] = mm::shmem_get_page_locked(object, 1);
        RUN_ELEVATED(sync::mutex_unlock(object->lock));
    }

    ~pinned_pages() {
        if (object != nullptr && object->release()) {
            mm::shmem::ref_destroy(object);
        }
    }

    int32_t wrap(engines& gpu_engines) {
        return gpu_engines.wrap_pages(pages, 2, static_cast<mm::shmem_mapping&&>(pin), &wrapped, &device_id);
    }

    // The object's mapping count, one for each pin while no CPU maps the object
    size_t pins() const {
        RUN_ELEVATED(sync::mutex_lock(object->lock));
        size_t count = object->m_mapping_count;
        RUN_ELEVATED(sync::mutex_unlock(object->lock));

        return count;
    }
};

// A reaper node that marks itself reached, then takes `hold`, waiting there while the test runner holds it. It never
// spins, as the reaper shares the boot CPU with the runner, its idle task, which runs only while no other task can
struct reaper_gate {
    rc::reaper::dead_node  reaper_node;
    sync::atomic<uint32_t> is_reached;
    sync::mutex            hold;
};

static rc::reaper::cleanup_result pass_gate(reaper_gate* gate) {
    gate->is_reached.store_release(1);
    RUN_ELEVATED(sync::mutex_lock(gate->hold));
    RUN_ELEVATED(sync::mutex_unlock(gate->hold));

    return rc::reaper::DONE;
}

// Defers `gate` and returns whether the reaper reaches it within the spin timeout, past every node deferred before.
// When `holds`, the caller must unlock `gate.hold` on every path, as the reaper waits there until it does
static bool reaper_reaches(reaper_gate& gate, bool holds) {
    gate.hold.init();
    if (holds) {
        RUN_ELEVATED(sync::mutex_lock(gate.hold));
    }

    gate.reaper_node.init(rc::reaper::reaper_thunk<reaper_gate, pass_gate>);
    RUN_ELEVATED(rc::reaper::defer(&gate.reaper_node));

    return test_helpers::spin_wait(gate.is_reached);
}

static reaper_gate s_reaper_held_for_a_wrap;
static reaper_gate s_reaper_past_a_wrap;

// A wrap gives the device id 0, makes one run of each page and takes the pin, which its release leaves to the reaper
TEST(nvidia_engines, a_wrap_holds_the_pin_until_its_release_on_the_reaper) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    pinned_pages shared;
    ASSERT_NOT_NULL(shared.object);

    ASSERT_EQ(shared.wrap(fixture.gpu_engines), gpu::OK);
    const drivers::nvidia::memory_run* runs = static_cast<backend_memory*>(shared.wrapped)->wrapped_pages;

    EXPECT_TRUE(shared.pin.ptr() == nullptr);
    EXPECT_EQ(shared.device_id, 0u);
    EXPECT_EQ(runs[0].address, shared.pages[0]);
    EXPECT_EQ(runs[1].address, shared.pages[1]);
    EXPECT_EQ(runs[1].bytes, pmm::PAGE_SIZE);

    EXPECT_TRUE(reaper_reaches(s_reaper_held_for_a_wrap, true));
    fixture.gpu_engines.release_memory(shared.wrapped);
    engines_operations().serve();
    EXPECT_EQ(shared.pins(), 1u);
    RUN_ELEVATED(sync::mutex_unlock(s_reaper_held_for_a_wrap.hold));

    ASSERT_TRUE(reaper_reaches(s_reaper_past_a_wrap, false));
    EXPECT_EQ(shared.pins(), 0u);
}

static reaper_gate s_reaper_past_a_failed_wrap;

// Once the GPU failed, a wrap takes the pin and refuses, and a wrapped memory's release keeps its pin
TEST(nvidia_engines, a_failed_gpu_keeps_each_wraps_pin) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    pinned_pages kept;
    pinned_pages refused;
    ASSERT_TRUE(kept.object != nullptr && refused.object != nullptr);
    ASSERT_EQ(kept.wrap(fixture.gpu_engines), gpu::OK);

    fixture.failed.set();
    EXPECT_EQ(refused.wrap(fixture.gpu_engines), gpu::ERR_DEVICE);
    fixture.gpu_engines.release_memory(kept.wrapped);
    engines_operations().serve();

    ASSERT_TRUE(reaper_reaches(s_reaper_past_a_failed_wrap, false));
    EXPECT_TRUE(refused.pin.ptr() == nullptr);
    EXPECT_EQ(kept.pins(), 1u);
    EXPECT_EQ(refused.pins(), 1u);
}

// On a card driving 40 address bits, a wrap is refused with nothing made when its first or last page ends past 1 TiB,
// and goes through when its highest page ends at 1 TiB
TEST(nvidia_engines, a_wrap_past_the_cards_address_limit_is_refused) {
    engines_fixture fixture;
    fixture.card = dma::device_profile_test_access::build(40, true);
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);

    engines& gpu_engines = fixture.gpu_engines;
    const pmm::phys_addr_t last_past[] = {0x7000000, 1ULL << 40};
    const pmm::phys_addr_t first_past[] = {1ULL << 40, 0x7000000};
    const pmm::phys_addr_t within[] = {0x7000000, (1ULL << 40) - pmm::PAGE_SIZE};
    gpu::memory* wrapped = nullptr;
    uint32_t device_id = 1;

    EXPECT_EQ(gpu_engines.wrap_pages(last_past, 2, mm::shmem_mapping(), &wrapped, &device_id), gpu::ERR_NO_MEMORY);
    EXPECT_EQ(gpu_engines.wrap_pages(first_past, 2, mm::shmem_mapping(), &wrapped, &device_id), gpu::ERR_NO_MEMORY);
    EXPECT_NULL(wrapped);
    ASSERT_EQ(gpu_engines.wrap_pages(within, 2, mm::shmem_mapping(), &wrapped, &device_id), gpu::OK);

    gpu_engines.release_memory(wrapped);
    engines_operations().serve();
}

// On a card that is not coherent every wrap is refused, the pin left to the core, as processes map shared memory cached
TEST(nvidia_engines, every_wrap_on_a_card_that_is_not_coherent_is_refused) {
    engines_fixture fixture;
    fixture.card = dma::device_profile_test_access::build(64, false);
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    pinned_pages shared;
    ASSERT_NOT_NULL(shared.object);

    EXPECT_EQ(shared.wrap(fixture.gpu_engines), gpu::ERR_NO_MEMORY);
    EXPECT_TRUE(shared.pin.ptr() == shared.object);
    EXPECT_NULL(shared.wrapped);
}

// NVIDIA's address space class and the device's controls that hand it a page directory and take it back
static constexpr uint32_t FERMI_VASPACE_A      = 0x90F1;
static constexpr uint32_t SET_PAGE_DIRECTORY   = 0x801813;
static constexpr uint32_t UNSET_PAGE_DIRECTORY = 0x801814;

// A space is created by the driver task as the firmware's object over the space's own root directory, and its destroy
// waits for nothing, leaving the take-back and both releases to the task
TEST(nvidia_engines, a_space_is_created_and_destroyed_by_the_driver_task) {
    engines_fixture fixture;
    fixture.memory.next_offset = TABLES_OFFSET;
    engines_task_state state;

    sched::task* task = start_step(fixture, state, create_space_step);
    ASSERT_NOT_NULL(task);
    bool waits_for_the_task = test_helpers::blocks_before_deadline(task) && fixture.fw.allocations == 0;
    ASSERT_EQ(finish_step(fixture, state, task), gpu::OK);

    EXPECT_TRUE(waits_for_the_task);
    EXPECT_EQ(fixture.fw.last_class, FERMI_VASPACE_A);
    EXPECT_EQ(fixture.fw.last_command, SET_PAGE_DIRECTORY);
    EXPECT_EQ(fixture.memory.last_allocated.offset, TABLES_OFFSET);

    fixture.gpu_engines.destroy_space(fixture.space);
    uint32_t releases_before_task = fixture.fw.releases;
    engines_operations().serve();

    EXPECT_EQ(releases_before_task, 0u);
    EXPECT_EQ(fixture.fw.last_command, UNSET_PAGE_DIRECTORY);
    EXPECT_EQ(fixture.fw.releases, 1u);
    EXPECT_EQ(fixture.memory.table_releases, 1u);
}

// A space whose address space object the firmware refuses gives its root directory back and is not created
TEST(nvidia_engines, a_space_the_firmware_refuses_is_not_created) {
    engines_fixture fixture;
    fixture.memory.next_offset = TABLES_OFFSET;
    fixture.fw.allocation_result = ERR_FIRMWARE;

    EXPECT_EQ(create_space_on_gpu(fixture), gpu::ERR_NO_MEMORY);

    EXPECT_TRUE(fixture.space == nullptr);
    EXPECT_EQ(fixture.memory.table_releases, 1u);
    EXPECT_FALSE(fixture.failed.is_set());
}

// A space whose root directory is not written fails the GPU before the firmware sees it, keeping the root
TEST(nvidia_engines, a_space_whose_root_directory_is_not_written_fails_the_gpu) {
    engines_fixture fixture;
    fixture.memory.next_offset = TABLES_OFFSET;
    fixture.memory.update_result = ERR_BAD_STATE;

    EXPECT_EQ(create_space_on_gpu(fixture), gpu::ERR_DEVICE);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_TRUE(fixture.space == nullptr);
    EXPECT_EQ(fixture.fw.allocations, 0u);
    EXPECT_EQ(fixture.memory.table_releases, 0u);
}

// A page table entry's valid bit, read-only bit and kind field, the coherent system memory aperture with the volatile
// bit, and NVIDIA's kind for generic memory
static constexpr uint64_t PAGE_VALID       = 0x1;
static constexpr uint64_t PAGE_READ_ONLY   = 1ULL << 6;
static constexpr uint32_t PAGE_KIND_SHIFT  = 56;
static constexpr uint64_t PAGE_SYSTEM_BITS = 0x4 | 0x8;
static constexpr uint32_t GENERIC_KIND     = 6;

// A GPU address in a context's user range, the fourth page of a 2 MiB page table
static constexpr uint64_t MAPPED_VA = 0x40000000 + 3 * 4096;

// An entry's address field, the page's number from bit 8
static uint64_t address_field(uint64_t address) {
    return address >> 12 << 8;
}

// The entry a map wrote for `va` into the page table a space adds fifth, after its root and three directories
static uint64_t page_entry_at(uint64_t va) {
    return s_tables_image[4 * TABLE_WORDS + (va >> 12) % TABLE_WORDS];
}

// Whether the engines came up with system memory created and a space whose tables start at `TABLES_OFFSET`
static bool sets_up_system_memory_and_a_space(engines_fixture& fixture) {
    fixture.create_placement = GPU_PLACEMENT_SYSTEM;
    if (bring_up_on_gpu(fixture) != OK || create_on_gpu(fixture) != gpu::OK) {
        return false;
    }

    fixture.memory.next_offset = TABLES_OFFSET;
    return create_space_on_gpu(fixture) == gpu::OK;
}

// Video memory maps at its offset with the kind and access asked, its entries written with the space's TLBs
// invalidated, and an unmap clears them the same way
TEST(nvidia_engines, video_memory_maps_at_its_offset_with_the_kind_and_access_asked) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    uint64_t offset = fixture.memory.last_allocated.offset;
    fixture.memory.next_offset = TABLES_OFFSET;
    ASSERT_EQ(create_space_on_gpu(fixture), gpu::OK);

    engines& gpu_engines = fixture.gpu_engines;
    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0x1000, 0x2000, GENERIC_KIND, true), gpu::OK);
    uint64_t mapped_directory = fixture.memory.invalidated_directory;
    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA + 0x2000, fixture.created, 0, 0x1000, 0, false), gpu::OK);

    uint64_t read_only_generic = PAGE_READ_ONLY | static_cast<uint64_t>(GENERIC_KIND) << PAGE_KIND_SHIFT | PAGE_VALID;
    EXPECT_EQ(mapped_directory, TABLES_OFFSET);
    EXPECT_EQ(page_entry_at(MAPPED_VA), address_field(offset + 0x1000) | read_only_generic);
    EXPECT_EQ(page_entry_at(MAPPED_VA + 0x1000), address_field(offset + 0x2000) | read_only_generic);
    EXPECT_EQ(page_entry_at(MAPPED_VA + 0x2000), address_field(offset) | PAGE_VALID);
    EXPECT_EQ(page_entry_at(MAPPED_VA + 0x3000), 0u);

    fixture.memory.invalidated_directory = 0;
    EXPECT_EQ(gpu_engines.unmap(fixture.space, MAPPED_VA, 0x3000), gpu::OK);

    EXPECT_EQ(fixture.memory.invalidated_directory, TABLES_OFFSET);
    for (uint64_t page = 0; page < 3; page++) {
        EXPECT_EQ(page_entry_at(MAPPED_VA + page * 0x1000), 0u);
    }

    gpu_engines.destroy_space(fixture.space);
    gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// System memory maps by its physical address, coherent with the CPU's caches and volatile
TEST(nvidia_engines, system_memory_maps_by_its_physical_address) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    uint64_t phys = static_cast<backend_memory*>(fixture.created)->pages.phys;

    EXPECT_EQ(fixture.gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0x1000, 0x1000, 0, false), gpu::OK);

    EXPECT_EQ(page_entry_at(MAPPED_VA), address_field(phys + 0x1000) | PAGE_SYSTEM_BITS | PAGE_VALID);

    fixture.gpu_engines.destroy_space(fixture.space);
    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// Wrapped memory maps each page at its own address, from the page the offset names
TEST(nvidia_engines, wrapped_memory_maps_each_page_at_its_own_address) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    const pmm::phys_addr_t pages[] = {0x7000000, 0x3000000, 0x5000000};
    gpu::memory* wrapped = nullptr;
    uint32_t device_id = 1;
    ASSERT_EQ(fixture.gpu_engines.wrap_pages(pages, 3, mm::shmem_mapping(), &wrapped, &device_id), gpu::OK);

    EXPECT_EQ(fixture.gpu_engines.map(fixture.space, MAPPED_VA, wrapped, 0x1000, 0x2000, 0, false), gpu::OK);

    EXPECT_EQ(page_entry_at(MAPPED_VA), address_field(pages[1]) | PAGE_SYSTEM_BITS | PAGE_VALID);
    EXPECT_EQ(page_entry_at(MAPPED_VA + 0x1000), address_field(pages[2]) | PAGE_SYSTEM_BITS | PAGE_VALID);
    EXPECT_EQ(page_entry_at(MAPPED_VA + 0x2000), 0u);

    fixture.gpu_engines.destroy_space(fixture.space);
    fixture.gpu_engines.release_memory(wrapped);
    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// A kind past the uncompressed ones, however many bits it has, and a map without room for its tables write nothing
TEST(nvidia_engines, a_refused_map_writes_nothing) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    engines& gpu_engines = fixture.gpu_engines;
    uint32_t updates = fixture.memory.table_updates;
    uint32_t allocations = fixture.memory.allocations;

    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 7, false), gpu::ERR_REFUSED);
    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0x106, false), gpu::ERR_REFUSED);
    fixture.memory.refuses_allocation = true;
    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0, false), gpu::ERR_NO_MEMORY);

    EXPECT_EQ(fixture.memory.table_updates, updates);
    EXPECT_EQ(fixture.memory.allocations, allocations);
    EXPECT_FALSE(fixture.failed.is_set());

    gpu_engines.destroy_space(fixture.space);
    gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// A map whose invalidate misses its deadline fails the GPU, after which an unmap writes nothing and a destroyed space
// keeps its tables and its firmware object, since the GPU may still walk them
TEST(nvidia_engines, a_map_whose_invalidate_misses_its_deadline_fails_the_gpu) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    engines& gpu_engines = fixture.gpu_engines;
    fixture.memory.update_result = ERR_TIMEOUT;

    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0, false), gpu::ERR_DEVICE);
    engines_operations().serve();
    EXPECT_TRUE(has_device_failed(fixture));

    uint32_t updates = fixture.memory.table_updates;
    uint32_t controls = fixture.fw.other_controls;
    EXPECT_EQ(gpu_engines.unmap(fixture.space, MAPPED_VA, 0x1000), gpu::ERR_DEVICE);
    gpu_engines.destroy_space(fixture.space);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_EQ(fixture.memory.table_updates, updates);
    EXPECT_EQ(fixture.fw.other_controls, controls);
    EXPECT_EQ(fixture.fw.releases, 0u);
    EXPECT_EQ(fixture.memory.table_releases, 0u);

    gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// An unmap whose invalidate misses its deadline fails the GPU, since the card may still translate the range
TEST(nvidia_engines, an_unmap_whose_invalidate_misses_its_deadline_fails_the_gpu) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    engines& gpu_engines = fixture.gpu_engines;
    ASSERT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0, false), gpu::OK);
    fixture.memory.update_result = ERR_TIMEOUT;

    EXPECT_EQ(gpu_engines.unmap(fixture.space, MAPPED_VA, 0x1000), gpu::ERR_DEVICE);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));

    gpu_engines.destroy_space(fixture.space);
    gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// An unmap the page tables refuse fails the GPU core's device but not the whole driver, since the range stays
// translated, and a release then keeps the memory it maps
TEST(nvidia_engines, a_refused_unmap_fails_the_device_and_keeps_the_memory) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    uint64_t offset = fixture.memory.last_allocated.offset;

    fixture.memory.next_offset = TABLES_OFFSET;
    ASSERT_EQ(create_space_on_gpu(fixture), gpu::OK);
    engines& gpu_engines = fixture.gpu_engines;
    ASSERT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0, false), gpu::OK);

    EXPECT_EQ(gpu_engines.unmap(fixture.space, MAPPED_VA, 0), gpu::ERR_DEVICE);
    gpu_engines.release_memory(fixture.created);
    engines_operations().serve();

    EXPECT_FALSE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_EQ(page_entry_at(MAPPED_VA), address_field(offset) | PAGE_VALID);
    EXPECT_EQ(fixture.memory.created_releases, 0u);

    gpu_engines.destroy_space(fixture.space);
    engines_operations().serve();
}

// Once a hung zeroing failed the GPU core's device, a space is neither created, mapped into nor unmapped from, and a
// destroyed one keeps its tables and its firmware object
TEST(nvidia_engines, spaces_stay_once_a_hung_zeroing_failed_the_device) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    engines& gpu_engines = fixture.gpu_engines;
    gpu::memory* system_memory = fixture.created;
    gpu::space* space = fixture.space;
    fixture.space = nullptr;

    fixture.create_placement = GPU_PLACEMENT_VIDEO;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 3;
    ASSERT_EQ(create_on_gpu(fixture), gpu::ERR_DEVICE);
    uint32_t updates = fixture.memory.table_updates;
    uint32_t controls = fixture.fw.other_controls;

    EXPECT_EQ(gpu_engines.map(space, MAPPED_VA, system_memory, 0, 0x1000, 0, false), gpu::ERR_DEVICE);
    EXPECT_EQ(gpu_engines.unmap(space, MAPPED_VA, 0x1000), gpu::ERR_DEVICE);
    EXPECT_EQ(create_space_on_gpu(fixture), gpu::ERR_DEVICE);
    gpu_engines.destroy_space(space);
    engines_operations().serve();

    EXPECT_TRUE(fixture.space == nullptr);
    EXPECT_EQ(fixture.memory.table_updates, updates);
    EXPECT_EQ(fixture.fw.other_controls, controls);
    EXPECT_EQ(fixture.fw.releases, 0u);
    EXPECT_EQ(fixture.memory.table_releases, 0u);
    EXPECT_FALSE(fixture.failed.is_set());

    gpu_engines.release_memory(system_memory);
    engines_operations().serve();
}

// The firmware objects a queue's channel allocates: its channel group, channel and copy object
static constexpr uint32_t QUEUE_CHANNEL_OBJECTS = 3;

// NVIDIA's allocation flags for unprivileged channels 1 and 2, with physical copies and privileged MEM_OPs denied,
// scheduling delayed and the id's USERD slot on page 0 fixed, and NVIDIA's internal privilege of a user's channel
static constexpr uint32_t USER_CHANNEL_1_FLAGS = 0x0060'01C0;
static constexpr uint32_t USER_CHANNEL_2_FLAGS = 0x0060'02C0;
static constexpr uint32_t PRIVILEGE_USER       = 0x0;

// A context's part above its user range: its semaphores, then a 256 KiB window per queue holding the queue's 128 KiB
// GPFIFO and then its push buffer, three pages of 128-byte release sequences
static constexpr uint64_t SEMAPHORES_VA      = 1ULL << 39;
static constexpr uint64_t WINDOW_BYTES       = 256 * 1024;
static constexpr uint64_t FIRST_GPFIFO_VA    = SEMAPHORES_VA + WINDOW_BYTES;
static constexpr uint64_t FIRST_PUSH_VA      = FIRST_GPFIFO_VA + 128 * 1024;
static constexpr uint64_t PUSH_BUFFER_PAGES  = 3;
static constexpr uint64_t RELEASE_SLOT_BYTES = 128;

// A release sequence's words, and those holding its semaphore's address and its payload
static constexpr uint32_t RELEASE_WORDS             = 20;
static constexpr uint32_t RELEASE_ADDRESS_LOW_WORD  = 8;
static constexpr uint32_t RELEASE_ADDRESS_HIGH_WORD = 9;
static constexpr uint32_t RELEASE_PAYLOAD_WORD      = 10;

// A GPFIFO entry's word count from bit 10 of its second word, and NVIDIA's SYNC_WAIT bit there
static constexpr uint32_t GPFIFO_LENGTH_SHIFT = 10;
static constexpr uint32_t GPFIFO_SYNC_WAIT    = 1u << 31;

// Copy engine 2's runlist on the RTX 3080, which a doorbell token holds from bit 16
static constexpr uint32_t COPY_RUNLIST_TOKEN = 1u << 16;

static int32_t create_queue_step(engines_fixture& fixture) {
    return fixture.gpu_engines.create_queue(fixture.space, GPU_QUEUE_COPY, nullptr, &fixture.queue);
}

static int32_t create_queue_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, create_queue_step);
}

static backend_queue& backend_of(gpu::queue* queue) {
    return *static_cast<backend_queue*>(queue);
}

static uint32_t gpfifo_word_of(gpu::queue* queue, uint32_t entry, uint32_t word) {
    return channel_test_access::gpfifo_word(backend_of(queue).user_channel, entry, word);
}

static uint32_t put_of_queue(gpu::queue* queue) {
    return channel_test_access::userd_word(backend_of(queue).user_channel, USERD_GP_PUT);
}

static uint32_t release_word_of(gpu::queue* queue, uint64_t slot, uint32_t word) {
    uintptr_t release = backend_of(queue).push_buffer.virt + slot * RELEASE_SLOT_BYTES;
    return reinterpret_cast<const uint32_t*>(release)[word];
}

static uint64_t semaphores_of(engines_fixture& fixture) {
    return static_cast<backend_space*>(fixture.space)->semaphores.phys;
}

// Tears a queue down: its channel, then its push buffer and the queue itself
static void tear_down_queue(gpu::queue* queue) {
    backend_queue& backend = backend_of(queue);
    backend.user_channel.destroy();
    RUN_ELEVATED(dma::free_pages(backend.push_buffer));
    heap::ufree_delete(&backend);
}

// Destroys the space and releases the memory `sets_up_system_memory_and_a_space` created
static void tear_down_space(engines_fixture& fixture) {
    fixture.gpu_engines.destroy_space(fixture.space);
    fixture.gpu_engines.release_memory(fixture.created);
    engines_operations().serve();
}

// A queue is an unprivileged channel on the copy engine's runlist with the lowest id above the kernel's copy channel,
// created by the driver task, with its rings mapped read-only and the semaphores writable above the user range
TEST(nvidia_engines, a_queue_is_an_unprivileged_channel_with_its_rings_above_the_user_range) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    uint32_t allocations = fixture.fw.allocations;
    engines_task_state state;

    sched::task* task = start_step(fixture, state, create_queue_step);
    ASSERT_NOT_NULL(task);
    bool waits_for_the_task = test_helpers::blocks_before_deadline(task) && fixture.fw.allocations == allocations;
    ASSERT_EQ(finish_step(fixture, state, task), gpu::OK);

    EXPECT_TRUE(waits_for_the_task);
    EXPECT_EQ(fixture.fw.allocations, allocations + QUEUE_CHANNEL_OBJECTS);

    const rm::channel_params& params = fixture.fw.last_channel;
    EXPECT_EQ(params.flags, USER_CHANNEL_1_FLAGS);
    EXPECT_EQ(params.internal_flags, PRIVILEGE_USER);
    EXPECT_EQ(params.engine_type, CE2);
    EXPECT_EQ(params.gpfifo_address, FIRST_GPFIFO_VA);

    backend_queue& queue = backend_of(fixture.queue);
    uint64_t gpfifo = channel_test_access::gpfifo_phys(queue.user_channel);
    uint64_t read_only_system = PAGE_READ_ONLY | PAGE_SYSTEM_BITS | PAGE_VALID;
    EXPECT_EQ(page_entry_at(SEMAPHORES_VA), address_field(semaphores_of(fixture)) | PAGE_SYSTEM_BITS | PAGE_VALID);
    EXPECT_EQ(page_entry_at(FIRST_GPFIFO_VA), address_field(gpfifo) | read_only_system);
    for (uint64_t page = 0; page < PUSH_BUFFER_PAGES; page++) {
        uint64_t phys = queue.push_buffer.phys + page * 4096;
        EXPECT_EQ(page_entry_at(FIRST_PUSH_VA + page * 4096), address_field(phys) | read_only_system);
    }

    EXPECT_EQ(page_entry_at(FIRST_PUSH_VA + PUSH_BUFFER_PAGES * 4096), 0u);

    tear_down_queue(fixture.queue);
    tear_down_space(fixture);
}

// A run takes a GPFIFO entry per range, one flagged GPU written fetched only once the host processed the entries before
// it, then an entry for its release in its sequence's push buffer slot, and rings the queue's doorbell
TEST(nvidia_engines, run_writes_an_entry_per_range_then_its_release_and_rings_the_doorbell) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu_nvidia_range ranges[] = {
        {.va = 0x10000, .size = 0x40, .flags = 0},
        {.va = 0x7F'FFFF'F000, .size = 0x1000, .flags = GPU_COMMANDS_GPU_WRITTEN},
    };

    fixture.gpu_engines.run(fixture.queue, 1, ranges, sizeof(ranges));

    EXPECT_EQ(gpfifo_word_of(fixture.queue, 0, 0), 0x10000u);
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 0, 1), 0x10u << GPFIFO_LENGTH_SHIFT);
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 1, 0), 0xFFFF'F000u);
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 1, 1), 0x7Fu | (0x400u << GPFIFO_LENGTH_SHIFT) | GPFIFO_SYNC_WAIT);
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 2, 0), static_cast<uint32_t>(FIRST_PUSH_VA + RELEASE_SLOT_BYTES));
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 2, 1), 0x80u | (RELEASE_WORDS << GPFIFO_LENGTH_SHIFT));

    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_ADDRESS_LOW_WORD), 0u);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_ADDRESS_HIGH_WORD), 0x80u);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_PAYLOAD_WORD), 1u);
    EXPECT_EQ(put_of_queue(fixture.queue), 3u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), COPY_RUNLIST_TOKEN | 1);

    tear_down_queue(fixture.queue);
    tear_down_space(fixture);
}

// A submission without ranges runs its release alone, in push buffer slot `sequence % 65` with the sequence's low 32
// bits as its payload
TEST(nvidia_engines, an_empty_submission_runs_its_release_alone_in_its_sequences_slot) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);

    fixture.gpu_engines.run(fixture.queue, (1ULL << 32) + 65, nullptr, 0);

    uint64_t slot = 61; // (2^32 + 65) % 65
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 0, 0), static_cast<uint32_t>(FIRST_PUSH_VA + slot * RELEASE_SLOT_BYTES));
    EXPECT_EQ(release_word_of(fixture.queue, slot, RELEASE_PAYLOAD_WORD), 65u);
    EXPECT_EQ(put_of_queue(fixture.queue), 1u);

    tear_down_queue(fixture.queue);
    tear_down_space(fixture);
}

// A second queue takes the next id, window and semaphore, in the semaphores' page the first queue mapped
TEST(nvidia_engines, a_second_queue_takes_the_next_id_window_and_semaphore) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;
    uint64_t semaphores = semaphores_of(fixture);

    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    fixture.gpu_engines.run(fixture.queue, 1, nullptr, 0);

    EXPECT_EQ(fixture.fw.last_channel.flags, USER_CHANNEL_2_FLAGS);
    EXPECT_EQ(fixture.fw.last_channel.gpfifo_address, FIRST_GPFIFO_VA + WINDOW_BYTES);
    EXPECT_EQ(semaphores_of(fixture), semaphores);

    uint64_t release_va = FIRST_PUSH_VA + WINDOW_BYTES + RELEASE_SLOT_BYTES;
    EXPECT_EQ(gpfifo_word_of(fixture.queue, 0, 0), static_cast<uint32_t>(release_va));
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_ADDRESS_LOW_WORD), 4u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), COPY_RUNLIST_TOKEN | 2);

    tear_down_queue(fixture.queue);
    tear_down_queue(first);
    tear_down_space(fixture);
}

// A command list is whole ranges, each whole words one GPFIFO entry can count, at a word-aligned address inside the
// context's user range, flagged at most GPU_COMMANDS_GPU_WRITTEN
TEST(nvidia_engines, check_commands_refuses_ranges_of_bad_size_address_or_flags) {
    engines_fixture fixture;
    engines& gpu_engines = fixture.gpu_engines;
    uint32_t words_max = (1u << 21) - 1;

    gpu_nvidia_range refused[] = {
        {.va = 0x10000, .size = 0, .flags = 0},
        {.va = 0x10000, .size = 6, .flags = 0},
        {.va = 0x10000, .size = (words_max + 1) * 4, .flags = 0},
        {.va = 0x10002, .size = 4, .flags = 0},
        {.va = 0xFFC, .size = 4, .flags = 0},
        {.va = 0x7F'FFFF'FFFC, .size = 8, .flags = 0},
        {.va = 0xFFFF'FFFF'FFFF'FFF0, .size = 0x20, .flags = 0},
        {.va = 0x10000, .size = 4, .flags = 0x2},
        {.va = 0x10000, .size = 4, .flags = 0x8000'0001},
    };

    gpu_nvidia_range accepted[] = {
        {.va = 0x1000, .size = words_max * 4, .flags = 0},
        {.va = 0x7F'FFFF'FFFC, .size = 4, .flags = GPU_COMMANDS_GPU_WRITTEN},
    };

    for (const gpu_nvidia_range& range : refused) {
        gpu_nvidia_range list[] = {accepted[0], range};
        EXPECT_EQ(gpu_engines.check_commands(&range, sizeof(range)), gpu::ERR_REFUSED);
        EXPECT_EQ(gpu_engines.check_commands(list, sizeof(list)), gpu::ERR_REFUSED);
    }

    EXPECT_EQ(gpu_engines.check_commands(accepted, sizeof(accepted)), gpu::OK);
    EXPECT_EQ(gpu_engines.check_commands(accepted, 0), gpu::OK);
    EXPECT_EQ(gpu_engines.check_commands(accepted, sizeof(accepted) - 8), gpu::ERR_REFUSED);
}

// A failed GPU runs no list, so a list a working one accepts reports the loss instead
TEST(nvidia_engines, check_commands_refuses_once_the_gpu_failed) {
    engines_fixture fixture;
    gpu_nvidia_range range = {.va = 0x10000, .size = 4, .flags = 0};
    ASSERT_EQ(fixture.gpu_engines.check_commands(&range, sizeof(range)), gpu::OK);

    fixture.failed.set();

    EXPECT_EQ(fixture.gpu_engines.check_commands(&range, sizeof(range)), gpu::ERR_DEVICE);
}

// A queue whose channel the firmware refuses unmaps its push buffer and gives back its window and id, which the next
// queue takes, while the space keeps its semaphores
TEST(nvidia_engines, a_queue_the_firmware_refuses_gives_back_its_window_and_id) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    fixture.fw.allocation_result = ERR_FIRMWARE;

    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);

    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(page_entry_at(FIRST_GPFIFO_VA), 0u);
    EXPECT_EQ(page_entry_at(FIRST_PUSH_VA), 0u);
    EXPECT_EQ(page_entry_at(SEMAPHORES_VA), address_field(semaphores_of(fixture)) | PAGE_SYSTEM_BITS | PAGE_VALID);
    EXPECT_FALSE(fixture.failed.is_set());

    fixture.fw.allocation_result = OK;
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);

    EXPECT_EQ(fixture.fw.last_channel.flags, USER_CHANNEL_1_FLAGS);
    EXPECT_EQ(fixture.fw.last_channel.gpfifo_address, FIRST_GPFIFO_VA);

    tear_down_queue(fixture.queue);
    tear_down_space(fixture);
}

// A queue whose channel keeps its memory, after the firmware refused to release the channel's group, keeps its push
// buffer mapped and its window and id held, since the firmware may still reach them
TEST(nvidia_engines, a_queue_whose_channel_keeps_its_memory_keeps_its_window_and_id) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    fixture.fw.refused_class = fixture.chip.channel_class;
    fixture.fw.release_result = ERR_FIRMWARE;

    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);

    uint64_t read_only_system = PAGE_READ_ONLY | PAGE_SYSTEM_BITS | PAGE_VALID;
    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(page_entry_at(FIRST_GPFIFO_VA) & read_only_system, read_only_system);
    EXPECT_EQ(page_entry_at(FIRST_PUSH_VA) & read_only_system, read_only_system);

    fixture.fw.refused_class = ~0u;
    fixture.fw.release_result = OK;
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);

    EXPECT_EQ(fixture.fw.last_channel.flags, USER_CHANNEL_2_FLAGS);
    EXPECT_EQ(fixture.fw.last_channel.gpfifo_address, FIRST_GPFIFO_VA + WINDOW_BYTES);

    tear_down_queue(fixture.queue);
    tear_down_space(fixture);
}

// A space whose eight windows are held, or a runlist whose channel ids are, refuses a queue before creating its channel
TEST(nvidia_engines, a_queue_without_a_free_window_or_id_allocates_no_channel) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;
    fixture.queue = nullptr;

    auto* space = static_cast<backend_space*>(fixture.space);
    uint32_t allocations = fixture.fw.allocations;
    uint32_t video_allocations = fixture.memory.allocations;

    space->queue_slots = 0xFF;
    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);
    space->queue_slots = 0x1;
    engines_test_access::hold_every_channel_id(fixture.gpu_engines);
    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);

    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(fixture.fw.allocations, allocations);
    EXPECT_EQ(fixture.memory.allocations, video_allocations);

    tear_down_queue(first);
    tear_down_space(fixture);
}

// Once the GPU failed, a queue is refused before the driver task sees it, and a run writes no entry and rings no
// doorbell
TEST(nvidia_engines, queues_are_refused_and_runs_ignored_once_the_gpu_failed) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;
    fixture.queue = nullptr;

    uint32_t allocations = fixture.fw.allocations;
    uint32_t doorbell = bar0_word(VIRTUAL_FUNCTION_DOORBELL);
    fixture.failed.set();

    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_DEVICE);
    fixture.gpu_engines.run(first, 1, nullptr, 0);

    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(fixture.fw.allocations, allocations);
    EXPECT_EQ(put_of_queue(first), 0u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), doorbell);

    tear_down_queue(first);
    tear_down_space(fixture);
}
