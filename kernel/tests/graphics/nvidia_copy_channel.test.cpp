// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/copy_channel.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "dynpriv/dynpriv.h"
#include "hw/mmio.h"
#include "tests/framework/helpers.h"
#include "tests/graphics/nvidia_channel_test_access.h"
#include "tests/graphics/nvidia_copy_channel_test_access.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_copy_channel);

using drivers::nvidia::channel_allocation_flags;
using drivers::nvidia::channel_test_access;
using drivers::nvidia::chip_info;
using drivers::nvidia::copy_channel;
using drivers::nvidia::copy_channel_test_access;
using drivers::nvidia::COPY_SLOTS;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::engine_info;
using drivers::nvidia::entry_run;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::failed_mark;
using drivers::nvidia::FILL_BYTES_MAX;
using drivers::nvidia::FILL_END_MAX;
using drivers::nvidia::firmware;
using drivers::nvidia::gpu_objects;
using drivers::nvidia::memory_report;
using drivers::nvidia::OK;
using drivers::nvidia::page_aperture;
using drivers::nvidia::page_entry;
using drivers::nvidia::page_mapping;
using drivers::nvidia::registers;
using drivers::nvidia::table_writes;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace rm = drivers::nvidia::rm;

// NVIDIA's classes, controls, privilege, USERD offset, doorbell and channel parameter offsets, restated here so the
// tests hold the copy channel to NVIDIA's layout rather than to the driver's
static constexpr uint32_t FERMI_VASPACE_A              = 0x90F1;
static constexpr uint32_t SET_PAGE_DIRECTORY           = 0x801813;
static constexpr uint32_t UNSET_PAGE_DIRECTORY         = 0x801814;
static constexpr uint32_t GET_FAULT_METHOD_BUFFER_SIZE = 0x20802A08;
static constexpr uint32_t KEPLER_CHANNEL_GROUP_A       = 0xA06C;
static constexpr uint32_t AMPERE_CHANNEL_GPFIFO_A      = 0xC56F;
static constexpr size_t   CHANNEL_PARAMS_BYTES         = 360;
static constexpr uint32_t AMPERE_DMA_COPY_B            = 0xC7B5;
static constexpr uint32_t BIND                         = 0xA06F0104;
static constexpr uint32_t GPFIFO_SCHEDULE              = 0xA06F0103;
static constexpr uint32_t PRIVILEGE_KERNEL             = 0x2;
static constexpr uint32_t USERD_GP_PUT                 = 0x8C;
static constexpr uint32_t VIRTUAL_FUNCTION_DOORBELL    = 0xBB0090;
static constexpr size_t   GPFIFO_OFFSET_AT             = 8;
static constexpr size_t   FLAGS_AT                     = 20;
static constexpr size_t   INTERNAL_FLAGS_AT            = 244;

// NVIDIA's method headers of the fill's first method and of the release's semaphore address, on subchannel 0
static constexpr uint32_t SET_OBJECT_HEADER  = 0x20010000;
static constexpr uint32_t SEM_ADDR_LO_HEADER = 0x20050017;

// The copy channel's GPU addresses and the words of a fill and of a fill with its release
static constexpr uint64_t GPFIFO_ADDRESS      = 1ULL << 39;
static constexpr uint64_t PUSH_BUFFER_ADDRESS = GPFIFO_ADDRESS + 0x20000;
static constexpr uint64_t SEMAPHORE_ADDRESS   = PUSH_BUFFER_ADDRESS + 0x1000;
static constexpr uint32_t FILL_WORDS          = 15;
static constexpr uint32_t ENTRY_WORDS         = 35;

static constexpr gpu_objects TEST_OBJECTS    = {.device = 0xC0C00001, .subdevice = 0xC0C00002};
static constexpr rm::handle  FIRST_HANDLE    = 0xC0C00010;
static constexpr engine_info COPY2_ENGINE    = {.type = 0xB, .runlist = 3, .channel_ram = 0};
static constexpr uint32_t    TEST_CHANNEL_ID = 0;
static constexpr uint64_t    FILL_OFFSET     = 0x200000;
static constexpr uint64_t    FILL_BYTES      = 0x10000;
static constexpr uint32_t    FILL_VALUE      = 0xA5A5A5A5;
static constexpr uint64_t    SHORT_TIMEOUT   = 2 * drivers::nvidia::NS_PER_MS;

// A firmware recording each call's kind, objects and command or class, plus the channel allocation's parameters,
// answering the fault method buffer's size and call `i` with `results[i]`
class copy_channel_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 16;

    enum class kind : uint8_t { allocate, control, release };

    struct call {
        kind       what;
        rm::handle parent;
        rm::handle object;
        uint32_t   number;
        uint32_t   first_word;
    };

    call       calls[MAX_CALLS] = {};
    uint32_t   call_count = 0;
    int32_t    results[MAX_CALLS] = {};
    uint8_t    channel_params[CHANNEL_PARAMS_BYTES] = {};
    rm::handle next_handle = FIRST_HANDLE;

    int32_t allocate(rm::handle parent, rm::handle object, uint32_t class_id, void* params, size_t size) override {
        if (class_id == AMPERE_CHANNEL_GPFIFO_A && size == CHANNEL_PARAMS_BYTES) {
            string::memcpy(channel_params, params, size);
        }

        return record(kind::allocate, parent, object, class_id, params, size);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t size) override {
        if (command == GET_FAULT_METHOD_BUFFER_SIZE && size == sizeof(uint32_t)) {
            uint32_t bytes = 20480;
            string::memcpy(params, &bytes, sizeof(bytes));
        }

        return record(kind::control, 0, object, command, params, size);
    }

    int32_t release(rm::handle object) override {
        return record(kind::release, 0, object, 0, nullptr, 0);
    }

    rm::handle new_handle() override { return next_handle++; }

    rm::handle client() const override { return 0; }

    int32_t internal_control(uint32_t, void*, size_t) override { return OK; }

    void set_event_sink(drivers::nvidia::firmware_event_sink*) override {}

    void set_error_sink(drivers::nvidia::firmware_error_sink*) override {}

    uint32_t channel_word(size_t offset) const {
        uint32_t word = 0;
        string::memcpy(&word, channel_params + offset, sizeof(word));
        return word;
    }

    uint64_t channel_doubleword(size_t offset) const {
        uint64_t doubleword = 0;
        string::memcpy(&doubleword, channel_params + offset, sizeof(doubleword));
        return doubleword;
    }

private:
    int32_t record(kind what, rm::handle parent, rm::handle object, uint32_t number, void* params, size_t size) {
        uint32_t index = call_count++;
        if (index >= MAX_CALLS) {
            return ERR_FIRMWARE;
        }

        uint32_t first_word = 0;
        if (params != nullptr && size >= sizeof(first_word)) {
            string::memcpy(&first_word, params, sizeof(first_word));
        }

        calls[index] = {.what = what, .parent = parent, .object = object, .number = number, .first_word = first_word};
        return results[index];
    }
};

// The instance block's stand-in, which the channel clears through `map_for_cpu`
alignas(4096) static uint8_t s_copy_instance_block[4096];

// A video memory handing out ranges from 4 MiB on, mapping each for the CPU onto `s_copy_instance_block` and recording
// the page table entries each update writes
class copy_channel_video_memory : public video_memory {
public:
    static constexpr uint32_t MAX_ENTRIES = 64;

    uint64_t next_offset = 0x400000;
    uint64_t entries[MAX_ENTRIES] = {};
    uint32_t entry_count = 0;
    uint32_t releases = 0;

    void report(memory_report& out) override {
        out = {};
    }

    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        out = {.offset = next_offset, .size = size};
        next_offset += size;
        return OK;
    }

    void release(const vram_range&) override { releases++; }

    int32_t map_for_cpu(const vram_range&, cpu_mapping& out) override {
        out = {.virt = reinterpret_cast<uintptr_t>(s_copy_instance_block), .phys = 0};
        return OK;
    }

    void unmap_for_cpu(const vram_range&) override {}

    int32_t update_context_tables(uint64_t, table_writes& writes, drivers::nvidia::context_tlbs) override {
        entry_run run = {};
        while (writes.next(run)) {
            for (uint64_t i = 0; i < run.count && entry_count < MAX_ENTRIES; i++) {
                if (run.entries[i] != 0) {
                    entries[entry_count++] = run.entries[i];
                }
            }
        }

        return OK;
    }

    bool wrote_entry(uint64_t entry) const {
        for (uint32_t i = 0; i < entry_count; i++) {
            if (entries[i] == entry) {
                return true;
            }
        }

        return false;
    }
};

// The doorbell's stand-in, which `registers` reaches at NVIDIA's offset from a base placed before it
static uint32_t s_copy_doorbell = 0;

static registers copy_doorbell_registers() {
    return registers(reinterpret_cast<uintptr_t>(&s_copy_doorbell) - VIRTUAL_FUNCTION_DOORBELL);
}

static dma::device_profile copy_test_card() {
    return dma::device_profile_test_access::build(64, true);
}

static chip_info copy_test_chip() {
    chip_info chip = {};
    chip.channel_class = AMPERE_CHANNEL_GPFIFO_A;
    chip.copy_class = AMPERE_DMA_COPY_B;
    return chip;
}

// The copy channel over doubles, destroyed at the end of the test
struct copy_fixture {
    copy_channel_firmware     fw;
    copy_channel_video_memory memory;
    failed_mark               failed;
    copy_channel              copier;

    copy_fixture() : copier(fw, TEST_OBJECTS, memory, copy_doorbell_registers(), failed) { s_copy_doorbell = 0; }

    ~copy_fixture() { copier.destroy(); }

    int32_t create() { return copier.create(copy_test_card(), copy_test_chip(), COPY2_ENGINE, TEST_CHANNEL_ID); }

    uint32_t put() const {
        return channel_test_access::userd_word(copy_channel_test_access::queue(copier), USERD_GP_PUT);
    }

    bool reaches_put(uint32_t target) const {
        uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
        while (put() != target) {
            if (clock::now_ns() > deadline) {
                return false;
            }
        }

        return true;
    }
};

// A fill a task runs, since a fill that waits must not run on the test runner, the boot CPU's idle task
struct fill_task_state {
    copy_channel*          copier;
    uint64_t               offset;
    uint64_t               bytes;
    int32_t                result;
    sync::atomic<uint32_t> is_done;
};

// What `fill_in_task` returns for a fill whose task did not finish, a value no error of the driver has
static constexpr int32_t NOT_FINISHED = 1;

static void run_fill_task(void* arg) {
    fill_task_state* state = static_cast<fill_task_state*>(arg);
    state->result = state->copier->fill(state->offset, state->bytes, FILL_VALUE);
    state->is_done.store_release(1);
    sched::exit(0);
}

static sched::task* start_fill_task(
    fill_task_state& state,
    copy_channel& copier,
    uint64_t offset = FILL_OFFSET,
    uint64_t bytes = FILL_BYTES
) {
    state.copier = &copier;
    state.offset = offset;
    state.bytes = bytes;
    state.result = NOT_FINISHED;
    state.is_done.store_relaxed(0);
    return test_helpers::start_pinned_task(run_fill_task, &state, "copy_fill");
}

// Runs a fill to its end in a task and returns its result
static int32_t fill_in_task(copy_channel& copier, uint64_t offset = FILL_OFFSET, uint64_t bytes = FILL_BYTES) {
    fill_task_state state;
    sched::task* filler = start_fill_task(state, copier, offset, bytes);
    if (filler == nullptr) {
        return NOT_FINISHED;
    }

    bool is_done = test_helpers::spin_wait(state.is_done);
    test_helpers::unpin(filler);

    return is_done ? state.result : NOT_FINISHED;
}

// Create allocates the address space and hands it its page directory, then the channel group in it and a privileged
// channel whose GPFIFO maps above the user range, with its copy object, bound and scheduled
TEST(nvidia_copy_channel, create_runs_a_privileged_channel_in_its_own_space) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);

    copy_channel_firmware& fw = fixture.fw;
    ASSERT_EQ(fw.call_count, 8u);

    uint32_t expected[] = {
        FERMI_VASPACE_A,
        SET_PAGE_DIRECTORY,
        GET_FAULT_METHOD_BUFFER_SIZE,
        KEPLER_CHANNEL_GROUP_A,
        AMPERE_CHANNEL_GPFIFO_A,
        AMPERE_DMA_COPY_B,
        BIND,
        GPFIFO_SCHEDULE,
    };

    for (uint32_t i = 0; i < fw.call_count; i++) {
        EXPECT_EQ(fw.calls[i].number, expected[i]);
    }

    EXPECT_EQ(fw.calls[0].parent, TEST_OBJECTS.device);
    EXPECT_EQ(fw.channel_doubleword(GPFIFO_OFFSET_AT), GPFIFO_ADDRESS);
    EXPECT_EQ(fw.channel_word(FLAGS_AT), channel_allocation_flags(TEST_CHANNEL_ID, true));
    EXPECT_EQ(fw.channel_word(INTERNAL_FLAGS_AT), PRIVILEGE_KERNEL);
    EXPECT_EQ(fixture.copier.create(copy_test_card(), copy_test_chip(), COPY2_ENGINE, TEST_CHANNEL_ID), ERR_BAD_STATE);
}

// The GPU may read the push buffer but not write it, and its release writes the semaphore, both in system memory
TEST(nvidia_copy_channel, create_maps_the_push_buffer_read_only_and_the_semaphore_writable) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);

    uint64_t push_buffer = copy_channel_test_access::push_buffer_phys(fixture.copier);
    page_mapping read_only = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = true};
    page_mapping writable = {.aperture = page_aperture::system_memory, .kind = 0, .read_only = false};

    EXPECT_TRUE(fixture.memory.wrote_entry(page_entry(push_buffer, read_only)));
    EXPECT_TRUE(fixture.memory.wrote_entry(page_entry(push_buffer + 0x1000, writable)));
    EXPECT_FALSE(fixture.memory.wrote_entry(page_entry(push_buffer, writable)));
}

// A refused channel releases its group, then takes the directory back and releases the address space
TEST(nvidia_copy_channel, refused_channel_releases_the_address_space) {
    copy_fixture fixture;
    fixture.fw.results[4] = ERR_FIRMWARE;
    EXPECT_EQ(fixture.create(), ERR_FIRMWARE);

    copy_channel_firmware& fw = fixture.fw;
    ASSERT_EQ(fw.call_count, 8u);
    EXPECT_TRUE(fw.calls[5].what == copy_channel_firmware::kind::release);
    EXPECT_EQ(fw.calls[5].object, FIRST_HANDLE + 1);

    EXPECT_EQ(fw.calls[6].number, UNSET_PAGE_DIRECTORY);
    EXPECT_TRUE(fw.calls[7].what == copy_channel_firmware::kind::release);
    EXPECT_EQ(fw.calls[7].object, FIRST_HANDLE);
}

// A refused group release after a refused channel keeps the address space and the channel's memory for good, as destroy
// keeps them, since the driver cannot tell what the firmware still holds
TEST(nvidia_copy_channel, refused_group_release_keeps_the_address_space) {
    copy_fixture fixture;
    fixture.fw.results[4] = ERR_FIRMWARE;
    fixture.fw.results[5] = ERR_FIRMWARE;
    EXPECT_EQ(fixture.create(), ERR_FIRMWARE);

    EXPECT_EQ(fixture.fw.call_count, 6u);
    EXPECT_EQ(fixture.memory.releases, 0u);
    EXPECT_EQ(fixture.create(), ERR_BAD_STATE);
}

// A fill writes its methods and release sequence into its slot, runs both through one GPFIFO entry, publishes GP_PUT
// and rings the doorbell, then returns once the handler's service finds its sequence number in the semaphore
TEST(nvidia_copy_channel, fill_runs_through_one_entry_until_service_sees_its_release) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));

    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, 0), SET_OBJECT_HEADER);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, 1), AMPERE_DMA_COPY_B);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, 3), FILL_VALUE);

    uint32_t semaphore_low = static_cast<uint32_t>(SEMAPHORE_ADDRESS);
    uint32_t semaphore_high = static_cast<uint32_t>(SEMAPHORE_ADDRESS >> 32);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, FILL_WORDS + 7), SEM_ADDR_LO_HEADER);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, FILL_WORDS + 8), semaphore_low);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, FILL_WORDS + 9), semaphore_high);
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 1, FILL_WORDS + 10), 1u);

    const drivers::nvidia::channel& queue = copy_channel_test_access::queue(fixture.copier);
    uint64_t slot_address = PUSH_BUFFER_ADDRESS + copy_channel_test_access::SLOT_WORDS * sizeof(uint32_t);
    uint32_t entry_high = static_cast<uint32_t>(slot_address >> 32) | (ENTRY_WORDS << 10);
    EXPECT_EQ(channel_test_access::gpfifo_word(queue, 0, 0), static_cast<uint32_t>(slot_address));
    EXPECT_EQ(channel_test_access::gpfifo_word(queue, 0, 1), entry_high);

    EXPECT_EQ(fixture.put(), 1u);
    EXPECT_EQ(s_copy_doorbell, (COPY2_ENGINE.runlist << 16) | TEST_CHANNEL_ID);

    copy_channel_test_access::release(fixture.copier, 1);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// A fill whose copy misses its deadline hangs the channel, so the next fill returns at once without submitting
TEST(nvidia_copy_channel, fill_past_its_deadline_hangs_the_channel) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_fill_timeout(fixture.copier, SHORT_TIMEOUT);

    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 1u);

    copy_channel_test_access::release(fixture.copier, 1);
    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 1u);
}

// A fill while all but one slot run fills waits for the oldest, and once that one passes its deadline the channel is
// hung: the fill submits nothing, and neither does the next after the oldest finishes
TEST(nvidia_copy_channel, fill_without_a_free_slot_submits_nothing_past_its_deadline) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_fill_timeout(fixture.copier, SHORT_TIMEOUT);
    copy_channel_test_access::set_submitted(fixture.copier, COPY_SLOTS - 1);

    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 0u);
    EXPECT_EQ(s_copy_doorbell, 0u);

    copy_channel_test_access::release(fixture.copier, 1);
    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 0u);
}

// A fill finding the oldest running fill past the deadline hangs the channel, so it and the next submit nothing
TEST(nvidia_copy_channel, fill_after_the_oldest_passes_the_deadline_submits_nothing) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 1);
    copy_channel_test_access::set_deadline(fixture.copier, clock::now_ns());

    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 0u);
    EXPECT_EQ(s_copy_doorbell, 0u);

    copy_channel_test_access::release(fixture.copier, 1);
    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 0u);
}

// Marking the channel hung ends at once both a fill waiting for its copy and one waiting for a slot, and the next fill
// submits nothing
TEST(nvidia_copy_channel, marking_the_channel_hung_ends_its_waiting_fills_at_once) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    uint64_t start_ns = clock::now_ns();
    copy_channel_test_access::set_submitted(fixture.copier, COPY_SLOTS - 2);

    fill_task_state copying;
    sched::task* copying_fill = start_fill_task(copying, fixture.copier);
    ASSERT_NOT_NULL(copying_fill);
    ASSERT_TRUE(fixture.reaches_put(1));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(copying_fill));

    fill_task_state queued;
    sched::task* queued_fill = start_fill_task(queued, fixture.copier);
    ASSERT_NOT_NULL(queued_fill);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(queued_fill));

    fixture.copier.mark_hung();
    EXPECT_TRUE(test_helpers::spin_wait(copying.is_done));
    EXPECT_TRUE(test_helpers::spin_wait(queued.is_done));
    uint64_t took_ns = clock::now_ns() - start_ns;
    test_helpers::unpin(copying_fill);
    test_helpers::unpin(queued_fill);

    EXPECT_EQ(copying.result, ERR_TIMEOUT);
    EXPECT_EQ(queued.result, ERR_TIMEOUT);
    EXPECT_LT(took_ns, GPU_SUBMIT_TIMEOUT_NS);
    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 1u);
}

// Progress a fill reads restarts the deadline for the next running fill, so the fill runs though the old one passed,
// and the release of an earlier fill wakes it only to wait again
TEST(nvidia_copy_channel, progress_restarts_the_deadline_for_the_next_fill) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 2);
    copy_channel_test_access::set_deadline(fixture.copier, clock::now_ns());
    copy_channel_test_access::release(fixture.copier, 1);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(fixture.put(), 1u);

    copy_channel_test_access::release(fixture.copier, 2);
    RUN_ELEVATED(fixture.copier.service());
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(fixture.put(), 1u);

    copy_channel_test_access::release(fixture.copier, 3);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// Progress with fills still running restarts the deadline a full timeout from now, not from the deadline before
TEST(nvidia_copy_channel, progress_restarts_the_deadline_a_full_timeout_from_now) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 2);
    uint64_t old_deadline_ns = copy_channel_test_access::deadline(fixture.copier);
    copy_channel_test_access::release(fixture.copier, 1);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));

    uint64_t deadline_ns = copy_channel_test_access::deadline(fixture.copier);
    EXPECT_GT(deadline_ns, old_deadline_ns);
    EXPECT_LE(deadline_ns, clock::now_ns() + GPU_SUBMIT_TIMEOUT_NS);

    copy_channel_test_access::release(fixture.copier, 3);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// A fill submitted while no fill runs starts a fresh deadline, whatever deadline the earlier fills left
TEST(nvidia_copy_channel, fill_on_an_idle_channel_starts_a_fresh_deadline) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 2);
    copy_channel_test_access::set_deadline(fixture.copier, clock::now_ns());
    copy_channel_test_access::release(fixture.copier, 2);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(fixture.put(), 1u);

    copy_channel_test_access::release(fixture.copier, 3);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// A fill submitted behind a running fill leaves the deadline as it was, so later fills never extend the oldest's
TEST(nvidia_copy_channel, fill_behind_a_running_fill_keeps_the_deadline) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 1);
    uint64_t deadline_ns = copy_channel_test_access::deadline(fixture.copier);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(fixture.put(), 1u);
    EXPECT_EQ(copy_channel_test_access::deadline(fixture.copier), deadline_ns);

    copy_channel_test_access::release(fixture.copier, 2);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// A semaphore ahead of the last fill submitted counts no fill as finished, so the fill is submitted and times out
TEST(nvidia_copy_channel, fill_ignores_a_semaphore_ahead_of_its_last_fill) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_fill_timeout(fixture.copier, SHORT_TIMEOUT);
    copy_channel_test_access::release(fixture.copier, 5);

    EXPECT_EQ(fill_in_task(fixture.copier), ERR_TIMEOUT);
    EXPECT_EQ(fixture.put(), 1u);
}

// Ranges the copy engine cannot fill in 4-byte elements are refused, and so is a fill before create. Each runs in a
// task, so a range wrongly taken times out instead of blocking the test runner
TEST(nvidia_copy_channel, fill_refuses_ranges_it_cannot_fill) {
    copy_fixture fixture;
    EXPECT_EQ(fill_in_task(fixture.copier), ERR_BAD_STATE);
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_fill_timeout(fixture.copier, SHORT_TIMEOUT);

    EXPECT_EQ(fill_in_task(fixture.copier, FILL_OFFSET + 2, FILL_BYTES), ERR_INVALID_ARG);
    EXPECT_EQ(fill_in_task(fixture.copier, FILL_OFFSET, 6), ERR_INVALID_ARG);
    EXPECT_EQ(fill_in_task(fixture.copier, FILL_OFFSET, 0), ERR_INVALID_ARG);
    EXPECT_EQ(fill_in_task(fixture.copier, FILL_OFFSET, FILL_BYTES_MAX + 4), ERR_INVALID_ARG);
    EXPECT_EQ(fill_in_task(fixture.copier, FILL_END_MAX - 4, 8), ERR_INVALID_ARG);
    EXPECT_EQ(fixture.put(), 0u);
}

// A fill waits while all but one slot run fills, and once the oldest finishes is submitted into the slot of its own
// sequence number, which no running fill holds
TEST(nvidia_copy_channel, fill_waits_for_a_slot_while_all_but_one_run) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, COPY_SLOTS - 1);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(fixture.put(), 0u);

    copy_channel_test_access::release(fixture.copier, 1);
    RUN_ELEVATED(fixture.copier.service());
    ASSERT_TRUE(fixture.reaches_put(1));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 0, FILL_WORDS + 10), COPY_SLOTS);

    copy_channel_test_access::release(fixture.copier, COPY_SLOTS);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}

// A release freeing one slot wakes both fills waiting for one, so one is submitted and the other waits again
TEST(nvidia_copy_channel, freed_slot_admits_one_of_two_waiting_fills) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, COPY_SLOTS - 1);

    fill_task_state first_state;
    fill_task_state second_state;
    sched::task* first = start_fill_task(first_state, fixture.copier);
    ASSERT_NOT_NULL(first);
    sched::task* second = start_fill_task(second_state, fixture.copier);
    ASSERT_NOT_NULL(second);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(first));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(second));

    copy_channel_test_access::release(fixture.copier, 1);
    RUN_ELEVATED(fixture.copier.service());
    ASSERT_TRUE(fixture.reaches_put(1));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(first));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(second));
    EXPECT_EQ(fixture.put(), 1u);

    copy_channel_test_access::release(fixture.copier, 2);
    RUN_ELEVATED(fixture.copier.service());
    ASSERT_TRUE(fixture.reaches_put(2));

    copy_channel_test_access::release(fixture.copier, COPY_SLOTS + 1);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(first_state.is_done));
    EXPECT_TRUE(test_helpers::spin_wait(second_state.is_done));
    EXPECT_EQ(first_state.result, OK);
    EXPECT_EQ(second_state.result, OK);
    test_helpers::unpin(first);
    test_helpers::unpin(second);
}

// A fill whose sequence number reaches 2^32 is released as its low 32 bits, which the service still counts as its own
TEST(nvidia_copy_channel, fill_finishes_once_its_sequence_number_passes_32_bits) {
    copy_fixture fixture;
    ASSERT_EQ(fixture.create(), OK);
    copy_channel_test_access::set_submitted(fixture.copier, 0xFFFF'FFFF);
    copy_channel_test_access::release(fixture.copier, 0xFFFF'FFFF);

    fill_task_state state;
    sched::task* filler = start_fill_task(state, fixture.copier);
    ASSERT_NOT_NULL(filler);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(filler));
    EXPECT_EQ(copy_channel_test_access::slot_word(fixture.copier, 0, FILL_WORDS + 10), 0u);

    copy_channel_test_access::release(fixture.copier, 0);
    RUN_ELEVATED(fixture.copier.service());

    EXPECT_TRUE(test_helpers::spin_wait(state.is_done));
    EXPECT_EQ(state.result, OK);
    test_helpers::unpin(filler);
}
