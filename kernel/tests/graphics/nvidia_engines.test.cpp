// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/logging.h"
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
using drivers::nvidia::channel;
using drivers::nvidia::channel_context_buffers;
using drivers::nvidia::channel_error;
using drivers::nvidia::CHANNEL_IDS;
using drivers::nvidia::channel_test_access;
using drivers::nvidia::chip_info;
using drivers::nvidia::context_buffer;
using drivers::nvidia::context_buffer_layout;
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
using drivers::nvidia::global_context_buffers;
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

// NVIDIA's controls, the device info table's layout and engine types, copy engine 2's and GR0's kernel interrupt table
// rows, and BAR0's CPU interrupt tree and doorbell, restated so the tests hold the engines to NVIDIA's values
static constexpr uint32_t GET_DEVICE_INFO_TABLE        = 0x20801112;
static constexpr uint32_t GET_KERNEL_TABLE             = 0x20800A5C;
static constexpr uint32_t GET_FAULT_METHOD_BUFFER_SIZE = 0x20802A08;
static constexpr size_t   DEVICE_INFO_ENTRY_BYTES      = 100;
static constexpr size_t   DEVICE_INFO_FIRST_ENTRY      = 12;
static constexpr uint32_t ENGINE_TYPE_WORD             = 2;
static constexpr uint32_t RUNLIST_WORD                 = 3;
static constexpr uint32_t CHANNEL_RAM_WORD             = 14;
static constexpr uint32_t GR0                          = 0x1;
static constexpr uint32_t CE0                          = 0x9;
static constexpr uint32_t CE1                          = 0xA;
static constexpr uint32_t CE2                          = 0xB;
static constexpr uint16_t MC_ENGINE_IDX_CE2            = 17;
static constexpr uint16_t MC_ENGINE_IDX_GR0            = 82;
static constexpr uint32_t CPU_INTR_LEAF                = 0xB81000;
static constexpr uint32_t CPU_INTR_LEAF_EN_SET         = 0xB81200;
static constexpr uint32_t CPU_INTR_LEAF_EN_CLEAR       = 0xB81400;
static constexpr uint32_t VIRTUAL_FUNCTION_DOORBELL    = 0xBB0090;
static constexpr uint32_t USERD_GP_PUT                 = 0x8C;

// NVIDIA's NV_CHRAM_CHANNEL(i), 4 bytes per channel id, with its PBDMA_FAULTED and ENG_FAULTED bits, and the copy and
// graphics engines' channel RAM in the tests, placed in the double's BAR0 clear of the registers the engines use
static constexpr uint32_t CHANNEL_RAM_ENTRY_BYTES   = 4;
static constexpr uint32_t CHANNEL_RAM_PBDMA_FAULTED = 1u << 4;
static constexpr uint32_t CHANNEL_RAM_ENG_FAULTED   = 1u << 5;
static constexpr uint32_t TEST_CHANNEL_RAM          = 0xB90000;
static constexpr uint32_t TEST_GRAPHICS_CHANNEL_RAM = 0xB94000;

// NVIDIA's controls reading the graphics engine's facts and context buffer sizes, the place of the subcontext count
// and of each buffer the driver allocates, and the control promoting a channel's context buffers
static constexpr uint32_t KGR_GET_INFO                 = 0x20800A2A;
static constexpr size_t   KGR_GET_INFO_BYTES           = 3392;
static constexpr uint32_t MAX_SUBCONTEXT_COUNT         = 0x2C;
static constexpr uint32_t KGR_GET_CONTEXT_BUFFERS_INFO = 0x20800A32;
static constexpr size_t   CONTEXT_BUFFERS_BYTES        = 1600;
static constexpr uint32_t GRAPHICS_BUFFER_POSITIONS[]  = {0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x17, 0x18};
static constexpr uint32_t GPU_PROMOTE_CTX              = 0x2080012B;

// Ampere's 3D, compute and copy classes
static constexpr uint32_t AMPERE_B          = 0xC797;
static constexpr uint32_t AMPERE_COMPUTE_B  = 0xC7C0;
static constexpr uint32_t AMPERE_DMA_COPY_B = 0xC7B5;

// NVIDIA's controls binding a channel to its engine's runlist and scheduling a bound channel, the last a channel's
// creation sends
static constexpr uint32_t GPFIFO_BIND     = 0xA06F0104;
static constexpr uint32_t GPFIFO_SCHEDULE = 0xA06F0103;

// NVIDIA's control giving the firmware's MMU fault record of a channel, the bytes of its reply, and the offsets there
// of the faulting address's high and low words and of the fault type's name, its bytes and the padding after them
static constexpr uint32_t GET_MMU_FAULT_INFO      = 0xA06F0107;
static constexpr size_t   MMU_FAULT_INFO_BYTES    = 104;
static constexpr size_t   MMU_FAULT_ADDRESS_HIGH  = 0;
static constexpr size_t   MMU_FAULT_ADDRESS_LOW   = 4;
static constexpr size_t   MMU_FAULT_TYPE_NAME     = 12;
static constexpr size_t   MMU_FAULT_NAME_BYTES    = 32;
static constexpr size_t   MMU_FAULT_PADDING_BYTES = 4;

// NVIDIA's control handing the firmware a split address space's directories above the range the firmware maps alone
static constexpr uint32_t COPY_SERVER_RESERVED_PDES = 0x90F10106;

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

// Copy engine 2's nonstall vector on the RTX 3080, bit 7 of leaf 0, the one the double gives the graphics engine, and
// another vector of that leaf
static constexpr uint32_t COPY_VECTOR         = 7;
static constexpr uint32_t COPY_VECTOR_BIT     = 1u << 7;
static constexpr uint32_t GRAPHICS_VECTOR     = 2;
static constexpr uint32_t GRAPHICS_VECTOR_BIT = 1u << 2;
static constexpr uint32_t OTHER_VECTOR_BIT    = 1u << 0;

// The firmware objects the copy channel's teardown releases: its copy object, channel, channel group and address space
static constexpr uint32_t COPY_CHANNEL_OBJECTS = 4;

// The layout the double gives each graphics context buffer at its position in `GRAPHICS_BUFFER_POSITIONS`, each size
// its own and more than an instance block, the RTV buffer aligned past 4 KiB
static constexpr context_buffer_layout MAIN_LAYOUT            = {.bytes = 0x2000, .alignment = 0x1000};
static constexpr context_buffer_layout PATCH_LAYOUT           = {.bytes = 0x4000, .alignment = 0x1000};
static constexpr context_buffer_layout BUNDLE_LAYOUT          = {.bytes = 0x5000, .alignment = 0x1000};
static constexpr context_buffer_layout PAGE_POOL_LAYOUT       = {.bytes = 0x6000, .alignment = 0x1000};
static constexpr context_buffer_layout ATTRIBUTE_LAYOUT       = {.bytes = 0x3000, .alignment = 0x1000};
static constexpr context_buffer_layout RTV_LAYOUT             = {.bytes = 0x7000, .alignment = 0x10000};
static constexpr context_buffer_layout FECS_EVENT_LAYOUT      = {.bytes = 0x8000, .alignment = 0x1000};
static constexpr context_buffer_layout PRIV_ACCESS_MAP_LAYOUT = {.bytes = 0x9000, .alignment = 0x1000};

static constexpr context_buffer_layout GRAPHICS_BUFFER_LAYOUTS[] = {
    MAIN_LAYOUT,
    PATCH_LAYOUT,
    BUNDLE_LAYOUT,
    PAGE_POOL_LAYOUT,
    ATTRIBUTE_LAYOUT,
    RTV_LAYOUT,
    FECS_EVENT_LAYOUT,
    PRIV_ACCESS_MAP_LAYOUT,
};

static constexpr size_t GRAPHICS_BUFFER_KINDS = sizeof(GRAPHICS_BUFFER_POSITIONS) / sizeof(uint32_t);

static_assert(
    sizeof(GRAPHICS_BUFFER_LAYOUTS) / sizeof(context_buffer_layout) == GRAPHICS_BUFFER_KINDS,
    "a layout per position"
);

// The double's one subcontext, so the main buffer grows by one header. Seven global buffers, and the golden
// channel's main and patch buffers
static constexpr uint32_t GRAPHICS_SUBCONTEXTS    = 1;
static constexpr uint64_t OWN_MAIN_BYTES          = MAIN_LAYOUT.bytes + 0x1000;
static constexpr uint32_t GLOBAL_BUFFER_COUNT     = 7;
static constexpr uint32_t GRAPHICS_BUFFER_COUNT   = GLOBAL_BUFFER_COUNT + 2;
static constexpr uint32_t GOLDEN_CHANNEL_OBJECTS  = 4; // its 3D object, channel, channel group and address space

// The bytes graphics bring-up zeroes: the global buffers, the register access map's layout twice, and the golden
// channel's own two
static constexpr uint64_t ZEROED_GRAPHICS_BYTES =
    BUNDLE_LAYOUT.bytes + PAGE_POOL_LAYOUT.bytes + ATTRIBUTE_LAYOUT.bytes + RTV_LAYOUT.bytes + FECS_EVENT_LAYOUT.bytes +
    2 * PRIV_ACCESS_MAP_LAYOUT.bytes + OWN_MAIN_BYTES + PATCH_LAYOUT.bytes;

// Copy engines only on the graphics engine's runlist, which run only inside a graphics context
static constexpr engine_info SHARED_COPY_ENGINES[] = {
    {.type = GR0, .runlist = 0, .channel_ram = 0},
    {.type = CE0, .runlist = 0, .channel_ram = 0},
    {.type = CE1, .runlist = 0, .channel_ram = 0},
};

// The RTX 3080's first engines, with copy engine 2 on a runlist of its own
static constexpr engine_info RTX_3080_FIRST_ENGINES[] = {
    {.type = GR0, .runlist = 0, .channel_ram = TEST_GRAPHICS_CHANNEL_RAM},
    {.type = CE0, .runlist = 0, .channel_ram = 0},
    {.type = CE1, .runlist = 0, .channel_ram = 0},
    {.type = CE2, .runlist = 1, .channel_ram = TEST_CHANNEL_RAM},
};

namespace drivers::nvidia {

// Reaches the engines' copy channel, as the card's interrupt handler does once bring-up publishes the copy vector
struct engines_test_access {
    static copy_channel* published_copy_channel(engines& gpu_engines) {
        return gpu_engines.m_is_copy_vector_on.load_acquire() ? gpu_engines.m_copy_channel : nullptr;
    }

    static copy_channel* copy_channel_of(engines& gpu_engines) { return gpu_engines.m_copy_channel; }

    static const global_context_buffers& global_buffers_of(engines& gpu_engines) {
        return gpu_engines.m_global_buffers;
    }

    // Gives `gpu_engines` the BAR0, chip and card that bring-up and registration would, without either
    static void describe(engines& gpu_engines, registers bar0, const chip_info& chip, const card_identity& identity) {
        gpu_engines.m_bar0 = bar0;
        gpu_engines.m_chip = &chip;
        gpu_engines.m_identity = identity;
    }

    // Gives `gpu_engines` the GPU core's device that registration would
    static void register_with(engines& gpu_engines, gpu::device& dev) { gpu_engines.m_device = &dev; }

    // Takes the device back, as engines whose registration failed hold none
    static void unregister(engines& gpu_engines) { gpu_engines.m_device = nullptr; }

    static bool is_listed(engines& gpu_engines, backend_queue& queue) {
        for (backend_queue& listed : gpu_engines.m_queues) {
            if (&listed == &queue) {
                return true;
            }
        }

        return false;
    }

    static void hold_every_channel_id(engines& gpu_engines) {
        for (uint64_t& word : gpu_engines.m_channel_ids) {
            word = ~0ULL;
        }
    }

    static bool holds_channel_id(engines& gpu_engines, uint32_t id) {
        return (gpu_engines.m_channel_ids[id / 64] >> (id % 64) & 1) != 0;
    }

    // Whether a failure the engines found themselves, with its cause logged, failed the device
    static bool is_device_failed(engines& gpu_engines) { return gpu_engines.m_is_device_failed.load_acquire(); }

    // Takes `queue` off the list the handler reads unless the failure's report did, before a test frees it
    static void unlist_queue(engines& gpu_engines, backend_queue& queue) {
        if (!is_listed(gpu_engines, queue)) {
            return;
        }

        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(gpu_engines.m_queues_lock);
            gpu_engines.m_queues.remove(&queue);
            sync::spin_unlock_irqrestore(gpu_engines.m_queues_lock, irq);
        });
    }

    // Whether the fault attribution's record is queued once. A record queued twice in a row links to itself, a link
    // this cuts so that a later serve ends
    static bool is_fault_attribution_queued_once(engines& gpu_engines) {
        operation& record = gpu_engines.m_fault_attribution;
        bool is_linked_to_itself = record.next == &record;
        if (is_linked_to_itself) {
            record.next = nullptr;
        }

        return record.is_queued && !is_linked_to_itself;
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::engines_test_access;

// The allocations whose classes the firmware's double keeps, from its first
static constexpr uint32_t KEPT_CLASSES = 64;

// Acts as the GSP's firmware: answers the engine table, buffer sizes, graphics facts and MMU fault records, and each
// control, promotion and interrupt table with `OK` unless the fields below say otherwise
class engines_firmware : public firmware {
public:
    const engine_info* engines = nullptr;
    uint32_t           engine_count = 0;
    int32_t            kernel_table_result = OK;
    int32_t            graphics_info_result = OK; // what the graphics facts' read answers
    int32_t            promote_result = OK;
    uint32_t           promotions = 0;
    rm::handle         promoted_on = 0;
    rm::handle         promoted_channel = 0;
    uint32_t           promoted_entries = 0;
    uint32_t           promoted_after = 0;  // the allocations made before the last promotion
    uint32_t           bound_after = 0;     // the allocations made before the last bind
    uint32_t           scheduled_after = 0; // the allocations made before the last schedule
    int32_t            allocation_result = OK; // what every allocation answers
    uint32_t           refused_class = ~0u;    // a class whose allocations the firmware refuses
    int32_t            release_result = OK;    // what every release answers
    uint32_t           refused_release = 0;    // the release, counted from 1, the firmware refuses
    uint32_t           missed_release = 0;     // the release, counted from 1, whose reply misses its deadline
    uint32_t           refused_command = 0;    // a control the firmware refuses
    uint32_t           missed_command = 0;     // a control whose reply misses its deadline
    failed_mark*       gpu_failed = nullptr;   // marked by those missed replies, as a missed reply marks the GPU failed
    uint32_t           table_reads = 0;
    uint32_t           other_controls = 0;
    uint32_t           allocations = 0;
    uint32_t           releases = 0;
    uint32_t           reserved_handovers = 0; // one per split address space
    uint32_t           internal_controls = 0;
    uint32_t           internal_command = 0;
    uint32_t           last_class = 0;
    uint32_t           last_command = 0; // of the controls other than the table's read
    rm::channel_params last_channel = {};
    rm::handle         last_channel_handle = 0;
    uint32_t           last_copy_engine = 0; // the engine type of the last copy object
    rm::handle         next_handle = FIRST_HANDLE;

    rm::handle  faulted_channel = 0;           // the channel whose MMU fault record names a fault
    uint64_t    faulted_address = 0;           // the faulting address that record names
    const char* faulted_name = "FAULT_PDE";    // that record's type name, as the RTX 3080's firmware gives one
    bool        reports_during_record = false; // reports an MMU fault once, while answering that record
    uint32_t    fault_records = 0;             // the MMU fault records asked for

    uint32_t classes[KEPT_CLASSES] = {}; // each allocation's class, in order

    // The sink `set_error_sink` named last
    drivers::nvidia::firmware_error_sink* error_sink = nullptr;

    int32_t allocate(rm::handle, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        if (allocations < KEPT_CLASSES) {
            classes[allocations] = class_id;
        }

        allocations++;
        last_class = class_id;
        if (size == sizeof(rm::channel_params)) {
            string::memcpy(&last_channel, params, size);
            last_channel_handle = object;
        }

        if (class_id == AMPERE_DMA_COPY_B && size == sizeof(rm::copy_object_params)) {
            last_copy_engine = static_cast<rm::copy_object_params*>(params)->engine_type;
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
        if (command == COPY_SERVER_RESERVED_PDES) {
            reserved_handovers++;
        }

        if (command == GPFIFO_BIND) {
            bound_after = allocations;
        }

        if (command == GPFIFO_SCHEDULE) {
            scheduled_after = allocations;
        }

        if (command == GET_MMU_FAULT_INFO) {
            fault_records++;
        }

        if (command == missed_command) {
            gpu_failed->set();
            return ERR_TIMEOUT;
        }

        if (command == refused_command) {
            return ERR_FIRMWARE;
        }

        if (command == GET_MMU_FAULT_INFO && size == MMU_FAULT_INFO_BYTES) {
            write_mmu_fault_record(object, static_cast<uint8_t*>(params));
        }

        if (command == GET_MMU_FAULT_INFO && object == faulted_channel && reports_during_record) {
            reports_during_record = false;
            error_sink->on_channel_error(channel_error::fault);
        }

        if (command == GET_FAULT_METHOD_BUFFER_SIZE && size == sizeof(uint32_t)) {
            uint32_t bytes = 20480;
            string::memcpy(params, &bytes, sizeof(bytes));
        }

        if (command == GPU_PROMOTE_CTX) {
            promotions++;
            promoted_on = object;
            promoted_after = allocations;

            auto* promotion = static_cast<rm::promote_context_params*>(params);
            promoted_channel = promotion->channel;
            promoted_entries = promotion->entry_count;
            return promote_result;
        }

        return OK;
    }

    int32_t control_with_status(rm::handle object, uint32_t command, void* params, size_t size, uint32_t&) override {
        return control(object, command, params, size);
    }

    int32_t release(rm::handle) override {
        releases++;
        if (releases == missed_release) {
            gpu_failed->set();
            return ERR_TIMEOUT;
        }

        if (releases == refused_release) {
            return ERR_FIRMWARE;
        }

        return release_result;
    }

    rm::handle new_handle() override { return next_handle++; }

    rm::handle client() const override { return 0; }

    int32_t internal_control(uint32_t command, void* params, size_t size) override {
        internal_controls++;
        internal_command = command;

        auto* words = static_cast<uint32_t*>(params);
        if (command == KGR_GET_INFO && size == KGR_GET_INFO_BYTES) {
            words[MAX_SUBCONTEXT_COUNT * 2] = MAX_SUBCONTEXT_COUNT;
            words[MAX_SUBCONTEXT_COUNT * 2 + 1] = GRAPHICS_SUBCONTEXTS;
            return graphics_info_result;
        }

        if (command == KGR_GET_CONTEXT_BUFFERS_INFO && size == CONTEXT_BUFFERS_BYTES) {
            for (size_t i = 0; i < GRAPHICS_BUFFER_KINDS; i++) {
                uint32_t position = GRAPHICS_BUFFER_POSITIONS[i];
                words[position * 2] = static_cast<uint32_t>(GRAPHICS_BUFFER_LAYOUTS[i].bytes);
                words[position * 2 + 1] = static_cast<uint32_t>(GRAPHICS_BUFFER_LAYOUTS[i].alignment);
            }

            return OK;
        }

        if (kernel_table_result != OK || size != sizeof(rm::kernel_interrupt_table_params)) {
            return kernel_table_result;
        }

        auto* table = static_cast<rm::kernel_interrupt_table_params*>(params);
        table->rows[0] = {MC_ENGINE_IDX_CE2, 0, 0, COPY_VECTOR};
        table->rows[1] = {MC_ENGINE_IDX_GR0, 0, 0, GRAPHICS_VECTOR};
        table->row_count = 2;
        return OK;
    }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

    void set_error_sink(drivers::nvidia::firmware_error_sink* sink) override { error_sink = sink; }

private:
    void write_table(uint8_t* bytes) const {
        for (uint32_t i = 0; i < engine_count; i++) {
            uint8_t* entry = bytes + DEVICE_INFO_FIRST_ENTRY + i * DEVICE_INFO_ENTRY_BYTES;
            string::memcpy(entry + ENGINE_TYPE_WORD * 4, &engines[i].type, 4);
            string::memcpy(entry + RUNLIST_WORD * 4, &engines[i].runlist, 4);
            string::memcpy(entry + CHANNEL_RAM_WORD * 4, &engines[i].channel_ram, 4);
        }

        string::memcpy(bytes + 4, &engine_count, sizeof(engine_count));
        bytes[8] = 0;
    }

    // Writes a channel's MMU fault record at NVIDIA's offsets: `faulted_name` at `faulted_address` for
    // `faulted_channel` and NONE for any other channel, a name of 32 bytes or more unterminated, padding set after it
    void write_mmu_fault_record(rm::handle object, uint8_t* bytes) const {
        bool is_faulted = object == faulted_channel;
        uint64_t address = is_faulted ? faulted_address : 0;
        uint32_t high = static_cast<uint32_t>(address >> 32);
        uint32_t low = static_cast<uint32_t>(address);

        const char* name = is_faulted ? faulted_name : "NONE";
        size_t name_bytes = string::strlen(name) + 1;
        name_bytes = name_bytes < MMU_FAULT_NAME_BYTES ? name_bytes : MMU_FAULT_NAME_BYTES;

        string::memcpy(bytes + MMU_FAULT_ADDRESS_HIGH, &high, sizeof(high));
        string::memcpy(bytes + MMU_FAULT_ADDRESS_LOW, &low, sizeof(low));
        string::memcpy(bytes + MMU_FAULT_TYPE_NAME, name, name_bytes);
        string::memset(bytes + MMU_FAULT_TYPE_NAME + MMU_FAULT_NAME_BYTES, 0xFF, MMU_FAULT_PADDING_BYTES);
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

// The text logged while `ENGINES_LOG_BACKEND` is set
static char     s_engines_log[2048] = {};
static uint32_t s_engines_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_engines_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_engines_log_bytes < sizeof(s_engines_log); i++) {
        s_engines_log[s_engines_log_bytes++] = data[i];
    }
}

static constexpr log::backend ENGINES_LOG_BACKEND = {.write = keep_engines_log};

// Keeps the text logged from here until the backend is reset
static void start_keeping_log() {
    s_engines_log_bytes = 0;
    log::set_backend(&ENGINES_LOG_BACKEND);
}

// Whether the kept text holds `text` anywhere
static bool engines_log_contains(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_engines_log_bytes; start++) {
        if (string::memcmp(s_engines_log + start, text, text_bytes) == 0) {
            return true;
        }
    }

    return false;
}

// How many times the kept text holds `text`
static uint32_t engines_log_count(const char* text) {
    size_t text_bytes = string::strlen(text);
    uint32_t count = 0;
    for (uint32_t start = 0; start + text_bytes <= s_engines_log_bytes; start++) {
        if (string::memcmp(s_engines_log + start, text, text_bytes) == 0) {
            count++;
        }
    }

    return count;
}

// The allocations whose offsets and alignments the video memory's double keeps, from its first
static constexpr uint32_t KEPT_ALLOCATIONS = 64;

// Hands out video memory from 4 MiB on, maps the check's range onto `s_check_memory`, an instance block onto
// `s_engines_instance_block` and a created range at `BAR1_PHYS` past its offset, counting maps, unmaps and releases
class engines_video_memory : public video_memory {
public:
    memory_report reported = {};
    bool          refuses_allocation = false;
    uint32_t      refused_allocation = 0;    // the first allocation, counted from 1, it refuses with every later one
    bool          is_window_full = false;    // refuses to map a created range
    bool          maps_without_phys = false; // maps a created range with no physical address
    int32_t       update_result = OK;        // what table updates answer once they wrote `s_tables_image`
    uint32_t      refused_update = 0;        // the first table update, counted from 1, it refuses with every later one

    uint64_t   next_offset = 0x400000;
    uint64_t   created_offset = 0; // where ranges larger than an instance block go once a test sets it
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

    uint64_t asked_offsets[KEPT_ALLOCATIONS] = {};    // each allocation's offset, in order
    uint64_t asked_alignments[KEPT_ALLOCATIONS] = {}; // the alignment each was asked for

    void report(memory_report& out) override {
        out = reported;
    }

    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) override {
        bool is_refused = refused_allocation != 0 && allocations + 1 >= refused_allocation;
        if (refuses_allocation || is_refused) {
            return ERR_NO_MEMORY;
        }

        allocations++;
        bool is_apart = created_offset != 0 && size > sizeof(s_engines_instance_block);
        uint64_t& next = is_apart ? created_offset : next_offset;
        out = {.offset = next, .size = size};
        if (size == sizeof(s_check_memory)) {
            check_offset = next;
        }

        if (allocations <= KEPT_ALLOCATIONS) {
            asked_offsets[allocations - 1] = next;
            asked_alignments[allocations - 1] = alignment;
        }

        next += size;
        last_allocated = out;
        last_use = use;
        return OK;
    }

    // The alignment the kept allocation at `offset` was asked for, or 0 when none lies there
    uint64_t alignment_asked_at(uint64_t offset) const {
        uint32_t kept = allocations < KEPT_ALLOCATIONS ? allocations : KEPT_ALLOCATIONS;
        for (uint32_t i = 0; i < kept; i++) {
            if (asked_offsets[i] == offset) {
                return asked_alignments[i];
            }
        }

        return 0;
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

        bool is_refused = refused_update != 0 && table_updates >= refused_update;
        return is_refused ? ERR_BAD_STATE : update_result;
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

// The BAR0 bytes bring-up is told are mapped, up to the end of the double's last register
static constexpr uint64_t TEST_BAR0_BYTES = VIRTUAL_FUNCTION_DOORBELL + sizeof(uint32_t);

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
    chip.copy_class = AMPERE_DMA_COPY_B;
    chip.graphics.three_d_class = AMPERE_B;
    chip.graphics.compute_class = AMPERE_COMPUTE_B;
    return chip;
}

static uint32_t put_of(const copy_channel& copier) {
    return channel_test_access::userd_word(copy_channel_test_access::queue(copier), USERD_GP_PUT);
}

// How the copy engine's double misbehaves in one fill: it leaves a word unwritten, writes one past the fill, hangs as a
// fill past its deadline does, never finishes as the GPU fails, or finishes just before the GPU fails
enum class copy_fault : uint8_t { none, skips_word, writes_past, hangs, fails_gpu, fails_gpu_after };

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

static constexpr uint32_t MAX_FILLS = 16;

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

    // The space `create_space_on_gpu` creates for `core_context`, and the queue `create_queue_on_gpu` creates in it for
    // `core_queue`
    gpu::context*    core_context = nullptr;
    gpu::space*      space = nullptr;
    gpu::queue*      queue = nullptr;
    gpu::core_queue* core_queue = nullptr;

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

    int32_t bring_up() { return gpu_engines.bring_up(bar0, TEST_BAR0_BYTES, card, chip); }

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
            copier.mark_hung();
            return;
        }

        if (fault == copy_fault::fails_gpu && fill_number == faulty_fill) {
            failed.set();
            copier.mark_hung();
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

        if (fault == copy_fault::fails_gpu_after && fill_number == faulty_fill) {
            failed.set();
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
    return fixture.gpu_engines.create_space(fixture.core_context, &fixture.space);
}

// Creates a space in a task while the test runner acts as the GPU and the driver task
static int32_t create_space_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, create_space_step);
}

// Without an asynchronous copy engine, bring-up asks for no vector, creates nothing and takes no channel error
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
    EXPECT_TRUE(fixture.fw.error_sink == nullptr);
}

// Channel RAM off a word boundary, or whose last word lies past BAR0's mapping, is refused by a line naming its cause
// before anything is asked for or created, and channel RAM ending at the mapping's end passes to the vector's read
TEST(nvidia_engines, bring_up_refuses_channel_ram_that_is_unaligned_or_past_bar0) {
    uint32_t at_bar0_end = static_cast<uint32_t>(TEST_BAR0_BYTES) - CHANNEL_IDS * CHANNEL_RAM_ENTRY_BYTES;
    uint32_t past_bar0_end = at_bar0_end + CHANNEL_RAM_ENTRY_BYTES;
    const char* unaligned_line = "type 0xB's channel RAM at BAR0 offset 0xB90002 is not 4-byte aligned";
    const char* past_end_line =
        "type 0xB's channel RAM of 0x4000 bytes at BAR0 offset 0xBAC098 ends past BAR0's 0xBB0094 bytes";
    struct {
        uint32_t    channel_ram;
        int32_t     result;       // with the vector's read refused, as only a channel RAM passing the check reaches it
        uint32_t    vector_reads;
        const char* line;         // the refusal's line, if any
    } placements[] = {
        {.channel_ram = TEST_CHANNEL_RAM + 2, .result = ERR_BAD_STATE, .vector_reads = 0, .line = unaligned_line},
        {.channel_ram = past_bar0_end, .result = ERR_BAD_STATE, .vector_reads = 0, .line = past_end_line},
        {.channel_ram = at_bar0_end, .result = ERR_FIRMWARE, .vector_reads = 1, .line = nullptr},
    };

    for (const auto& placement : placements) {
        engines_fixture fixture;
        engine_info engines[] = {RTX_3080_FIRST_ENGINES[0], RTX_3080_FIRST_ENGINES[3]};
        engines[1].channel_ram = placement.channel_ram;
        fixture.fw.engines = engines;
        fixture.fw.engine_count = 2;
        fixture.fw.kernel_table_result = ERR_FIRMWARE;

        start_keeping_log();
        EXPECT_EQ(fixture.bring_up(), placement.result);
        log::set_backend(nullptr);

        EXPECT_EQ(fixture.fw.internal_controls, placement.vector_reads);
        EXPECT_EQ(fixture.fw.allocations, 0u);
        EXPECT_TRUE(is_bar0_untouched());
        EXPECT_EQ(engines_log_contains("'s channel RAM"), placement.line != nullptr);
        EXPECT_TRUE(placement.line == nullptr || engines_log_contains(placement.line));
    }
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
// then releases the memory and leaves the channel running with its vector enabled and the engines taking channel errors
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
    EXPECT_TRUE(fixture.fw.error_sink == &fixture.gpu_engines);
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

// Whether `buffer`'s video memory has `layout`'s size and was asked for at `layout`'s alignment
static bool is_allocated_as(
    const engines_video_memory& memory,
    const context_buffer& buffer,
    const context_buffer_layout& layout
) {
    return buffer.range.size == layout.bytes && memory.alignment_asked_at(buffer.range.offset) == layout.alignment;
}

// Brings the fixture's graphics engine up in a task while the test runner acts as the GPU, after bring-up
static int32_t bring_up_graphics_on_gpu(engines_fixture& fixture) {
    if (bring_up_on_gpu(fixture) != OK) {
        return NOT_FINISHED;
    }

    return run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); });
}

// The line graphics bring-up logs once it enables the double's graphics vector
static constexpr const char* GRAPHICS_VECTOR_LINE =
    "graphics channels' releases raise GPU interrupt vector 2, bit 2 of the CPU interrupt tree's leaf 0";

// Graphics bring-up zeroes the global buffers and the golden channel's two through the copy channel, promotes them in
// a split space, captures the golden context, frees the golden channel and its buffers and enables GR0's vector
TEST(nvidia_engines, bring_up_graphics_captures_the_golden_context_and_keeps_the_global_buffers) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);

    start_keeping_log();
    int32_t rc = run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); });
    log::set_backend(nullptr);
    ASSERT_EQ(rc, OK);
    EXPECT_EQ(fixture.strays, 0u);

    ASSERT_EQ(fixture.fill_count, 2 + GRAPHICS_BUFFER_COUNT);
    uint64_t zeroed_bytes = 0;
    for (uint32_t i = 2; i < fixture.fill_count; i++) {
        EXPECT_EQ(fixture.fills[i].value, 0u);
        zeroed_bytes += fixture.fills[i].words * sizeof(uint32_t);
    }

    EXPECT_EQ(zeroed_bytes, ZEROED_GRAPHICS_BYTES);

    const global_context_buffers& global = engines_test_access::global_buffers_of(fixture.gpu_engines);
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.bundle, BUNDLE_LAYOUT));
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.page_pool, PAGE_POOL_LAYOUT));
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.attribute, ATTRIBUTE_LAYOUT));
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.rtv, RTV_LAYOUT));

    EXPECT_TRUE(is_allocated_as(fixture.memory, global.fecs_event, FECS_EVENT_LAYOUT));
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.priv_access_map, PRIV_ACCESS_MAP_LAYOUT));
    EXPECT_TRUE(is_allocated_as(fixture.memory, global.unrestricted_priv_access_map, PRIV_ACCESS_MAP_LAYOUT));

    EXPECT_EQ(fixture.fw.reserved_handovers, 1u);
    EXPECT_EQ(fixture.fw.promotions, 1u);
    EXPECT_EQ(fixture.fw.promoted_on, TEST_OBJECTS.subdevice);
    EXPECT_EQ(fixture.fw.promoted_channel, fixture.fw.last_channel_handle);
    EXPECT_EQ(fixture.fw.last_class, AMPERE_B);

    EXPECT_EQ(fixture.fw.releases, GOLDEN_CHANNEL_OBJECTS);
    EXPECT_EQ(fixture.memory.created_releases, 2u);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_SET), GRAPHICS_VECTOR_BIT);
    EXPECT_TRUE(engines_log_contains(GRAPHICS_VECTOR_LINE));
}

// Whether graphics bring-up over `engines`, its interrupt table read answering `kernel_table_result`, refuses with that
// error or else ERR_BAD_STATE before allocating or promoting anything, logging `line` and no vector
static bool refuses_graphics_before_allocating(
    const engine_info* engines,
    uint32_t count,
    int32_t kernel_table_result,
    const char* line
) {
    engines_fixture fixture;
    fixture.fw.engines = engines;
    fixture.fw.engine_count = count;
    if (bring_up_on_gpu(fixture) != OK) {
        return false;
    }

    fixture.fw.kernel_table_result = kernel_table_result;
    uint32_t allocations = fixture.memory.allocations;
    start_keeping_log();
    int32_t rc = run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); });
    log::set_backend(nullptr);

    bool allocated_nothing = fixture.memory.allocations == allocations && fixture.fw.promotions == 0;
    bool logged_only_line = engines_log_contains(line) && !engines_log_contains(GRAPHICS_VECTOR_LINE);
    int32_t expected = kernel_table_result == OK ? ERR_BAD_STATE : kernel_table_result;
    return rc == expected && allocated_nothing && logged_only_line;
}

// Without a graphics engine in the table, or without a copy engine on its runlist for a graphics queue's copy object,
// graphics bring-up allocates nothing
TEST(nvidia_engines, bring_up_graphics_refuses_a_table_without_a_graphics_engine_or_a_copy_engine_on_its_runlist) {
    static constexpr engine_info COPY_ENGINE_ONLY[] = {{.type = CE2, .runlist = 1, .channel_ram = TEST_CHANNEL_RAM}};
    static constexpr engine_info GRAPHICS_APART[] = {
        {.type = GR0, .runlist = 0, .channel_ram = TEST_GRAPHICS_CHANNEL_RAM},
        {.type = CE2, .runlist = 1, .channel_ram = TEST_CHANNEL_RAM},
    };

    const char* no_graphics_line = "the device info table lists 1 engine and no graphics engine";
    const char* no_copy_line = "the device info table lists no copy engine on the graphics engine's runlist 0";

    EXPECT_TRUE(refuses_graphics_before_allocating(COPY_ENGINE_ONLY, 1, OK, no_graphics_line));
    EXPECT_TRUE(refuses_graphics_before_allocating(GRAPHICS_APART, 2, OK, no_copy_line));
}

// Graphics bring-up reads the graphics engine's vector before it allocates anything
TEST(nvidia_engines, bring_up_graphics_reads_the_vector_before_allocating) {
    const char* line = "reading the graphics engine's nonstall vector failed: -12";
    EXPECT_TRUE(refuses_graphics_before_allocating(RTX_3080_FIRST_ENGINES, 4, ERR_FIRMWARE, line));
}

// The graphics engine's channel RAM off a word boundary, or whose last word lies past BAR0's mapping, is refused by a
// line naming its cause before the vector's read, and channel RAM ending at the mapping's end passes to that read
TEST(nvidia_engines, bring_up_graphics_refuses_channel_ram_that_is_unaligned_or_past_bar0) {
    uint32_t unaligned = TEST_GRAPHICS_CHANNEL_RAM + 2;
    uint32_t at_bar0_end = static_cast<uint32_t>(TEST_BAR0_BYTES) - CHANNEL_IDS * CHANNEL_RAM_ENTRY_BYTES;
    uint32_t past_bar0_end = at_bar0_end + CHANNEL_RAM_ENTRY_BYTES;
    const char* unaligned_line = "graphics engine type 0x1's channel RAM at BAR0 offset 0xB94002 is not 4-byte aligned";
    const char* past_end_line =
        "graphics engine type 0x1's channel RAM of 0x4000 bytes at BAR0 offset 0xBAC098 ends past BAR0's 0xBB0094";
    struct {
        uint32_t    channel_ram;
        int32_t     result;       // with the vector's read refused, as only a channel RAM passing the check reaches it
        uint32_t    vector_reads;
        const char* line;         // the refusal's line, if any
    } placements[] = {
        {.channel_ram = unaligned, .result = ERR_BAD_STATE, .vector_reads = 0, .line = unaligned_line},
        {.channel_ram = past_bar0_end, .result = ERR_BAD_STATE, .vector_reads = 0, .line = past_end_line},
        {.channel_ram = at_bar0_end, .result = ERR_FIRMWARE, .vector_reads = 1, .line = nullptr},
    };

    for (const auto& placement : placements) {
        engines_fixture fixture;
        engine_info engines[] = {RTX_3080_FIRST_ENGINES[0], RTX_3080_FIRST_ENGINES[1], RTX_3080_FIRST_ENGINES[3]};
        engines[0].channel_ram = placement.channel_ram;
        fixture.fw.engines = engines;
        fixture.fw.engine_count = 3;
        ASSERT_EQ(bring_up_on_gpu(fixture), OK);

        fixture.fw.kernel_table_result = ERR_FIRMWARE;
        uint32_t internal_controls = fixture.fw.internal_controls;
        uint32_t allocations = fixture.memory.allocations;
        start_keeping_log();
        int32_t rc = run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); });
        log::set_backend(nullptr);

        EXPECT_EQ(rc, placement.result);
        EXPECT_EQ(fixture.fw.internal_controls, internal_controls + placement.vector_reads);
        EXPECT_EQ(fixture.memory.allocations, allocations);
        EXPECT_EQ(engines_log_contains("'s channel RAM"), placement.line != nullptr);
        EXPECT_TRUE(placement.line == nullptr || engines_log_contains(placement.line));
    }
}

// A refused read of the graphics engine's facts fails graphics bring-up with its error before any allocation
TEST(nvidia_engines, refused_graphics_facts_read_fails_bring_up_before_allocating) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.fw.graphics_info_result = ERR_FIRMWARE;
    uint32_t allocations = fixture.memory.allocations;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_FIRMWARE);
    EXPECT_EQ(fixture.memory.allocations, allocations);
    EXPECT_EQ(fixture.fw.reserved_handovers, 0u);
    EXPECT_EQ(fixture.fw.promotions, 0u);
}

// A refused promotion frees the golden channel, leaves the graphics engine's vector masked and gives back every context
// buffer, since the firmware holds none
TEST(nvidia_engines, refused_promotion_gives_back_every_context_buffer) {
    engines_fixture fixture;
    fixture.fw.promote_result = ERR_FIRMWARE;

    EXPECT_EQ(bring_up_graphics_on_gpu(fixture), ERR_FIRMWARE);
    EXPECT_EQ(fixture.fw.releases, GOLDEN_CHANNEL_OBJECTS - 1);
    EXPECT_EQ(fixture.memory.created_releases, GRAPHICS_BUFFER_COUNT);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_SET), COPY_VECTOR_BIT);
}

// A refused release of the golden channel keeps every context buffer, since the firmware may still reach them
TEST(nvidia_engines, refused_golden_channel_release_keeps_the_context_buffers) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.fw.release_result = ERR_FIRMWARE;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_FIRMWARE);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_TRUE(engines_test_access::holds_channel_id(fixture.gpu_engines, 1));
    fixture.fw.release_result = OK;
}

// A golden channel release whose reply misses its deadline marks the GPU failed and keeps every context buffer, and
// the golden channel's space stays unreleased
TEST(nvidia_engines, missed_golden_channel_release_keeps_the_context_buffers) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    uint32_t releases = fixture.fw.releases;
    fixture.fw.gpu_failed = &fixture.failed;
    fixture.fw.missed_release = releases + 1;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_TIMEOUT);
    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_EQ(fixture.fw.releases, releases + GOLDEN_CHANNEL_OBJECTS - 1);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_TRUE(engines_test_access::holds_channel_id(fixture.gpu_engines, 1));
}

// A refused release of the golden channel's space keeps every context buffer, since the tables it keeps still map them
TEST(nvidia_engines, refused_golden_channel_space_release_keeps_the_context_buffers) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    uint32_t releases = fixture.fw.releases;
    fixture.fw.refused_release = releases + GOLDEN_CHANNEL_OBJECTS;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_FIRMWARE);
    EXPECT_EQ(fixture.fw.releases, releases + GOLDEN_CHANNEL_OBJECTS);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
}

// A release of the golden channel's space whose reply misses its deadline marks the GPU failed and keeps every
// context buffer
TEST(nvidia_engines, missed_golden_channel_space_release_keeps_the_context_buffers) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.fw.gpu_failed = &fixture.failed;
    fixture.fw.missed_release = fixture.fw.releases + GOLDEN_CHANNEL_OBJECTS;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_TIMEOUT);
    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_EQ(fixture.memory.created_releases, 0u);
}

// A golden buffer whose zeroing hangs stays allocated for good, since the hung copy may still write it, and the global
// buffers are given back
TEST(nvidia_engines, hung_golden_zeroing_keeps_its_buffer_and_gives_back_the_global_ones) {
    engines_fixture fixture;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 2 + GLOBAL_BUFFER_COUNT + 1;

    EXPECT_EQ(bring_up_graphics_on_gpu(fixture), ERR_TIMEOUT);
    EXPECT_EQ(fixture.memory.created_releases, GLOBAL_BUFFER_COUNT);
    EXPECT_EQ(fixture.fw.reserved_handovers, 0u);
}

// A global buffer the video memory refuses gives back the global buffers allocated before it
TEST(nvidia_engines, refused_global_buffer_gives_back_the_ones_before_it) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.memory.refused_allocation = fixture.memory.allocations + 4; // the RTV buffer's
    const char* line = "allocating 0x7000 bytes of video memory for a graphics context buffer failed: -6";

    start_keeping_log();
    int32_t rc = run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); });
    log::set_backend(nullptr);

    EXPECT_EQ(rc, ERR_NO_MEMORY);
    EXPECT_EQ(fixture.memory.created_releases, 3u);
    EXPECT_TRUE(engines_log_contains(line));
}

// A golden channel's space whose directories the firmware refuses gives back every context buffer and the channel id,
// with no channel allocated
TEST(nvidia_engines, refused_directory_handover_gives_back_every_context_buffer_and_the_id) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    uint32_t allocations = fixture.fw.allocations;
    fixture.fw.refused_command = COPY_SERVER_RESERVED_PDES;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_FIRMWARE);
    EXPECT_EQ(fixture.fw.allocations, allocations + 1);
    EXPECT_EQ(fixture.memory.created_releases, GRAPHICS_BUFFER_COUNT);
    EXPECT_FALSE(engines_test_access::holds_channel_id(fixture.gpu_engines, 1));
}

// A refused 3D object frees the golden channel and its space and gives back every context buffer
TEST(nvidia_engines, refused_golden_3d_object_gives_back_every_context_buffer) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    uint32_t releases = fixture.fw.releases;
    fixture.fw.refused_class = AMPERE_B;

    EXPECT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), ERR_FIRMWARE);
    EXPECT_EQ(fixture.fw.releases, releases + GOLDEN_CHANNEL_OBJECTS - 1);
    EXPECT_EQ(fixture.memory.created_releases, GRAPHICS_BUFFER_COUNT);
}

// GA102's chip as the driver's chip table names it, set by each facts test
static chip_info s_ga102_chip;

// Gives the fixture's engines GA102's chip and an RTX 3080's identity, without bring-up or registration
static void describe_rtx_3080(engines_fixture& fixture) {
    s_ga102_chip.id = 0x172;
    s_ga102_chip.channel_class = 0xC56F;
    s_ga102_chip.copy_class = 0xC7B5;
    s_ga102_chip.graphics = {
        .three_d_class = 0xC797,
        .compute_class = 0xC7C0,
        .two_d_class = 0x902D,
        .inline_to_memory_class = 0xA140,
    };

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

// The facts describe the card from its identity, chip and memory, with graphics and copy queues, NVK's user range, and
// the command bytes a queue's GPFIFO holds less one entry per running submission and the entry kept empty
TEST(nvidia_engines, facts_describe_the_card) {
    engines_fixture fixture;
    fixture.memory.reported = REPORTED_MEMORY;
    describe_rtx_3080(fixture);
    gpu::device_facts facts = {};

    fixture.gpu_engines.read_facts(facts);

    EXPECT_EQ(facts.info.backend, static_cast<uint32_t>(GPU_BACKEND_NVIDIA));
    EXPECT_EQ(facts.info.queue_kinds, (1u << GPU_QUEUE_GRAPHICS) | (1u << GPU_QUEUE_COPY));
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

    EXPECT_EQ(facts.info.nvidia.class_2d, static_cast<uint16_t>(0x902D));
    EXPECT_EQ(facts.info.nvidia.class_3d, static_cast<uint16_t>(0xC797));
    EXPECT_EQ(facts.info.nvidia.class_m2mf, static_cast<uint16_t>(0xA140));
    EXPECT_EQ(facts.info.nvidia.class_compute, static_cast<uint16_t>(0xC7C0));

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

// Whether the copy channel publishes its GPFIFO entries up to `count` within the spin timeout
static bool publishes_entries(const copy_channel& copier, uint32_t count) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (put_of(copier) != count) {
        if (clock::now_ns() > deadline) {
            return false;
        }
    }

    return true;
}

// A create whose zeroing still runs when another call finds the GPU failed returns at once, keeping its range
// unmapped from BAR1 since the copy may still write it, and logs the kept range but no hang
TEST(nvidia_engines, a_create_whose_zeroing_runs_when_the_gpu_fails_returns_at_once) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    const copy_channel& copier = *engines_test_access::copy_channel_of(fixture.gpu_engines);
    uint32_t zeroing_entries = fixture.next_entry + 1;
    fixture.create_flags = GPU_BUFFER_MAPPABLE;
    gpu::queue* refused = nullptr;

    engines_task_state state;
    uint64_t start_ns = clock::now_ns();
    sched::task* task = start_step(fixture, state, [](engines_fixture& f) { return f.create(); });
    ASSERT_NOT_NULL(task);
    bool has_submitted = publishes_entries(copier, zeroing_entries);
    bool waits_for_its_zeroing = has_submitted && test_helpers::blocks_before_deadline(task);

    fixture.failed.set();
    start_keeping_log();
    int32_t queue_rc = fixture.gpu_engines.create_queue(nullptr, GPU_QUEUE_COPY, nullptr, &refused);
    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    uint64_t took_ns = clock::now_ns() - start_ns;
    log::set_backend(nullptr);

    test_helpers::unpin(task);
    engines_operations().serve();

    EXPECT_TRUE(waits_for_its_zeroing);
    EXPECT_EQ(queue_rc, gpu::ERR_DEVICE);
    EXPECT_EQ(state.result, gpu::ERR_DEVICE);
    EXPECT_LT(took_ns, GPU_SUBMIT_TIMEOUT_NS);

    EXPECT_EQ(fixture.memory.created_unmaps, 1u);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_TRUE(fixture.created == nullptr);
    EXPECT_TRUE(has_device_failed(fixture));

    EXPECT_TRUE(engines_log_contains("stays allocated for good, since its zeroing may still write it"));
    EXPECT_FALSE(engines_log_contains("hung zeroing video memory"));
}

// A mark set outside the engines' calls, as a reply the firmware missed for the display sets it, ends a create waiting
// for its zeroing once the driver task has the engines report it, and a report while the mark is clear does nothing
TEST(nvidia_engines, a_failure_marked_outside_the_engines_ends_a_waiting_create_once_reported) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    const copy_channel& copier = *engines_test_access::copy_channel_of(fixture.gpu_engines);
    uint32_t zeroing_entries = fixture.next_entry + 1;

    engines_task_state state;
    uint64_t start_ns = clock::now_ns();
    sched::task* task = start_step(fixture, state, [](engines_fixture& f) { return f.create(); });
    ASSERT_NOT_NULL(task);
    bool has_submitted = publishes_entries(copier, zeroing_entries);
    bool waits_for_its_zeroing = has_submitted && test_helpers::blocks_before_deadline(task);

    fixture.gpu_engines.report_marked_failure();
    engines_operations().serve();
    bool has_failed_unmarked = has_device_failed(fixture);

    fixture.failed.set();
    fixture.gpu_engines.report_marked_failure();
    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    uint64_t took_ns = clock::now_ns() - start_ns;
    test_helpers::unpin(task);
    engines_operations().serve();

    EXPECT_TRUE(waits_for_its_zeroing);
    EXPECT_FALSE(has_failed_unmarked);
    EXPECT_EQ(state.result, gpu::ERR_DEVICE);
    EXPECT_LT(took_ns, GPU_SUBMIT_TIMEOUT_NS);
    EXPECT_EQ(fixture.memory.created_releases, 0u);
    EXPECT_TRUE(has_device_failed(fixture));
}

// Engines whose registration failed report no marked failure, as the GPU core holds no device of theirs to fail, and
// keep their copy vector on
TEST(nvidia_engines, a_marked_failure_is_not_reported_before_registration) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    engines_test_access::unregister(fixture.gpu_engines);

    fixture.failed.set();
    fixture.gpu_engines.report_marked_failure();
    // Registered again before the serve, so a wrongly queued report fails the fixture's device, not a null one
    engines_test_access::register_with(fixture.gpu_engines, fixture.core_device);
    engines_operations().serve();

    EXPECT_FALSE(copy_channel_test_access::is_hung(*engines_test_access::copy_channel_of(fixture.gpu_engines)));
    EXPECT_NOT_NULL(engines_test_access::published_copy_channel(fixture.gpu_engines));
    EXPECT_FALSE(has_device_failed(fixture));
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
// scheduling delayed and the id's USERD slot on page 0 fixed
static constexpr uint32_t USER_CHANNEL_1_FLAGS = 0x0060'01C0;
static constexpr uint32_t USER_CHANNEL_2_FLAGS = 0x0060'02C0;

// NVIDIA's internal flags of a user's channel naming no error notifiers: user privilege and both notifier types NONE
static constexpr uint32_t USER_INTERNAL_FLAGS = 0x14;

// A context's part above its user range: its semaphores, then a 256 KiB window per queue holding the queue's 128 KiB
// GPFIFO and then its push buffer, three pages of 128-byte release sequences
static constexpr uint64_t SEMAPHORES_VA      = 1ULL << 39;
static constexpr uint64_t WINDOW_BYTES       = 256 * 1024;
static constexpr uint64_t FIRST_GPFIFO_VA    = SEMAPHORES_VA + WINDOW_BYTES;
static constexpr uint64_t FIRST_PUSH_VA      = FIRST_GPFIFO_VA + 128 * 1024;
static constexpr uint64_t PUSH_BUFFER_PAGES  = 3;
static constexpr uint64_t RELEASE_SLOT_BYTES = 128;

// A release sequence's words, those holding its wait's scope, its semaphore's address and its payload, and NVIDIA's
// scopes waiting for the current pipe and for every pipe
static constexpr uint32_t RELEASE_WORDS             = 20;
static constexpr uint32_t RELEASE_SCOPE_WORD        = 1;
static constexpr uint32_t RELEASE_ADDRESS_LOW_WORD  = 8;
static constexpr uint32_t RELEASE_ADDRESS_HIGH_WORD = 9;
static constexpr uint32_t RELEASE_PAYLOAD_WORD      = 10;
static constexpr uint32_t WFI_SCOPE_CURRENT         = 0;
static constexpr uint32_t WFI_SCOPE_ALL             = 1;

// A GPFIFO entry's word count from bit 10 of its second word, and NVIDIA's SYNC_WAIT bit there
static constexpr uint32_t GPFIFO_LENGTH_SHIFT = 10;
static constexpr uint32_t GPFIFO_SYNC_WAIT    = 1u << 31;

// Copy engine 2's runlist on the RTX 3080, which a doorbell token holds from bit 16
static constexpr uint32_t COPY_RUNLIST_TOKEN = 1u << 16;

static int32_t create_queue_step(engines_fixture& fixture) {
    return fixture.gpu_engines.create_queue(fixture.space, GPU_QUEUE_COPY, fixture.core_queue, &fixture.queue);
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

// Tears a queue down: off the handler's list, then its channel, its push buffer and the queue itself
static void tear_down_queue(engines_fixture& fixture, gpu::queue* queue) {
    backend_queue& backend = backend_of(queue);
    engines_test_access::unlist_queue(fixture.gpu_engines, backend);
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
    EXPECT_EQ(params.internal_flags, USER_INTERNAL_FLAGS);
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

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// The firmware's channel ids are one space across runlists, so the golden channel takes the lowest id above the
// kernel's copy channel, and gives it back once freed for the first queue to take
TEST(nvidia_engines, the_golden_channel_takes_the_lowest_free_channel_id_and_gives_it_back) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_graphics_on_gpu(fixture), OK);
    EXPECT_EQ(fixture.fw.last_channel.flags, USER_CHANNEL_1_FLAGS);
    EXPECT_EQ(fixture.fw.last_channel.engine_type, GR0);

    fixture.create_placement = GPU_PLACEMENT_SYSTEM;
    ASSERT_EQ(create_on_gpu(fixture), gpu::OK);
    fixture.memory.next_offset = TABLES_OFFSET;
    ASSERT_EQ(create_space_on_gpu(fixture), gpu::OK);
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    EXPECT_EQ(fixture.fw.last_channel.flags, USER_CHANNEL_1_FLAGS);

    tear_down_queue(fixture, fixture.queue);
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

    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_SCOPE_WORD), WFI_SCOPE_CURRENT);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_ADDRESS_LOW_WORD), 0u);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_ADDRESS_HIGH_WORD), 0x80u);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_PAYLOAD_WORD), 1u);
    EXPECT_EQ(put_of_queue(fixture.queue), 3u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), COPY_RUNLIST_TOKEN | 1);

    tear_down_queue(fixture, fixture.queue);
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

    tear_down_queue(fixture, fixture.queue);
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

    tear_down_queue(fixture, fixture.queue);
    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// A queue's channel reads as faulted while either fault bit of its own word in its runlist's channel RAM is set, even
// one bit short of all ones, and not while the word reads all ones, as on a card that stopped responding
TEST(nvidia_engines, a_queues_channel_reads_its_fault_bits_from_its_runlists_channel_ram) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    const channel& user = backend_of(fixture.queue).user_channel;
    ASSERT_EQ(channel_test_access::channel_ram_entry(user), TEST_CHANNEL_RAM + user.id() * CHANNEL_RAM_ENTRY_BYTES);

    uint32_t& word = bar0_word(TEST_CHANNEL_RAM + user.id() * CHANNEL_RAM_ENTRY_BYTES);
    uint32_t& next_word = bar0_word(TEST_CHANNEL_RAM + (user.id() + 1) * CHANNEL_RAM_ENTRY_BYTES);
    word = ~(CHANNEL_RAM_PBDMA_FAULTED | CHANNEL_RAM_ENG_FAULTED);
    next_word = CHANNEL_RAM_PBDMA_FAULTED | CHANNEL_RAM_ENG_FAULTED;
    bool is_faulted_by_other_bits = user.has_faulted();

    word = CHANNEL_RAM_PBDMA_FAULTED;
    bool is_faulted_fetching = user.has_faulted();

    word = CHANNEL_RAM_ENG_FAULTED;
    bool is_faulted_running = user.has_faulted();

    word = 0xFFFFFFFF;
    bool is_faulted_on_a_card_gone = user.has_faulted();

    word = 0xFFFFFFFE;
    bool is_faulted_without_bit_0 = user.has_faulted();

    word = 0x7FFFFFFF;
    bool is_faulted_without_bit_31 = user.has_faulted();

    EXPECT_FALSE(is_faulted_by_other_bits);
    EXPECT_TRUE(is_faulted_fetching);
    EXPECT_TRUE(is_faulted_running);
    EXPECT_FALSE(is_faulted_on_a_card_gone);
    EXPECT_TRUE(is_faulted_without_bit_0);
    EXPECT_TRUE(is_faulted_without_bit_31);

    tear_down_queue(fixture, fixture.queue);
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

    tear_down_queue(fixture, fixture.queue);
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

    tear_down_queue(fixture, fixture.queue);
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

    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// Once the GPU failed, a queue is refused before the driver task sees it and the task reports the failure, and a
// run writes no entry and rings no doorbell
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
    engines_operations().serve();

    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(fixture.fw.allocations, allocations);
    EXPECT_EQ(put_of_queue(first), 0u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), doorbell);
    EXPECT_TRUE(has_device_failed(fixture));

    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// Once a hung zeroing failed the GPU core's device, the driver task refuses a queue before creating anything
TEST(nvidia_engines, a_queue_is_refused_once_a_hung_zeroing_failed_the_device) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));

    fixture.create_placement = GPU_PLACEMENT_VIDEO;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 3;
    ASSERT_EQ(create_on_gpu(fixture), gpu::ERR_DEVICE);
    uint32_t allocations = fixture.fw.allocations;
    gpu_nvidia_range range = {.va = 0x10000, .size = 4, .flags = 0};

    EXPECT_EQ(create_queue_on_gpu(fixture), gpu::ERR_DEVICE);
    EXPECT_EQ(fixture.gpu_engines.check_commands(&range, sizeof(range)), gpu::ERR_DEVICE);

    EXPECT_TRUE(fixture.queue == nullptr);
    EXPECT_EQ(fixture.fw.allocations, allocations);
    tear_down_space(fixture);
}

// A GPU core context on the fixture's device and two copy queues the handler reports to, holding nothing outside
// themselves, so each test builds them anew in place
struct progress_core {
    explicit progress_core(gpu::device& dev)
        : ctx(dev), first(dev, ctx, GPU_QUEUE_COPY), second(dev, ctx, GPU_QUEUE_COPY) {}

    gpu::context    ctx;
    gpu::core_queue first;
    gpu::core_queue second;
};

alignas(progress_core) static uint8_t s_progress_core_bytes[sizeof(progress_core)];

// Builds the GPU core the handler reports to, with its context the one `fixture` creates its next space for and its
// first queue the one `fixture` creates its next queue for
static progress_core& build_progress_core(engines_fixture& fixture) {
    auto* core = new (s_progress_core_bytes) progress_core(fixture.core_device);
    fixture.core_context = &core->ctx;
    fixture.core_queue = &core->first;
    return *core;
}

static uint32_t& semaphore_word(engines_fixture& fixture, uint32_t slot) {
    return reinterpret_cast<uint32_t*>(static_cast<backend_space*>(fixture.space)->semaphores.virt)[slot];
}

// Raises `vectors` in the CPU interrupt tree's first leaf and runs the handler's service
static void service_vectors(engines_fixture& fixture, uint32_t vectors) {
    bar0_word(CPU_INTR_LEAF) = vectors;
    RUN_ELEVATED(fixture.gpu_engines.service_interrupt(fixture.bar0));
}

// Releases `payload` in the semaphore of `slot` as the end of its queue's commands would, then raises the copy vector
static void release(engines_fixture& fixture, uint32_t slot, uint32_t payload) {
    semaphore_word(fixture, slot) = payload;
    service_vectors(fixture, COPY_VECTOR_BIT);
}

// Runs sequence numbers `first` to `last` on `queue` as the GPU core does for `core_queue`
static void run_sequences(
    engines_fixture& fixture,
    gpu::queue* queue,
    gpu::core_queue& core_queue,
    uint64_t first,
    uint64_t last
) {
    for (uint64_t sequence = first; sequence <= last; sequence++) {
        fixture.gpu_engines.run(queue, sequence, nullptr, 0);
    }

    core_queue.last_run = last;
}

// The handler reports a queue's release to its GPU core queue once the copy engine's vector is pending, and each later
// release as it comes
TEST(nvidia_engines, a_queues_release_reaches_the_core_once_the_copy_vector_is_pending) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    run_sequences(fixture, fixture.queue, core.first, 1, 3);

    semaphore_word(fixture, 0) = 2;
    service_vectors(fixture, OTHER_VECTOR_BIT);
    EXPECT_EQ(core.first.last_finished, 0u);

    service_vectors(fixture, COPY_VECTOR_BIT);
    EXPECT_EQ(core.first.last_finished, 2u);

    release(fixture, 0, 3);
    EXPECT_EQ(core.first.last_finished, 3u);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// A release holds a sequence number's low 32 bits, which the handler widens against the last run across 2^32
TEST(nvidia_engines, a_release_widens_against_the_last_run_across_two_to_the_32) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    uint64_t last_run = (1ULL << 32) + 5;
    run_sequences(fixture, fixture.queue, core.first, last_run, last_run);

    release(fixture, 0, 0xFFFF'FFFC);
    EXPECT_EQ(core.first.last_finished, (1ULL << 32) - 4);

    release(fixture, 0, 3);
    EXPECT_EQ(core.first.last_finished, (1ULL << 32) + 3);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// A release past the last run or at most the last reported, as a queue's own commands may write, reports nothing
TEST(nvidia_engines, a_release_outside_the_running_sequences_reports_nothing) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    run_sequences(fixture, fixture.queue, core.first, 1, 3);

    release(fixture, 0, 2);
    ASSERT_EQ(core.first.last_finished, 2u);

    release(fixture, 0, 7);
    EXPECT_EQ(core.first.last_finished, 2u);

    release(fixture, 0, 1);
    EXPECT_EQ(core.first.last_finished, 2u);

    release(fixture, 0, 3);
    EXPECT_EQ(core.first.last_finished, 3u);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// Each listed queue's release reaches its own GPU core queue from one service
TEST(nvidia_engines, each_queues_release_reaches_its_own_core_queue) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;

    fixture.core_queue = &core.second;
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    run_sequences(fixture, first, core.first, 1, 2);
    run_sequences(fixture, fixture.queue, core.second, 1, 3);

    semaphore_word(fixture, 0) = 1;
    release(fixture, 1, 3);
    EXPECT_EQ(core.first.last_finished, 1u);
    EXPECT_EQ(core.second.last_finished, 3u);

    release(fixture, 0, 2);
    EXPECT_EQ(core.first.last_finished, 2u);
    EXPECT_EQ(core.second.last_finished, 3u);

    tear_down_queue(fixture, fixture.queue);
    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// A queue's semaphore starts at zero whatever was written in its slot before, so no stale release ends its first
// submission
TEST(nvidia_engines, a_queues_semaphore_starts_at_zero) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;
    semaphore_word(fixture, 1) = 1;

    fixture.core_queue = &core.second;
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    run_sequences(fixture, fixture.queue, core.second, 1, 1);
    service_vectors(fixture, COPY_VECTOR_BIT);

    EXPECT_EQ(semaphore_word(fixture, 1), 0u);
    EXPECT_EQ(core.second.last_finished, 0u);

    tear_down_queue(fixture, fixture.queue);
    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// Lists `queue` on its device and counts its stop, as GPU_QUEUE_CREATE does around the backend's create
static bool track_on_core(gpu::core_queue& queue) {
    bool is_listed = false;
    RUN_ELEVATED(is_listed = queue.dev.list_queue(queue));
    queue.ctx.stop_count.fetch_add_relaxed(1);
    return is_listed;
}

// Whether the engines came up with a space over system memory and a queue for `core`'s first queue, tracked by `core`
static bool sets_up_a_tracked_queue(engines_fixture& fixture, progress_core& core) {
    return sets_up_system_memory_and_a_space(fixture) && create_queue_on_gpu(fixture) == gpu::OK &&
           track_on_core(core.first);
}

// A stop frees the queue's channel objects on the driver task, never in the call, then takes the queue off the
// handler's list and reports it stopped
TEST(nvidia_engines, a_stop_frees_the_channel_on_the_task_then_reports_the_queue_stopped) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    backend_queue& queue = backend_of(fixture.queue);
    uint32_t releases = fixture.fw.releases;

    fixture.gpu_engines.stop_queue(fixture.queue);
    EXPECT_EQ(fixture.fw.releases, releases);
    EXPECT_FALSE(core.first.stopped);

    engines_operations().serve();
    EXPECT_EQ(fixture.fw.releases, releases + 3);
    EXPECT_EQ(page_entry_at(FIRST_GPFIFO_VA), 0u);
    EXPECT_FALSE(engines_test_access::is_listed(fixture.gpu_engines, queue));
    EXPECT_TRUE(core.first.stopped);
    EXPECT_FALSE(has_device_failed(fixture));

    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// A destroy unmaps the stopped queue's push buffer and gives back its window and channel id, which the next queue takes
TEST(nvidia_engines, a_destroy_gives_back_the_queues_window_and_channel_id) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    uint32_t channel_id = backend_of(fixture.queue).channel_id;
    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();

    fixture.gpu_engines.destroy_queue(fixture.queue);
    engines_operations().serve();
    EXPECT_EQ(page_entry_at(FIRST_PUSH_VA), 0u);

    fixture.core_queue = &core.second;
    ASSERT_EQ(create_queue_on_gpu(fixture), gpu::OK);
    EXPECT_EQ(fixture.fw.last_channel.gpfifo_address, FIRST_GPFIFO_VA);
    EXPECT_EQ(backend_of(fixture.queue).channel_id, channel_id);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// The word of `queue`'s channel in the copy engine's channel RAM
static uint32_t& channel_ram_word(gpu::queue* queue) {
    return bar0_word(TEST_CHANNEL_RAM + backend_of(queue).user_channel.id() * CHANNEL_RAM_ENTRY_BYTES);
}

// A GPU core context with one queue, for a queue listed between two of `progress_core`'s in a space of its own, built
// anew in place by each test
struct lone_core {
    explicit lone_core(gpu::device& dev) : ctx(dev), queue(dev, ctx, GPU_QUEUE_COPY) {}

    gpu::context    ctx;
    gpu::core_queue queue;
};

alignas(lone_core) static uint8_t s_lone_core_bytes[sizeof(lone_core)];

// The backend's queues for `progress_core`'s first queue, `lone_core`'s and `progress_core`'s second, in the order the
// handler's list holds them, and the space `lone_core`'s is in
struct three_queues {
    gpu::queue* first;
    gpu::queue* middle;
    gpu::queue* last;
    gpu::space* middle_space;
};

// Whether the engines came up with system memory and `out`'s queues, each reading its fault bits from the word
// `channel_ram_word` gives, which reads 0
static bool sets_up_three_queues(engines_fixture& fixture, progress_core& core, lone_core& lone, three_queues& out) {
    if (!sets_up_system_memory_and_a_space(fixture) || create_queue_on_gpu(fixture) != gpu::OK) {
        return false;
    }

    gpu::space* shared_space = fixture.space;
    out.first = fixture.queue;
    fixture.core_context = &lone.ctx;
    fixture.core_queue = &lone.queue;
    if (create_space_on_gpu(fixture) != gpu::OK || create_queue_on_gpu(fixture) != gpu::OK) {
        return false;
    }

    out.middle_space = fixture.space;
    out.middle = fixture.queue;
    fixture.space = shared_space;
    fixture.core_queue = &core.second;
    if (create_queue_on_gpu(fixture) != gpu::OK) {
        return false;
    }

    out.last = fixture.queue;
    gpu::queue* listed[] = {out.first, out.middle, out.last};
    for (gpu::queue* queue : listed) {
        const channel& user = backend_of(queue).user_channel;
        bool reads_its_own_word =
            channel_test_access::channel_ram_entry(user) == TEST_CHANNEL_RAM + user.id() * CHANNEL_RAM_ENTRY_BYTES;
        if (!reads_its_own_word) {
            return false;
        }

        channel_ram_word(queue) = 0;
    }

    return true;
}

// Tears down the three queues, then both spaces
static void tear_down_three_queues(engines_fixture& fixture, const three_queues& queues) {
    tear_down_queue(fixture, queues.last);
    tear_down_queue(fixture, queues.middle);
    tear_down_queue(fixture, queues.first);
    fixture.gpu_engines.destroy_space(queues.middle_space);
    tear_down_space(fixture);
}

// After an MMU fault's report the driver task, not the call, has the GPU core lose the context of each listed queue
// whose channel reads faulted, here one between two of another context, at fault address 0, and logs how many it lost
TEST(nvidia_engines, a_fault_report_loses_the_contexts_of_the_queues_whose_channels_read_faulted) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));
    channel_ram_word(queues.middle) = CHANNEL_RAM_ENG_FAULTED;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    uint32_t lost_in_the_call = lone.ctx.lost_reason;
    engines_operations().serve();
    log::set_backend(nullptr);

    const char* loss_line = "reported an MMU fault, and the GPU core lost the context of each faulted queue, 1 in all";
    EXPECT_EQ(lost_in_the_call, 0u);
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(lone.ctx.fault_va, 0u);
    EXPECT_EQ(core.ctx.lost_reason, 0u);
    EXPECT_TRUE(engines_log_contains(loss_line));

    tear_down_three_queues(fixture, queues);
}

// An engine error's report while no channel reads faulted loses no context and warns, and a later one loses a faulted
// queue's context for an engine error, here one listed between two of another context
TEST(nvidia_engines, an_engine_error_report_loses_a_faulted_queues_context_for_an_engine_error) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::engine);
    engines_operations().serve();
    log::set_backend(nullptr);
    uint32_t lost_unfaulted = lone.ctx.lost_reason;
    bool warns_unfaulted = engines_log_contains("reported an engine error, but no queue's channel reads faulted");

    channel_ram_word(queues.middle) = CHANNEL_RAM_PBDMA_FAULTED;
    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::engine);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(lost_unfaulted, 0u);
    EXPECT_TRUE(warns_unfaulted);

    const char* loss_line =
        "reported an engine error, and the GPU core lost the context of each faulted queue, 1 in all";
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_ENGINE);
    EXPECT_EQ(lone.ctx.fault_va, 0u);
    EXPECT_EQ(core.ctx.lost_reason, 0u);
    EXPECT_TRUE(engines_log_contains(loss_line));

    tear_down_three_queues(fixture, queues);
}

// Faults reported before the driver task serves the first are attributed together, here for queues of two contexts
TEST(nvidia_engines, faults_reported_before_a_serve_are_attributed_together) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    channel_ram_word(queues.first) = CHANNEL_RAM_ENG_FAULTED;
    channel_ram_word(queues.middle) = CHANNEL_RAM_ENG_FAULTED;
    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    fixture.gpu_engines.on_channel_error(channel_error::fault);

    // A record queued twice would link to itself, so the check runs before any serve and cuts such a link
    EXPECT_TRUE(engines_test_access::is_fault_attribution_queued_once(fixture.gpu_engines));
    engines_operations().serve();
    log::set_backend(nullptr);

    const char* loss_line = "reported an MMU fault, and the GPU core lost the context of each faulted queue, 2 in all";
    EXPECT_EQ(core.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_TRUE(engines_log_contains(loss_line));

    tear_down_three_queues(fixture, queues);
}

// A faulting address that sets bits in both of the record's words, the high one past its 16th bit
static constexpr uint64_t RECORDED_FAULT_VA = 0x1'2345'6789'A000;

// Writes channel id `id`, which must be below 10, over each `#` in `line`
static void write_channel_id(char* line, uint32_t id) {
    for (char* c = line; *c != '\0'; c++) {
        if (*c == '#') {
            *c = static_cast<char>('0' + id);
        }
    }
}

// An MMU fault's report asks for each listed queue's fault record and loses the context of the queue whose record
// names a fault, at the record's address, though channel RAM marks none, as after the graphics engine's fault
TEST(nvidia_engines, a_fault_report_loses_a_queues_context_at_the_address_its_record_names) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    uint32_t middle_id = backend_of(queues.middle).user_channel.id();
    ASSERT_TRUE(middle_id < 10);
    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();
    fixture.fw.faulted_address = RECORDED_FAULT_VA;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(fixture.fw.fault_records, 3u);
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(lone.ctx.fault_va, RECORDED_FAULT_VA);
    EXPECT_EQ(core.ctx.lost_reason, 0u);

    char record_line[] =
        "the GSP's firmware records an MMU fault of type FAULT_PDE at GPU address 0x123456789A000 on channel #";
    write_channel_id(record_line, middle_id);
    const char* loss_line = "reported an MMU fault, and the GPU core lost the context of each faulted queue, 1 in all";
    EXPECT_TRUE(engines_log_contains(record_line));
    EXPECT_TRUE(engines_log_contains(loss_line));
    EXPECT_FALSE(engines_log_contains("of type NONE"));

    tear_down_three_queues(fixture, queues);
}

// A queue that both its fault record and its channel RAM mark faulted loses its context once, at the record's address
TEST(nvidia_engines, a_queue_both_sources_mark_faulted_loses_its_context_once_at_the_records_address) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();
    fixture.fw.faulted_address = RECORDED_FAULT_VA;
    channel_ram_word(queues.middle) = CHANNEL_RAM_ENG_FAULTED;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    const char* loss_line = "reported an MMU fault, and the GPU core lost the context of each faulted queue, 1 in all";
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(lone.ctx.fault_va, RECORDED_FAULT_VA);
    EXPECT_EQ(core.ctx.lost_reason, 0u);
    EXPECT_TRUE(engines_log_contains(loss_line));

    tear_down_three_queues(fixture, queues);
}

// A fault report the firmware sends during a record's read queues the attribution again, which skips the queue the
// first lost, so the record and the loss are each logged once
TEST(nvidia_engines, a_fault_report_during_a_records_read_logs_the_loss_once) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();
    fixture.fw.reports_during_record = true;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(fixture.fw.fault_records, 5u);
    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(engines_log_count("records an MMU fault of type FAULT_PDE"), 1u);
    EXPECT_EQ(engines_log_count("the GPU core lost the context of each faulted queue, 1 in all"), 1u);
    EXPECT_FALSE(engines_log_contains("but no queue's channel reads faulted"));

    tear_down_three_queues(fixture, queues);
}

// A record's type name that fills its 32 bytes unterminated is read to its 31st byte, so one that only begins with
// NONE names a fault
TEST(nvidia_engines, a_record_name_filling_its_bytes_is_read_to_its_31st_byte) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();
    fixture.fw.faulted_name = "NONEXXXXXXXXXXXXXXXXXXXXXXXXXXXX";

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_TRUE(engines_log_contains("of type NONEXXXXXXXXXXXXXXXXXXXXXXXXXXX at GPU address 0x0 on channel"));

    tear_down_three_queues(fixture, queues);
}

// An engine error's report asks for no fault record, so a queue only its record names faulted keeps its context
TEST(nvidia_engines, an_engine_error_report_asks_for_no_fault_record) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::engine);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(fixture.fw.fault_records, 0u);
    EXPECT_EQ(lone.ctx.lost_reason, 0u);
    EXPECT_TRUE(engines_log_contains("reported an engine error, but no queue's channel reads faulted"));

    tear_down_three_queues(fixture, queues);
}

// A fault record the firmware refuses names no fault and is logged, and channel RAM still attributes the fault, at
// address 0
TEST(nvidia_engines, a_refused_fault_record_leaves_the_fault_to_channel_ram) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    lone_core& lone = *new (s_lone_core_bytes) lone_core(fixture.core_device);
    three_queues queues = {};
    ASSERT_TRUE(sets_up_three_queues(fixture, core, lone, queues));

    uint32_t middle_id = backend_of(queues.middle).user_channel.id();
    ASSERT_TRUE(middle_id < 10);
    fixture.fw.refused_command = GET_MMU_FAULT_INFO;
    fixture.fw.faulted_channel = backend_of(queues.middle).user_channel.handle();
    fixture.fw.faulted_address = RECORDED_FAULT_VA;
    channel_ram_word(queues.middle) = CHANNEL_RAM_ENG_FAULTED;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(lone.ctx.lost_reason, GPU_LOST_FAULT);
    EXPECT_EQ(lone.ctx.fault_va, 0u);
    EXPECT_EQ(core.ctx.lost_reason, 0u);

    char refused_line[] = "nvidia: asking the GSP's firmware for channel #'s MMU fault record failed: -12";
    write_channel_id(refused_line, middle_id);
    EXPECT_TRUE(engines_log_contains(refused_line));

    tear_down_three_queues(fixture, queues);
}

// A fault record whose reply misses its deadline fails the GPU, so the attribution stops without losing the queue's
// context, though channel RAM marks its channel faulted, and has the failure reported, which ends the queue
TEST(nvidia_engines, a_fault_record_missing_its_deadline_has_the_failed_gpu_reported) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));

    fixture.fw.gpu_failed = &fixture.failed;
    fixture.fw.missed_command = GET_MMU_FAULT_INFO;
    channel_ram_word(fixture.queue) = CHANNEL_RAM_ENG_FAULTED;

    start_keeping_log();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(fixture.fw.fault_records, 1u);
    EXPECT_EQ(core.ctx.lost_reason, 0u);
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_FALSE(engines_log_contains("nvidia: the firmware reported an MMU fault"));

    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// Once the GPU failed, a channel error loses no context, since the failure's report ends every queue
TEST(nvidia_engines, once_the_gpu_failed_a_channel_error_loses_no_context) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    channel_ram_word(fixture.queue) = CHANNEL_RAM_ENG_FAULTED;

    fixture.failed.set();
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    EXPECT_EQ(core.ctx.lost_reason, 0u);
    EXPECT_EQ(fixture.fw.fault_records, 0u);

    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();
    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// Once the GPU failed, a stop frees nothing and has the driver task report the failure, which frees no channel, masks
// the copy engine's vector, takes every queue off the handler's list and ends each, and a destroy keeps their memory
TEST(nvidia_engines, once_the_gpu_failed_a_stop_frees_nothing_and_the_failures_report_ends_the_queue) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    backend_queue& queue = backend_of(fixture.queue);
    uint32_t releases = fixture.fw.releases;
    uint32_t updates = fixture.memory.table_updates;

    fixture.failed.set();
    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();

    EXPECT_FALSE(engines_test_access::is_listed(fixture.gpu_engines, queue));
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), COPY_VECTOR_BIT);

    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);

    EXPECT_EQ(fixture.fw.releases, releases);
    EXPECT_EQ(fixture.memory.table_updates, updates);
    EXPECT_NE(page_entry_at(FIRST_PUSH_VA), 0u);
}

// A stop the driver task reaches after the GPU failed frees nothing, and the failure's report ends the queue
TEST(nvidia_engines, a_stop_reached_after_the_gpu_failed_leaves_the_queue_to_the_failures_report) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    engines& gpu_engines = fixture.gpu_engines;
    uint32_t releases = fixture.fw.releases;

    gpu_engines.stop_queue(fixture.queue);
    fixture.memory.update_result = ERR_TIMEOUT;
    EXPECT_EQ(gpu_engines.map(fixture.space, MAPPED_VA, fixture.created, 0, 0x1000, 0, false), gpu::ERR_DEVICE);
    engines_operations().serve();

    EXPECT_EQ(fixture.fw.releases, releases);
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(has_device_failed(fixture));

    gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// A stop whose GPFIFO unmap misses its invalidate's deadline fails the GPU, and the failure's report ends the queue in
// place of its stop
TEST(nvidia_engines, a_stop_whose_unmap_fails_the_gpu_leaves_the_queue_to_the_failures_report) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    backend_queue& queue = backend_of(fixture.queue);

    fixture.memory.update_result = ERR_TIMEOUT;
    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(queue.user_channel.keeps_memory());
    EXPECT_FALSE(engines_test_access::is_listed(fixture.gpu_engines, queue));
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(has_device_failed(fixture));

    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// A channel release the firmware refuses fails the GPU core's device in place of the queue's stop, since the channel
// may still run, and the failure's report ends the queue
TEST(nvidia_engines, a_refused_channel_release_fails_the_device_in_place_of_the_queues_stop) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    backend_queue& queue = backend_of(fixture.queue);
    uint64_t gpu_ns = 0;

    fixture.fw.release_result = ERR_FIRMWARE;
    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();
    fixture.fw.release_result = OK;

    EXPECT_TRUE(queue.user_channel.keeps_memory());
    EXPECT_FALSE(engines_test_access::is_listed(fixture.gpu_engines, queue));
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_EQ(fixture.gpu_engines.read_timestamp(gpu_ns), gpu::ERR_DEVICE);
    EXPECT_FALSE(fixture.failed.is_set());

    fixture.gpu_engines.destroy_queue(fixture.queue);
    tear_down_space(fixture);
}

// A destroy whose push buffer unmap misses its invalidate's deadline fails the GPU, which the driver task reports
TEST(nvidia_engines, a_destroy_whose_unmap_fails_the_gpu_reports_it_to_the_core) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_a_tracked_queue(fixture, core));
    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();

    fixture.memory.update_result = ERR_TIMEOUT;
    fixture.gpu_engines.destroy_queue(fixture.queue);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));

    tear_down_space(fixture);
}

// A space destroy whose invalidate misses its deadline fails the GPU, which the driver task reports to the core
TEST(nvidia_engines, a_space_destroy_that_fails_the_gpu_reports_it_to_the_core) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_system_memory_and_a_space(fixture));
    fixture.memory.update_result = ERR_TIMEOUT;

    tear_down_space(fixture);

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));
}

// Whether the engines came up with a space over system memory and a queue for each of `core`'s queues, both tracked by
// `core`, the first returned in `first`
static bool sets_up_two_tracked_queues(engines_fixture& fixture, progress_core& core, gpu::queue*& first) {
    if (!sets_up_a_tracked_queue(fixture, core)) {
        return false;
    }

    first = fixture.queue;
    fixture.core_queue = &core.second;

    return create_queue_on_gpu(fixture) == gpu::OK && track_on_core(core.second);
}

// Fails the GPU core's device alone with a video memory create whose zeroing hangs the copy channel, keeping the
// fixture's created memory
static int32_t hang_a_zeroing(engines_fixture& fixture) {
    gpu::memory* created = fixture.created;
    fixture.create_placement = GPU_PLACEMENT_VIDEO;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 3;

    int32_t rc = create_on_gpu(fixture);
    fixture.created = created;

    return rc;
}

// A hung zeroing fails the GPU core's device alone, so the failure's report has the firmware free each queue's channel,
// keeping its memory, with its GPFIFO and USERD still mapped, logs how many it freed and masks the copy engine's vector
TEST(nvidia_engines, a_hung_zeroing_has_the_firmware_free_every_queues_channel) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    gpu::queue* first = nullptr;
    ASSERT_TRUE(sets_up_two_tracked_queues(fixture, core, first));
    uint32_t releases = fixture.fw.releases;

    // The report may run before `hang_a_zeroing` returns, since `act_as_gpu` serves operations while the create waits
    start_keeping_log();
    int32_t rc = hang_a_zeroing(fixture);
    engines_operations().serve();
    log::set_backend(nullptr);
    ASSERT_EQ(rc, gpu::ERR_DEVICE);

    EXPECT_EQ(fixture.fw.releases, releases + 2 * QUEUE_CHANNEL_OBJECTS);
    EXPECT_TRUE(engines_log_contains("the firmware freed 2 user channels once the GPU core's device failed"));
    EXPECT_TRUE(backend_of(first).user_channel.keeps_memory());
    EXPECT_TRUE(backend_of(fixture.queue).user_channel.keeps_memory());

    uint64_t read_only_system = PAGE_READ_ONLY | PAGE_SYSTEM_BITS | PAGE_VALID;
    EXPECT_TRUE(channel_test_access::are_gpfifo_and_userd_mapped(backend_of(first).user_channel));
    EXPECT_TRUE(channel_test_access::are_gpfifo_and_userd_mapped(backend_of(fixture.queue).user_channel));
    EXPECT_EQ(page_entry_at(FIRST_GPFIFO_VA) & read_only_system, read_only_system);

    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), COPY_VECTOR_BIT);
    EXPECT_TRUE(core.first.stopped);
    EXPECT_TRUE(core.second.stopped);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_FALSE(fixture.failed.is_set());

    fixture.gpu_engines.destroy_queue(fixture.queue);
    fixture.gpu_engines.destroy_queue(first);
    tear_down_space(fixture);
}

// A free whose reply misses its deadline marks the GPU failed, after which the failure's report frees no further
// channel, so the other queue's channel runs on
TEST(nvidia_engines, the_failures_report_frees_no_channel_once_a_free_failed_the_gpu) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    gpu::queue* first = nullptr;
    ASSERT_TRUE(sets_up_two_tracked_queues(fixture, core, first));

    uint32_t releases = fixture.fw.releases;
    fixture.fw.gpu_failed = &fixture.failed;
    fixture.fw.missed_release = releases + 1;

    ASSERT_EQ(hang_a_zeroing(fixture), gpu::ERR_DEVICE);
    engines_operations().serve();

    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_EQ(fixture.fw.releases, releases + QUEUE_CHANNEL_OBJECTS);
    EXPECT_TRUE(backend_of(first).user_channel.keeps_memory());
    EXPECT_FALSE(backend_of(fixture.queue).user_channel.keeps_memory());
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), COPY_VECTOR_BIT);
    EXPECT_TRUE(has_device_failed(fixture));

    fixture.gpu_engines.destroy_queue(fixture.queue);
    fixture.gpu_engines.destroy_queue(first);
    tear_down_space(fixture);
}

// Where created ranges go in a graphics test, apart from the tables, which context buffers would otherwise split
static constexpr uint64_t CREATED_APART_OFFSET = 0x20000000;

// The firmware objects a graphics queue's channel allocates: its channel group, channel, and 3D, compute and copy
// objects, the first two before its promotion, which lists its own two buffers and the six global ones it maps
static constexpr uint32_t GRAPHICS_QUEUE_CHANNEL_OBJECTS = 5;
static constexpr uint32_t OBJECTS_BEFORE_PROMOTION       = 2;
static constexpr uint32_t CHANNEL_PROMOTION_ENTRIES      = 8;

// The six global buffers map from 1 GiB above the user range, the attribute buffer first and the RTV buffer at its
// alignment's next multiple, then slot 0's own buffers, the golden channel's too, the main buffer first
static constexpr uint64_t GRAPHICS_BUFFERS_VA  = (1ULL << 39) + (1ULL << 30);
static constexpr uint64_t RTV_VA               = GRAPHICS_BUFFERS_VA + RTV_LAYOUT.alignment;
static constexpr uint64_t PRIV_ACCESS_MAP_VA   = RTV_VA + RTV_LAYOUT.bytes + FECS_EVENT_LAYOUT.bytes;
static constexpr uint64_t FIRST_OWN_BUFFERS_VA = PRIV_ACCESS_MAP_VA + PRIV_ACCESS_MAP_LAYOUT.bytes;
static constexpr uint64_t OWN_BUFFERS_STRIDE   = OWN_MAIN_BYTES + PATCH_LAYOUT.bytes;

// A page table entry's privilege bit and its address field's bits 53:8
static constexpr uint64_t PAGE_PRIVILEGED    = 1ULL << 5;
static constexpr uint64_t PAGE_ADDRESS_FIELD = 0x003F'FFFF'FFFF'FF00;

// The graphics engine's runlist on the RTX 3080 as a doorbell token holds it
static constexpr uint32_t GRAPHICS_RUNLIST_TOKEN = 0u << 16;

// The entry a map wrote for `va` into the page table the golden channel's space adds sixth, after its root, the two
// directories it hands the firmware and the two directories above its buffers
static uint64_t golden_page_entry_at(uint64_t va) {
    return s_tables_image[5 * TABLE_WORDS + (va >> 12) % TABLE_WORDS];
}

// The entry a map wrote for `va` into the page table a space adds seventh, for the graphics buffers, after the
// directory below the semaphores' that they add sixth
static uint64_t& graphics_page_entry_at(uint64_t va) {
    return s_tables_image[6 * TABLE_WORDS + (va >> 12) % TABLE_WORDS];
}

// A writable entry for the GPU's privileged accesses to video memory at `offset`
static uint64_t privileged_entry(uint64_t offset) {
    return address_field(offset) | PAGE_PRIVILEGED | PAGE_VALID;
}

// Whether the golden channel's space maps `buffer`'s first page at its address for privileged accesses, read-only if
// `read_only`
static bool is_golden_mapped(const context_buffer& buffer, bool read_only) {
    uint64_t entry = privileged_entry(buffer.range.offset) | (read_only ? PAGE_READ_ONLY : 0);
    return golden_page_entry_at(buffer.gpu_address) == entry;
}

// Whether a table the double kept holds an entry naming video memory at `offset`
static bool is_mapped_anywhere(uint64_t offset) {
    for (uint64_t word : s_tables_image) {
        if (word != UNWRITTEN_ENTRY && (word & PAGE_ADDRESS_FIELD) == address_field(offset)) {
            return true;
        }
    }

    return false;
}

// The golden channel's space maps the global buffers and the golden channel's own for the GPU's privileged accesses
// alone, the register access map read-only, and leaves the unrestricted map unmapped
TEST(nvidia_engines, the_golden_channel_space_maps_the_context_buffers_privileged_and_the_access_map_read_only) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_on_gpu(fixture), OK);
    fixture.memory.next_offset = TABLES_OFFSET;
    fixture.memory.created_offset = CREATED_APART_OFFSET;
    ASSERT_EQ(run_on_gpu(fixture, [](engines_fixture& f) { return f.gpu_engines.bring_up_graphics(); }), OK);

    const global_context_buffers& global = engines_test_access::global_buffers_of(fixture.gpu_engines);
    EXPECT_EQ(global.attribute.gpu_address, GRAPHICS_BUFFERS_VA);
    EXPECT_EQ(global.rtv.gpu_address, RTV_VA);

    EXPECT_TRUE(is_golden_mapped(global.attribute, false));
    EXPECT_TRUE(is_golden_mapped(global.bundle, false));
    EXPECT_TRUE(is_golden_mapped(global.page_pool, false));
    EXPECT_TRUE(is_golden_mapped(global.rtv, false));

    EXPECT_TRUE(is_golden_mapped(global.fecs_event, false));
    EXPECT_TRUE(is_golden_mapped(global.priv_access_map, true));
    EXPECT_FALSE(is_mapped_anywhere(global.unrestricted_priv_access_map.range.offset));

    uint64_t main_offset = fixture.memory.last_released.offset;
    uint64_t patch_va = FIRST_OWN_BUFFERS_VA + OWN_MAIN_BYTES;
    EXPECT_EQ(golden_page_entry_at(FIRST_OWN_BUFFERS_VA), privileged_entry(main_offset));
    EXPECT_EQ(golden_page_entry_at(patch_va), privileged_entry(main_offset + OWN_MAIN_BYTES));
}

// The golden channel's GPFIFO lies at the first queue slot's address, above the 256 GiB whose directory the split
// space hands the firmware
TEST(nvidia_engines, golden_channel_gpfifo_lies_at_the_first_queue_slots_address) {
    engines_fixture fixture;
    ASSERT_EQ(bring_up_graphics_on_gpu(fixture), OK);

    EXPECT_EQ(fixture.fw.last_channel.gpfifo_address, FIRST_GPFIFO_VA);
}

// Whether the engines came up with their graphics engine, system memory created and a space whose tables start at
// `TABLES_OFFSET`, with later created ranges apart from them
static bool sets_up_graphics_and_a_space(engines_fixture& fixture) {
    fixture.create_placement = GPU_PLACEMENT_SYSTEM;
    if (bring_up_graphics_on_gpu(fixture) != OK || create_on_gpu(fixture) != gpu::OK) {
        return false;
    }

    fixture.memory.next_offset = TABLES_OFFSET;
    fixture.memory.created_offset = CREATED_APART_OFFSET;
    return create_space_on_gpu(fixture) == gpu::OK;
}

static int32_t create_graphics_queue_step(engines_fixture& fixture) {
    return fixture.gpu_engines.create_queue(fixture.space, GPU_QUEUE_GRAPHICS, fixture.core_queue, &fixture.queue);
}

static int32_t create_graphics_queue_on_gpu(engines_fixture& fixture) {
    return run_on_gpu(fixture, create_graphics_queue_step);
}

// A graphics queue is an unprivileged channel on the graphics engine's runlist, promoted with its own context buffers
// and the global ones, then given 3D, compute and copy objects, the copy one on the graphics runlist's copy engine
TEST(nvidia_engines, a_graphics_queue_is_promoted_then_given_3d_compute_and_copy_objects) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t allocations = fixture.fw.allocations;

    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);

    const rm::channel_params& params = fixture.fw.last_channel;
    EXPECT_EQ(params.flags, USER_CHANNEL_1_FLAGS);
    EXPECT_EQ(params.internal_flags, USER_INTERNAL_FLAGS);
    EXPECT_EQ(params.engine_type, GR0);
    EXPECT_EQ(params.gpfifo_address, FIRST_GPFIFO_VA);

    EXPECT_EQ(fixture.fw.promotions, 2u);
    EXPECT_EQ(fixture.fw.promoted_channel, backend_of(fixture.queue).user_channel.handle());
    EXPECT_EQ(fixture.fw.promoted_entries, CHANNEL_PROMOTION_ENTRIES);
    EXPECT_EQ(fixture.fw.promoted_after, allocations + OBJECTS_BEFORE_PROMOTION);

    ASSERT_EQ(fixture.fw.allocations, allocations + GRAPHICS_QUEUE_CHANNEL_OBJECTS);
    EXPECT_EQ(fixture.fw.classes[allocations + 2], AMPERE_B);
    EXPECT_EQ(fixture.fw.classes[allocations + 3], AMPERE_COMPUTE_B);
    EXPECT_EQ(fixture.fw.classes[allocations + 4], AMPERE_DMA_COPY_B);
    EXPECT_EQ(fixture.fw.last_copy_engine, CE0);

    EXPECT_EQ(fixture.fw.bound_after, allocations + GRAPHICS_QUEUE_CHANNEL_OBJECTS);
    EXPECT_EQ(fixture.fw.scheduled_after, allocations + GRAPHICS_QUEUE_CHANNEL_OBJECTS);
    EXPECT_EQ(fixture.fw.last_command, GPFIFO_SCHEDULE);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// A graphics queue's release waits for every pipe, as its 3D and compute work may run at once, and its run rings the
// doorbell with the graphics runlist's token
TEST(nvidia_engines, a_graphics_queues_release_waits_for_every_pipe) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);

    fixture.gpu_engines.run(fixture.queue, 1, nullptr, 0);

    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_SCOPE_WORD), WFI_SCOPE_ALL);
    EXPECT_EQ(release_word_of(fixture.queue, 1, RELEASE_PAYLOAD_WORD), 1u);
    EXPECT_EQ(bar0_word(VIRTUAL_FUNCTION_DOORBELL), GRAPHICS_RUNLIST_TOKEN | 1);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// A space's first graphics queue maps the global context buffers for privileged accesses, the access map read-only,
// and each graphics queue maps its own two in its slot, so a second queue leaves the global ones as they are
TEST(nvidia_engines, graphics_queues_map_the_global_buffers_once_and_their_own_in_their_slots) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);
    gpu::queue* first = fixture.queue;

    const global_context_buffers& global = engines_test_access::global_buffers_of(fixture.gpu_engines);
    const channel_context_buffers& own = backend_of(first).own_buffers;
    uint64_t read_only_map = privileged_entry(global.priv_access_map.range.offset) | PAGE_READ_ONLY;

    EXPECT_EQ(graphics_page_entry_at(GRAPHICS_BUFFERS_VA), privileged_entry(global.attribute.range.offset));
    EXPECT_EQ(graphics_page_entry_at(PRIV_ACCESS_MAP_VA), read_only_map);
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA), privileged_entry(own.main.range.offset));
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA + OWN_MAIN_BYTES), privileged_entry(own.patch.range.offset));

    graphics_page_entry_at(GRAPHICS_BUFFERS_VA) = UNWRITTEN_ENTRY;
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);

    const channel_context_buffers& second = backend_of(fixture.queue).own_buffers;
    uint64_t second_main_va = FIRST_OWN_BUFFERS_VA + OWN_BUFFERS_STRIDE;
    EXPECT_EQ(graphics_page_entry_at(GRAPHICS_BUFFERS_VA), UNWRITTEN_ENTRY);
    EXPECT_EQ(graphics_page_entry_at(second_main_va), privileged_entry(second.main.range.offset));
    EXPECT_EQ(graphics_page_entry_at(second_main_va + OWN_MAIN_BYTES), privileged_entry(second.patch.range.offset));

    tear_down_queue(fixture, fixture.queue);
    tear_down_queue(fixture, first);
    tear_down_space(fixture);
}

// The handler reports a graphics queue's release once the graphics engine's vector is pending, clearing that vector
// alone
TEST(nvidia_engines, a_graphics_queues_release_reaches_the_core_on_the_graphics_vector) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);
    run_sequences(fixture, fixture.queue, core.first, 1, 2);

    semaphore_word(fixture, 0) = 2;
    service_vectors(fixture, OTHER_VECTOR_BIT);
    EXPECT_EQ(core.first.last_finished, 0u);

    service_vectors(fixture, GRAPHICS_VECTOR_BIT | OTHER_VECTOR_BIT);
    EXPECT_EQ(core.first.last_finished, 2u);
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF), GRAPHICS_VECTOR_BIT);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}

// A destroyed graphics queue unmaps and gives back its own context buffers, while the global ones stay mapped
TEST(nvidia_engines, a_destroyed_graphics_queue_gives_back_its_own_context_buffers) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);
    ASSERT_TRUE(track_on_core(core.first));
    uint32_t releases = fixture.memory.created_releases;

    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();
    fixture.gpu_engines.destroy_queue(fixture.queue);
    engines_operations().serve();

    const global_context_buffers& global = engines_test_access::global_buffers_of(fixture.gpu_engines);
    EXPECT_EQ(fixture.memory.created_releases, releases + 2);
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA), 0u);
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA + OWN_MAIN_BYTES), 0u);
    EXPECT_EQ(graphics_page_entry_at(GRAPHICS_BUFFERS_VA), privileged_entry(global.attribute.range.offset));

    tear_down_space(fixture);
}

// A destroyed graphics queue whose own buffer's unmap is refused after its push buffer's keeps its own buffers, its
// slot and its channel id, since the refused update fails the GPU, which may still reach them
TEST(nvidia_engines, a_graphics_queue_whose_own_buffer_unmap_is_refused_keeps_its_buffers_slot_and_channel_id) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);
    ASSERT_TRUE(track_on_core(core.first));
    uint32_t releases = fixture.memory.created_releases;

    fixture.gpu_engines.stop_queue(fixture.queue);
    engines_operations().serve();
    uint32_t updates = fixture.memory.table_updates;
    fixture.memory.refused_update = updates + 2; // the main buffer's unmap, after the push buffer's
    fixture.gpu_engines.destroy_queue(fixture.queue);
    engines_operations().serve();

    EXPECT_EQ(fixture.memory.table_updates, updates + 2);
    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_EQ(fixture.memory.created_releases, releases);
    EXPECT_EQ(static_cast<backend_space*>(fixture.space)->queue_slots, 1u);
    EXPECT_TRUE(engines_test_access::holds_channel_id(fixture.gpu_engines, 1));

    tear_down_space(fixture);
}

// A graphics queue whose compute object the firmware refuses frees its channel and gives back its own context
// buffers, its slot and its channel id
TEST(nvidia_engines, a_graphics_queue_the_firmware_refuses_gives_back_its_own_context_buffers) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    fixture.fw.refused_class = AMPERE_COMPUTE_B;
    uint32_t firmware_releases = fixture.fw.releases;
    uint32_t releases = fixture.memory.created_releases;

    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);
    EXPECT_EQ(fixture.fw.releases, firmware_releases + 3); // its 3D object, channel and channel group
    EXPECT_EQ(fixture.fw.last_command, GPU_PROMOTE_CTX); // no bind or schedule after the promotion
    EXPECT_EQ(fixture.memory.created_releases, releases + 2);
    EXPECT_EQ(static_cast<backend_space*>(fixture.space)->queue_slots, 0u);
    EXPECT_FALSE(engines_test_access::holds_channel_id(fixture.gpu_engines, 1));

    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA), 0u);
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA + OWN_MAIN_BYTES), 0u);

    fixture.fw.refused_class = ~0u;
    tear_down_space(fixture);
}

// A graphics queue whose own buffer's zeroing hangs fails the device before the driver task sees the queue, and the
// failure's report masks the graphics engine's vector
TEST(nvidia_engines, a_graphics_queue_whose_zeroing_hangs_fails_the_device) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t allocations = fixture.fw.allocations;
    uint32_t releases = fixture.memory.created_releases;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 2 + GRAPHICS_BUFFER_COUNT + 1; // its main buffer's, after the check's and bring-up's

    const char* line = "copy channel hung zeroing a graphics context buffer, so the GPU core's device failed";
    start_keeping_log();
    int32_t rc = create_graphics_queue_on_gpu(fixture);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(rc, gpu::ERR_DEVICE);
    EXPECT_TRUE(engines_log_contains(line));
    EXPECT_EQ(fixture.fw.allocations, allocations);
    EXPECT_EQ(fixture.memory.created_releases, releases);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_EQ(bar0_word(CPU_INTR_LEAF_EN_CLEAR), GRAPHICS_VECTOR_BIT);

    tear_down_space(fixture);
}

// A graphics queue whose patch buffer's zeroing hangs gives back its main buffer, which no copy writes any more
TEST(nvidia_engines, a_graphics_queue_whose_patch_zeroing_hangs_gives_back_its_main_buffer) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t releases = fixture.memory.created_releases;
    fixture.fault = copy_fault::hangs;
    fixture.faulty_fill = 2 + GRAPHICS_BUFFER_COUNT + 2;

    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_DEVICE);
    engines_operations().serve();
    EXPECT_EQ(fixture.memory.created_releases, releases + 1);
    EXPECT_EQ(fixture.memory.last_released.size, OWN_MAIN_BYTES);

    tear_down_space(fixture);
}

// A graphics queue whose zeroing the GPU's failure elsewhere ended reports that failure without naming the hang as the
// device's cause
TEST(nvidia_engines, a_graphics_queue_whose_zeroing_the_gpus_failure_ended_names_no_hang) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    fixture.fault = copy_fault::fails_gpu;
    fixture.faulty_fill = 2 + GRAPHICS_BUFFER_COUNT + 1;

    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_DEVICE);
    engines_operations().serve();
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_FALSE(engines_test_access::is_device_failed(fixture.gpu_engines));

    tear_down_space(fixture);
}

// A graphics queue whose own buffers' zeroing ends just before the GPU fails gives them back once the driver task
// finds the GPU failed, naming no hang
TEST(nvidia_engines, a_graphics_queue_the_task_finds_after_the_gpus_failure_gives_back_its_own_buffers) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t releases = fixture.memory.created_releases;
    fixture.fault = copy_fault::fails_gpu_after;
    fixture.faulty_fill = 2 + GRAPHICS_BUFFER_COUNT + 2; // its patch buffer's

    start_keeping_log();
    int32_t rc = create_graphics_queue_on_gpu(fixture);
    engines_operations().serve();
    log::set_backend(nullptr);

    EXPECT_EQ(rc, gpu::ERR_DEVICE);
    EXPECT_EQ(fixture.memory.created_releases, releases + 2);
    EXPECT_TRUE(has_device_failed(fixture));
    EXPECT_FALSE(engines_log_contains("hung"));

    tear_down_space(fixture);
}

// A graphics queue the video memory has no room for keeps none of its own context buffers, logs no buffer line and
// leaves the device up
TEST(nvidia_engines, a_graphics_queue_without_room_for_its_own_buffers_keeps_none) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t releases = fixture.memory.created_releases;
    fixture.memory.refused_allocation = fixture.memory.allocations + 2;

    start_keeping_log();
    int32_t rc = create_graphics_queue_on_gpu(fixture);
    log::set_backend(nullptr);

    EXPECT_EQ(rc, gpu::ERR_NO_MEMORY);
    EXPECT_EQ(fixture.memory.created_releases, releases + 1);
    EXPECT_FALSE(has_device_failed(fixture));
    EXPECT_FALSE(engines_log_contains("graphics context buffer"));

    tear_down_space(fixture);
}

// A graphics queue refused at a full slot mask or with every channel id held gives back its own context buffers, which
// it never mapped
TEST(nvidia_engines, a_graphics_queue_without_a_slot_or_a_channel_id_gives_back_its_own_buffers) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    auto* space = static_cast<backend_space*>(fixture.space);
    uint32_t releases = fixture.memory.created_releases;

    space->queue_slots = 0xFF;
    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);
    EXPECT_EQ(fixture.memory.created_releases, releases + 2);

    space->queue_slots = 0;
    engines_test_access::hold_every_channel_id(fixture.gpu_engines);
    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_NO_MEMORY);
    EXPECT_EQ(fixture.memory.created_releases, releases + 4);
    EXPECT_EQ(graphics_page_entry_at(FIRST_OWN_BUFFERS_VA), UNWRITTEN_ENTRY);

    tear_down_space(fixture);
}

// A space's first graphics queue whose semaphore page's map is refused gives back its own context buffers, which it
// never mapped, though the refused update fails the GPU
TEST(nvidia_engines, a_graphics_queue_whose_semaphore_page_map_is_refused_gives_back_its_own_buffers) {
    engines_fixture fixture;
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    uint32_t releases = fixture.memory.created_releases;
    fixture.memory.refused_update = fixture.memory.table_updates + 1;

    EXPECT_EQ(create_graphics_queue_on_gpu(fixture), gpu::ERR_DEVICE);
    engines_operations().serve();
    EXPECT_TRUE(fixture.failed.is_set());
    EXPECT_EQ(fixture.memory.created_releases, releases + 2);

    tear_down_space(fixture);
}

// The word of `queue`'s channel in the graphics engine's channel RAM
static uint32_t& graphics_channel_ram_word(gpu::queue* queue) {
    return bar0_word(TEST_GRAPHICS_CHANNEL_RAM + backend_of(queue).user_channel.id() * CHANNEL_RAM_ENTRY_BYTES);
}

// A graphics queue reads its fault bits in the graphics engine's channel RAM, so a fault report loses its context once
// its word there reads faulted, and never for its id's word in the copy engine's channel RAM
TEST(nvidia_engines, a_fault_report_loses_a_graphics_queues_context_by_the_graphics_engines_channel_ram) {
    engines_fixture fixture;
    progress_core& core = build_progress_core(fixture);
    ASSERT_TRUE(sets_up_graphics_and_a_space(fixture));
    ASSERT_EQ(create_graphics_queue_on_gpu(fixture), gpu::OK);
    ASSERT_TRUE(track_on_core(core.first));

    graphics_channel_ram_word(fixture.queue) = 0;
    channel_ram_word(fixture.queue) = CHANNEL_RAM_ENG_FAULTED;
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    EXPECT_EQ(core.ctx.lost_reason, 0u);

    graphics_channel_ram_word(fixture.queue) = CHANNEL_RAM_ENG_FAULTED;
    fixture.gpu_engines.on_channel_error(channel_error::fault);
    engines_operations().serve();
    EXPECT_EQ(core.ctx.lost_reason, GPU_LOST_FAULT);

    tear_down_queue(fixture, fixture.queue);
    tear_down_space(fixture);
}
