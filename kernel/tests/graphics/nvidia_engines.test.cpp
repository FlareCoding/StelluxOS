// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/string.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/engines.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "dynpriv/dynpriv.h"
#include "tests/framework/helpers.h"
#include "tests/graphics/nvidia_channel_test_access.h"
#include "tests/graphics/nvidia_copy_channel_test_access.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_engines);

using drivers::nvidia::channel_test_access;
using drivers::nvidia::chip_info;
using drivers::nvidia::copy_channel;
using drivers::nvidia::copy_channel_test_access;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::engine_info;
using drivers::nvidia::engines;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::failed_mark;
using drivers::nvidia::firmware;
using drivers::nvidia::GPU_SUBMIT_TIMEOUT_NS;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::OK;
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
    uint32_t           table_reads = 0;
    uint32_t           other_controls = 0;
    uint32_t           allocations = 0;
    uint32_t           releases = 0;
    uint32_t           internal_controls = 0;
    uint32_t           internal_command = 0;
    rm::handle         next_handle = FIRST_HANDLE;

    int32_t allocate(rm::handle, rm::handle, uint32_t, void*, size_t) override {
        allocations++;
        return OK;
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        bool is_table_read = object == TEST_OBJECTS.subdevice && command == GET_DEVICE_INFO_TABLE;
        if (is_table_read) {
            table_reads++;
            write_table(static_cast<uint8_t*>(params));
            return OK;
        }

        other_controls++;
        if (command == GET_FAULT_METHOD_BUFFER_SIZE && size == sizeof(uint32_t)) {
            uint32_t bytes = 20480;
            string::memcpy(params, &bytes, sizeof(bytes));
        }

        return OK;
    }

    int32_t release(rm::handle) override {
        releases++;
        return OK;
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

// Hands out video memory from 4 MiB on, maps the check's range for the CPU onto `s_check_memory` and an instance block
// onto `s_engines_instance_block`, and counts the check range's unmaps and releases
class engines_video_memory : public video_memory {
public:
    uint64_t next_offset = 0x400000;
    uint64_t check_offset = 0;
    uint32_t allocations = 0;
    uint32_t check_unmaps = 0;
    uint32_t check_releases = 0;

    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        allocations++;
        out = {.offset = next_offset, .size = size};
        if (size == sizeof(s_check_memory)) {
            check_offset = next_offset;
        }

        next_offset += size;
        return OK;
    }

    void release(const vram_range& range) override {
        if (is_check(range)) {
            check_releases++;
        }
    }

    int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) override {
        if (is_check(range)) {
            out = {.virt = reinterpret_cast<uintptr_t>(s_check_memory), .phys = 0};
            return OK;
        }

        if (range.size > sizeof(s_engines_instance_block)) {
            return ERR_NO_MEMORY;
        }

        out = {.virt = reinterpret_cast<uintptr_t>(s_engines_instance_block), .phys = 0};
        return OK;
    }

    void unmap_for_cpu(const vram_range& range) override {
        if (is_check(range)) {
            check_unmaps++;
        }
    }

    int32_t update_context_tables(uint64_t, table_writes&, drivers::nvidia::context_tlbs) override { return OK; }

private:
    bool is_check(const vram_range& range) const {
        return check_offset != 0 && range.offset == check_offset && range.size == sizeof(s_check_memory);
    }
};

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

static constexpr uint32_t MAX_FILLS = 4;

// Bring-up over doubles with the test runner as the GPU, running each entry the copy channel publishes and raising the
// copy vector. Fill `faulty_fill`, counted from 1, has `fault` at word `fault_word` of the check's memory
struct engines_fixture {
    engines_firmware     fw;
    engines_video_memory memory;
    failed_mark          failed;
    registers            bar0;
    engines              gpu_engines;

    copy_fault fault = copy_fault::none;
    uint32_t   faulty_fill = 0;
    uint64_t   fault_word = 0;

    ran_fill fills[MAX_FILLS] = {};
    uint32_t fill_count = 0;
    uint32_t strays = 0; // entries outside the push buffer and fills outside the check's memory, which the double skips
    uint32_t acknowledged = 0;
    uint64_t took_ns = 0;

    engines_fixture() : bar0(fresh_bar0()), gpu_engines(fw, TEST_OBJECTS, memory, failed) {
        fw.engines = RTX_3080_FIRST_ENGINES;
        fw.engine_count = 4;

        for (uint64_t i = 0; i < CHECK_WORDS; i++) {
            s_check_memory[i] = STALE_WORD;
        }
    }

    ~engines_fixture() {
        copy_channel* copier = engines_test_access::copy_channel_of(gpu_engines);
        if (copier != nullptr) {
            copier->destroy();
        }
    }

    int32_t bring_up() { return gpu_engines.bring_up(bar0, engines_test_card(), engines_test_chip()); }

    // Runs each entry the copy channel publishes once bring-up's task waits for it, until the task ends
    bool act_as_gpu(sched::task& task, const sync::atomic<uint32_t>& is_done) {
        uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
        uint32_t get = 0;
        while (!is_done.load_acquire()) {
            if (clock::now_ns() > deadline) {
                return false;
            }

            // The channel is read only while the task waits, since a failed check destroys it
            bool is_waiting = task.state.load_acquire() == sched::TASK_STATE_BLOCKED;
            copy_channel* copier = is_waiting ? engines_test_access::published_copy_channel(gpu_engines) : nullptr;
            if (copier != nullptr && put_of(*copier) != get) {
                run_entry(*copier, get);
                get++;
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

    // Writes the fill into the check's memory as the copy engine would, with this fill's fault
    void launch_fill(const entry_methods& methods, uint32_t fill_number) {
        uint64_t first = (methods.fill_offset - memory.check_offset) / sizeof(uint32_t);
        bool is_in_check = methods.fill_offset >= memory.check_offset && first + methods.fill_words <= CHECK_WORDS;
        if (!is_in_check || fill_count == MAX_FILLS) {
            strays++;
            return;
        }

        fills[fill_count] = {.offset = methods.fill_offset, .words = methods.fill_words, .value = methods.fill_value};
        fill_count++;

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

// Bring-up run by a task, since its fills sleep and the test runner, the boot CPU's idle task, must never block
struct bring_up_task_state {
    engines_fixture*       fixture;
    int32_t                result;
    sync::atomic<uint32_t> is_done;
};

// What `bring_up_on_gpu` returns for a bring-up whose task did not finish, a value no error of the driver has
static constexpr int32_t NOT_FINISHED = 1;

static void run_bring_up_task(void* arg) {
    bring_up_task_state* state = static_cast<bring_up_task_state*>(arg);
    state->result = state->fixture->bring_up();
    state->is_done.store_release(1);
    sched::exit(0);
}

// Runs bring-up in a task while the test runner acts as the GPU, and returns its result
static int32_t bring_up_on_gpu(engines_fixture& fixture) {
    bring_up_task_state state;
    state.fixture = &fixture;
    state.result = NOT_FINISHED;
    state.is_done.store_relaxed(0);

    uint64_t start_ns = clock::now_ns();
    sched::task* task = test_helpers::start_pinned_task(run_bring_up_task, &state, "engines_bring_up");
    if (task == nullptr) {
        return NOT_FINISHED;
    }

    bool is_done = fixture.act_as_gpu(*task, state.is_done);
    fixture.took_ns = clock::now_ns() - start_ns;
    test_helpers::unpin(task);

    return is_done ? state.result : NOT_FINISHED;
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
