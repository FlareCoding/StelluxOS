// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_cpu_sequencer);

using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::FUNCTION_RUN_CPU_SEQUENCER;
using drivers::nvidia::OK;
using drivers::nvidia::cpu_sequencer_context;
using drivers::nvidia::registers;
using drivers::nvidia::run_cpu_sequencer;

// NVIDIA's opcodes and the payload's header, restated here so the tests hold the sequencer to NVIDIA's format rather
// than to its own constants
static constexpr uint32_t WRITE_REGISTER    = 0;
static constexpr uint32_t MODIFY_REGISTER   = 1;
static constexpr uint32_t POLL_REGISTER     = 2;
static constexpr uint32_t DELAY             = 3;
static constexpr uint32_t STORE_REGISTER    = 4;
static constexpr uint32_t RESET_GSP         = 5;
static constexpr uint32_t START_GSP         = 6;
static constexpr uint32_t WAIT_FOR_GSP_HALT = 7;
static constexpr uint32_t RESUME_GSP        = 8;

static constexpr uint32_t HEADER_WORDS           = 10;
static constexpr uint32_t FIRST_SAVE_AREA_WORD   = 2;
static constexpr uint32_t SAVE_AREA_WORDS        = 8;
static constexpr uint32_t SEQUENCER_HEADER_BYTES = 40;
static constexpr uint32_t SEQUENCER_WORD_BYTES   = 4;

// Registers the steps act on, chosen apart from every register the falcon steps use
static constexpr uint32_t BEFORE_REGISTER = 0x1000;
static constexpr uint32_t STEP_REGISTER   = 0x2000;
static constexpr uint32_t AFTER_REGISTER  = 0x3000;
static constexpr uint32_t WRITTEN_VALUE   = 0x13579BDF;

static constexpr uint64_t SHORT_TIMEOUT_NS = 1000000;
static constexpr uint64_t LONG_TIMEOUT_NS  = 1000000000;

// Far longer than any run here takes, since every wait in these tests ends at once or after a few milliseconds
static constexpr uint64_t RUN_TIME_LIMIT_NS = 100000000;

// A zeroed fake of BAR0, all 16 MiB of it, of which the steps are told half is mapped, so a bound off by a word still
// lands inside the fake
struct sequencer_bar0 {
    static constexpr size_t BYTES        = 0x1000000;
    static constexpr size_t MAPPED_BYTES = BYTES / 2;

    uint8_t*  bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~sequencer_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<uint32_t*>(bytes + offset); }
    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// A payload built a step at a time, whose buffer holds one word more than its steps take, the fewest NVIDIA accepts
struct sequencer_payload {
    static constexpr uint32_t CAPACITY_WORDS = 64;

    uint32_t words[CAPACITY_WORDS] = {};
    uint32_t step_words            = 0;

    template <size_t N>
    void add(const uint32_t (&step)[N]) {
        for (size_t i = 0; i < N; i++) {
            words[HEADER_WORDS + step_words] = step[i];
            step_words++;
        }

        words[0] = step_words + 1;
        words[1] = step_words;
    }

    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(words); }
    uint32_t byte_count() const { return (HEADER_WORDS + step_words) * SEQUENCER_WORD_BYTES; }
};

// The chip's `PMC_BOOT_0` value an RTX 3080 reports
static constexpr uint32_t RTX_3080_BOOT_0 = 0xB72000A1;

static constexpr cpu_sequencer_context sequencer_context(uint64_t ready_timeout_ns, uint64_t deadline_ns) {
    return {
        .register_bytes          = sequencer_bar0::MAPPED_BYTES,
        .boot0                   = RTX_3080_BOOT_0,
        .libos_arguments_address = 0x5A4B3C2D1E000,
        .app_version             = 0x2C1E5A7,
        .ready_timeout_ns        = ready_timeout_ns,
        .deadline_ns             = deadline_ns,
    };
}

// Runs `step` between a write of `BEFORE_REGISTER` and a write of `AFTER_REGISTER`, so a test sees which ran
template <size_t N>
static int32_t run_between_writes(
    sequencer_bar0& bar0,
    const uint32_t (&step)[N],
    const cpu_sequencer_context& context
) {
    const uint32_t before[] = {WRITE_REGISTER, BEFORE_REGISTER, WRITTEN_VALUE};
    const uint32_t after[] = {WRITE_REGISTER, AFTER_REGISTER, WRITTEN_VALUE};

    sequencer_payload payload;
    payload.add(before);
    payload.add(step);
    payload.add(after);

    return run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count());
}

// Whether the run refuses `step` once the write before it ran, without running the write after it
template <size_t N>
static bool refuses_after_earlier_steps(sequencer_bar0& bar0, const uint32_t (&step)[N]) {
    bar0.write(BEFORE_REGISTER, 0);
    bar0.write(AFTER_REGISTER, 0);

    cpu_sequencer_context context = sequencer_context(0, clock::now_ns() + SHORT_TIMEOUT_NS);
    bool refused = run_between_writes(bar0, step, context) == ERR_BAD_MESSAGE;
    bool ran_before = bar0.read(BEFORE_REGISTER) == WRITTEN_VALUE;
    bool ran_after = bar0.read(AFTER_REGISTER) != 0;

    return refused && ran_before && !ran_after;
}

TEST(nvidia_cpu_sequencer, function_number_is_nvidias) {
    EXPECT_EQ(FUNCTION_RUN_CPU_SEQUENCER, 0x1002u);
}

// A write and a store replace every bit, a modify clears the step's mask and sets its value whole, even the value's
// bits outside the mask, and each store lands in the save area word its index names
TEST(nvidia_cpu_sequencer, register_steps_write_modify_and_store) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(BEFORE_REGISTER, ~0u);
    bar0.write(STEP_REGISTER, 0x0F0F0F0F);
    bar0.write(AFTER_REGISTER, 0x89ABCDEF);

    const uint32_t write[] = {WRITE_REGISTER, BEFORE_REGISTER, WRITTEN_VALUE};
    const uint32_t modify[] = {MODIFY_REGISTER, STEP_REGISTER, 0x00FF00FF, 0x80120034};
    const uint32_t store_fourth[] = {STORE_REGISTER, STEP_REGISTER, 3};
    const uint32_t store_last[] = {STORE_REGISTER, AFTER_REGISTER, SAVE_AREA_WORDS - 1};
    sequencer_payload payload;
    payload.add(write);
    payload.add(modify);
    payload.add(store_fourth);
    payload.add(store_last);
    payload.words[FIRST_SAVE_AREA_WORD + SAVE_AREA_WORDS - 1] = ~0u;

    int32_t rc = run_cpu_sequencer(
        bar0.regs,
        sequencer_context(0, clock::now_ns() + SHORT_TIMEOUT_NS),
        payload.bytes(),
        payload.byte_count()
    );

    EXPECT_EQ(rc, OK);
    EXPECT_EQ(bar0.read(BEFORE_REGISTER), WRITTEN_VALUE);
    EXPECT_EQ(bar0.read(STEP_REGISTER), 0x8F120F34u);
    EXPECT_EQ(payload.words[FIRST_SAVE_AREA_WORD + 3], 0x8F120F34u);
    EXPECT_EQ(payload.words[FIRST_SAVE_AREA_WORD + SAVE_AREA_WORDS - 1], 0x89ABCDEFu);
    EXPECT_EQ(payload.words[FIRST_SAVE_AREA_WORD], 0u);
}

// A poll compares only its mask's bits with its value: every other bit set passes, and the mask's other bit set as
// well times out
TEST(nvidia_cpu_sequencer, poll_tests_only_its_mask) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(STEP_REGISTER, 0xFFFFFFFE);

    const uint32_t poll[] = {POLL_REGISTER, STEP_REGISTER, 0x80000001, 0x80000000, 0, 0};
    uint64_t start = clock::now_ns();
    int32_t rc = run_between_writes(bar0, poll, sequencer_context(0, start + LONG_TIMEOUT_NS));
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, OK);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(AFTER_REGISTER), WRITTEN_VALUE);

    const uint32_t timed_poll[] = {POLL_REGISTER, STEP_REGISTER, 0x80000001, 0x80000000, 1000, 0};
    bar0.write(STEP_REGISTER, 0xFFFFFFFF);
    rc = run_between_writes(bar0, timed_poll, sequencer_context(0, clock::now_ns() + LONG_TIMEOUT_NS));

    EXPECT_EQ(rc, ERR_TIMEOUT);
}

// A poll's timeout counts microseconds, as NVIDIA's driver passes it, and a poll that times out ends the run
TEST(nvidia_cpu_sequencer, poll_times_out_after_its_microseconds) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(STEP_REGISTER, 0x7FFFFFFF);

    const uint32_t poll[] = {POLL_REGISTER, STEP_REGISTER, 0x80000001, 0x80000000, 1000, 0x5};
    uint64_t start = clock::now_ns();
    int32_t rc = run_between_writes(bar0, poll, sequencer_context(0, start + LONG_TIMEOUT_NS));
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, ERR_TIMEOUT);
    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(BEFORE_REGISTER), WRITTEN_VALUE);
    EXPECT_EQ(bar0.read(AFTER_REGISTER), 0u);
}

// A poll whose own timeout runs past the deadline ends at the deadline, its microseconds counted past 32 bits of
// nanoseconds
TEST(nvidia_cpu_sequencer, poll_ends_at_the_deadline_before_its_own_timeout) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    const uint32_t poll[] = {POLL_REGISTER, STEP_REGISTER, 0x80000001, 0x80000000, 4294968, 0};
    uint64_t start = clock::now_ns();
    int32_t rc = run_between_writes(bar0, poll, sequencer_context(0, start + SHORT_TIMEOUT_NS));
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, ERR_TIMEOUT);
    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
}

// A poll whose timeout is 0, NVIDIA's default, ends at the deadline rather than at once
TEST(nvidia_cpu_sequencer, poll_of_timeout_0_ends_at_the_deadline) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    const uint32_t poll[] = {POLL_REGISTER, STEP_REGISTER, 0x80000001, 0x80000000, 0, 0};
    uint64_t start = clock::now_ns();
    int32_t rc = run_between_writes(bar0, poll, sequencer_context(0, start + SHORT_TIMEOUT_NS));
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, ERR_TIMEOUT);
    EXPECT_GE(elapsed, SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
}

// A delay waits its microseconds even past the deadline, a count not a multiple of 4 included, since its argument names
// no register
TEST(nvidia_cpu_sequencer, delay_waits_its_microseconds) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    const uint32_t delay[] = {DELAY, 2001};
    uint64_t start = clock::now_ns();
    int32_t rc = run_between_writes(bar0, delay, sequencer_context(0, start + SHORT_TIMEOUT_NS));
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, OK);
    EXPECT_GE(elapsed, 2 * SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(AFTER_REGISTER), WRITTEN_VALUE);
}

// Registers and fields the falcon steps use, restated here so the tests hold the steps to NVIDIA's layout
static constexpr uint32_t GSP_FALCON_MAILBOX0     = 0x110040;
static constexpr uint32_t GSP_FALCON_MAILBOX1     = 0x110044;
static constexpr uint32_t GSP_FALCON_OS           = 0x110080;
static constexpr uint32_t GSP_FALCON_RM           = 0x110084;
static constexpr uint32_t GSP_FALCON_HWCFG2       = 0x1100F4;
static constexpr uint32_t GSP_FALCON_CPUCTL       = 0x110100;
static constexpr uint32_t GSP_FALCON_DMACTL       = 0x11010C;
static constexpr uint32_t GSP_FALCON_CPUCTL_ALIAS = 0x110130;
static constexpr uint32_t GSP_FBIF_CTL            = 0x110624;
static constexpr uint32_t GSP_RISCV_CPUCTL        = 0x111388;
static constexpr uint32_t GSP_RISCV_BCR_CTRL      = 0x111668;
static constexpr uint32_t BOOT_STAGE_SCRATCH      = 0x1180F8;
static constexpr uint32_t SEC2_FALCON_CPUCTL      = 0x840100;

static constexpr uint32_t START_FALCON                   = 0x2;
static constexpr uint32_t FALCON_HALTED                  = 0x10;
static constexpr uint32_t STARTS_THROUGH_ALIAS           = 0x40;
static constexpr uint32_t ALLOW_PHYSICAL_WITHOUT_CONTEXT = 0x80;
static constexpr uint32_t RISCV_ACTIVE                   = 0x80;
static constexpr uint32_t CLEARING_MEMORIES              = 0x1000;
static constexpr uint32_t BOOT_STAGE_3_HANDED_OFF        = 0x4000000;
static constexpr uint32_t SELECT_RISCV_CORE              = 0x111;

// The falcon steps reset the GSP's falcon and let its DMA engine run with no address space bound, start it through its
// alias, wait for its halt and resume its firmware, the reset waiting out the ready timeout twice and the resume once
TEST(nvidia_cpu_sequencer, falcon_steps_reset_start_wait_for_and_resume_the_gsp) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    bar0.write(GSP_FBIF_CTL, ~ALLOW_PHYSICAL_WITHOUT_CONTEXT);
    bar0.write(GSP_FALCON_DMACTL, ~0u);
    bar0.write(GSP_FALCON_CPUCTL, STARTS_THROUGH_ALIAS | FALCON_HALTED);
    bar0.write(BOOT_STAGE_SCRATCH, BOOT_STAGE_3_HANDED_OFF);
    bar0.write(GSP_RISCV_CPUCTL, RISCV_ACTIVE);

    const uint32_t steps[] = {RESET_GSP, START_GSP, WAIT_FOR_GSP_HALT, RESUME_GSP};
    sequencer_payload payload;
    payload.add(steps);

    uint64_t start = clock::now_ns();
    cpu_sequencer_context context = sequencer_context(SHORT_TIMEOUT_NS, start + LONG_TIMEOUT_NS);
    int32_t rc = run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count());
    uint64_t elapsed = clock::now_ns() - start;

    EXPECT_EQ(rc, OK);
    EXPECT_GE(elapsed, 3 * SHORT_TIMEOUT_NS);
    EXPECT_LT(elapsed, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(bar0.read(GSP_FALCON_RM), context.boot0);
    EXPECT_EQ(bar0.read(GSP_FBIF_CTL), ~0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_DMACTL), 0u);
    EXPECT_EQ(bar0.read(GSP_FALCON_CPUCTL_ALIAS), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_RISCV_BCR_CTRL), SELECT_RISCV_CORE);
    EXPECT_EQ(bar0.read(GSP_FALCON_MAILBOX0), 0xC2D1E000u);
    EXPECT_EQ(bar0.read(GSP_FALCON_MAILBOX1), 0x5A4B3u);
    EXPECT_EQ(bar0.read(SEC2_FALCON_CPUCTL), START_FALCON);
    EXPECT_EQ(bar0.read(GSP_FALCON_OS), context.app_version);
}

// A failing falcon step ends the run with its error, each wait running out the time left until the deadline, and a
// reset that times out ends it before the DMA engine may run with no address space bound, where NVIDIA's cannot fail
TEST(nvidia_cpu_sequencer, falcon_steps_end_the_run_when_they_fail) {
    const uint32_t reset[] = {RESET_GSP};
    const uint32_t wait[] = {WAIT_FOR_GSP_HALT};
    const uint32_t resume[] = {RESUME_GSP};

    sequencer_bar0 reset_bar0;
    ASSERT_NOT_NULL(reset_bar0.bytes);
    reset_bar0.write(GSP_FALCON_HWCFG2, CLEARING_MEMORIES);
    uint64_t start = clock::now_ns();

    EXPECT_EQ(run_between_writes(reset_bar0, reset, sequencer_context(0, start + SHORT_TIMEOUT_NS)), ERR_TIMEOUT);
    EXPECT_GE(clock::now_ns() - start, SHORT_TIMEOUT_NS);
    EXPECT_LT(clock::now_ns() - start, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(reset_bar0.read(GSP_FBIF_CTL), 0u);
    EXPECT_EQ(reset_bar0.read(AFTER_REGISTER), 0u);

    sequencer_bar0 halt_bar0;
    ASSERT_NOT_NULL(halt_bar0.bytes);
    start = clock::now_ns();

    EXPECT_EQ(run_between_writes(halt_bar0, wait, sequencer_context(0, start + SHORT_TIMEOUT_NS)), ERR_TIMEOUT);
    EXPECT_GE(clock::now_ns() - start, SHORT_TIMEOUT_NS);
    EXPECT_LT(clock::now_ns() - start, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(halt_bar0.read(AFTER_REGISTER), 0u);

    sequencer_bar0 handoff_bar0;
    ASSERT_NOT_NULL(handoff_bar0.bytes);
    handoff_bar0.write(GSP_RISCV_CPUCTL, RISCV_ACTIVE);
    start = clock::now_ns();

    EXPECT_EQ(run_between_writes(handoff_bar0, resume, sequencer_context(0, start + SHORT_TIMEOUT_NS)), ERR_TIMEOUT);
    EXPECT_GE(clock::now_ns() - start, SHORT_TIMEOUT_NS);
    EXPECT_LT(clock::now_ns() - start, RUN_TIME_LIMIT_NS);
    EXPECT_EQ(handoff_bar0.read(AFTER_REGISTER), 0u);

    sequencer_bar0 riscv_bar0;
    ASSERT_NOT_NULL(riscv_bar0.bytes);
    riscv_bar0.write(BOOT_STAGE_SCRATCH, BOOT_STAGE_3_HANDED_OFF);
    start = clock::now_ns();

    EXPECT_EQ(run_between_writes(riscv_bar0, resume, sequencer_context(0, start + SHORT_TIMEOUT_NS)), ERR_BAD_STATE);
    EXPECT_EQ(riscv_bar0.read(AFTER_REGISTER), 0u);
}

// A step is refused when the run reaches it for an unknown opcode or a store index past the save area (even with
// low bits naming a valid one), an offset past BAR0 in each register step or a write's unaligned or wrapping offset
TEST(nvidia_cpu_sequencer, refused_steps_end_the_run) {
    const uint32_t unknown[] = {9};
    const uint32_t high_bit[] = {0x80000000, STEP_REGISTER, WRITTEN_VALUE};
    const uint32_t past_save_area[] = {STORE_REGISTER, STEP_REGISTER, SAVE_AREA_WORDS};
    const uint32_t high_index[] = {STORE_REGISTER, STEP_REGISTER, 0x10000007};
    const uint32_t unaligned[] = {WRITE_REGISTER, STEP_REGISTER + 2, WRITTEN_VALUE};
    const uint32_t write_past_bar0[] = {WRITE_REGISTER, sequencer_bar0::MAPPED_BYTES, WRITTEN_VALUE};
    const uint32_t modify_past_bar0[] = {MODIFY_REGISTER, sequencer_bar0::MAPPED_BYTES, 0, WRITTEN_VALUE};
    const uint32_t poll_past_bar0[] = {POLL_REGISTER, sequencer_bar0::MAPPED_BYTES, 0, 0, 0, 0};
    const uint32_t store_past_bar0[] = {STORE_REGISTER, sequencer_bar0::MAPPED_BYTES, 0};
    const uint32_t wrapping[] = {WRITE_REGISTER, 0xFFFFFFFC, WRITTEN_VALUE};
    const uint32_t last_word[] = {WRITE_REGISTER, sequencer_bar0::MAPPED_BYTES - 4, WRITTEN_VALUE};

    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    EXPECT_TRUE(refuses_after_earlier_steps(bar0, unknown));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, high_bit));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, past_save_area));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, high_index));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, unaligned));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, write_past_bar0));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, modify_past_bar0));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, poll_past_bar0));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, store_past_bar0));
    EXPECT_TRUE(refuses_after_earlier_steps(bar0, wrapping));
    EXPECT_EQ(bar0.read(STEP_REGISTER), 0u);

    uint64_t deadline = clock::now_ns() + SHORT_TIMEOUT_NS;
    EXPECT_EQ(run_between_writes(bar0, last_word, sequencer_context(0, deadline)), OK);
    EXPECT_EQ(bar0.read(sequencer_bar0::MAPPED_BYTES - 4), WRITTEN_VALUE);
}

// A payload too short for its header or its steps, or whose steps take every word of its buffer or more, is refused
// before any step runs, as is a first step running past the steps, while each limit passes and no step past them runs
TEST(nvidia_cpu_sequencer, malformed_payloads_are_refused) {
    sequencer_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    cpu_sequencer_context context = sequencer_context(0, clock::now_ns() + SHORT_TIMEOUT_NS);
    const uint32_t write[] = {WRITE_REGISTER, BEFORE_REGISTER, WRITTEN_VALUE};
    const uint32_t after[] = {WRITE_REGISTER, AFTER_REGISTER, WRITTEN_VALUE};

    sequencer_payload empty;
    empty.words[0] = 1;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, empty.bytes(), SEQUENCER_HEADER_BYTES - 1), ERR_BAD_MESSAGE);
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, empty.bytes(), SEQUENCER_HEADER_BYTES), OK);

    sequencer_payload payload;
    payload.add(write);
    payload.words[0] = payload.step_words - 1;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), ERR_BAD_MESSAGE);

    payload.words[0] = payload.step_words;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), ERR_BAD_MESSAGE);

    payload.words[0] = payload.step_words + 1;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count() - 1), ERR_BAD_MESSAGE);

    payload.words[1] = payload.step_words - 1;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), ERR_BAD_MESSAGE);
    EXPECT_EQ(bar0.read(BEFORE_REGISTER), 0u);

    payload.words[1] = payload.step_words;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), OK);
    EXPECT_EQ(bar0.read(BEFORE_REGISTER), WRITTEN_VALUE);

    payload.words[0] = 0x10000 + payload.step_words;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), OK);

    payload.words[1] = 0x10000 + payload.step_words;
    payload.words[0] = payload.words[1] + 1;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, payload.bytes(), payload.byte_count()), ERR_BAD_MESSAGE);

    sequencer_payload counted;
    counted.add(write);
    uint32_t write_words = counted.step_words;
    counted.add(after);
    counted.words[1] = write_words;
    EXPECT_EQ(run_cpu_sequencer(bar0.regs, context, counted.bytes(), counted.byte_count()), OK);
    EXPECT_EQ(bar0.read(AFTER_REGISTER), 0u);
}
