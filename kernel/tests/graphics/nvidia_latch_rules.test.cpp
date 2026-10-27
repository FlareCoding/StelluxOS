// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/latch_rules.h"

TEST_SUITE(nvidia_latch_rules);

using drivers::nvidia::compute_mode_times;
using drivers::nvidia::deadline_action;
using drivers::nvidia::deadline_decision;
using drivers::nvidia::decide_deadline;
using drivers::nvidia::decide_latch_check;
using drivers::nvidia::decide_latch_step;
using drivers::nvidia::latch_inputs;
using drivers::nvidia::latch_report;
using drivers::nvidia::latch_step_action;
using drivers::nvidia::latch_step_decision;
using drivers::nvidia::mode_times;
using drivers::nvidia::next_notifier_slot;
using drivers::nvidia::NOTIFIER_SLOTS;

// CTA-861's 1080p at 60 Hz, 2200 by 1125 at 148.5 MHz
static constexpr edid::timing FULL_HD_60 = {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true};

// `FULL_HD_60`'s times, as `compute_mode_times` gives them
static constexpr mode_times FULL_HD_60_TIMES = {16666666, 88889, true};

// A frame count past 32 bits (so cutting a frame to 32 bits fails the tests), the time the handler serviced that
// frame's vertical blank, the latch check's time, the queued present's deadline and half the period
static constexpr uint64_t FRAME          = (1ULL << 32) + 41;
static constexpr uint64_t SERVICE_NS     = 5000000000;
static constexpr uint64_t CHECK_NS       = SERVICE_NS + FULL_HD_60_TIMES.latch_check_delay_ns;
static constexpr uint64_t DEADLINE_NS    = 5100000000;
static constexpr uint64_t HALF_PERIOD_NS = FULL_HD_60_TIMES.period_ns / 2;

// A value no result the tests expect holds, so a refusal shows it left its out parameter unwritten
static constexpr uint64_t SENTINEL = 0xDEADBEEF;

// A present queued before `FRAME`'s vertical blank, its slot unwritten when the handler serviced that vertical blank
// at `SERVICE_NS`, the latch check recorded for `FRAME`, the last report at `FRAME - 1` and the deadline `DEADLINE_NS`
static latch_inputs queued_present() {
    return {true, false, FRAME, SERVICE_NS, FRAME, FRAME - 1, DEADLINE_NS};
}

// The handler's service of the next vertical blank, a period after the last
static void service_next_vertical_blank(latch_inputs& inputs) {
    inputs.frame++;
    inputs.vertical_blank_ns += FULL_HD_60_TIMES.period_ns;
}

// The caller's part of ending the queued present with a report, its slot left as the hardware wrote it
static void end_queued_present(latch_inputs& inputs, const latch_report& report) {
    inputs.present_queued = false;
    inputs.last_reported_frame = report.frame;
}

// The next present, given with its slot cleared to not begun, its deadline left at the last present's
static void queue_next_present(latch_inputs& inputs) {
    inputs.present_queued = true;
    inputs.slot_written = false;
}

// The latch check at `now_ns` ends nothing and leaves its report as it found it
static void expect_check_ends_nothing(const latch_inputs& inputs, uint64_t now_ns) {
    latch_report report = {SENTINEL, SENTINEL};
    EXPECT_FALSE(decide_latch_check(inputs, FULL_HD_60_TIMES, now_ns, report));
    EXPECT_EQ(report.frame, SENTINEL);
    EXPECT_EQ(report.timestamp_ns, SENTINEL);
}

TEST(nvidia_latch_rules, mode_times_of_full_hd_60) {
    mode_times times = {};
    ASSERT_TRUE(compute_mode_times(FULL_HD_60, times));

    // 2200 * 1125 pixels at 148.5 MHz is 16666666.7 ns, and 6 lines of 2200 pixels are 88888.9 ns
    EXPECT_EQ(times.period_ns, FULL_HD_60_TIMES.period_ns);
    EXPECT_EQ(times.latch_check_delay_ns, FULL_HD_60_TIMES.latch_check_delay_ns);
    EXPECT_TRUE(times.has_latch_check);
}

// The IOC 25G2S monitor's 2560 by 1440 timing at 240 Hz as `edid::parse` reads its EDID, 2720 by 1485 at 969.41 MHz
static constexpr edid::timing QHD_240 = {969410, 2560, 48, 32, 80, 1440, 3, 5, 37, true, false};

TEST(nvidia_latch_rules, mode_times_of_qhd_240) {
    mode_times times = {};
    ASSERT_TRUE(compute_mode_times(QHD_240, times));

    // 2720 * 1485 pixels at 969.41 MHz is 4166658.1 ns, and 5 lines of 2720 pixels are 14029.2 ns
    EXPECT_EQ(times.period_ns, 4166658u);
    EXPECT_EQ(times.latch_check_delay_ns, 14030u);
    EXPECT_TRUE(times.has_latch_check);
}

// The EDID parser's largest timing, from a DisplayID descriptor with its clock and every count but the sync widths at
// its largest, each sync width the blanking minus the front porch
static constexpr edid::timing LARGEST_PARSED_TIMING = {
    167772160, 65536, 32768, 32768, 0, 65536, 32768, 32768, 0, true, true
};

TEST(nvidia_latch_rules, mode_times_of_largest_parsed_timing) {
    mode_times times = {};
    ASSERT_TRUE(compute_mode_times(LARGEST_PARSED_TIMING, times));

    // 131072 * 131072 pixels at 167772.16 MHz is 102400000 ns, and 32770 lines of 131072 pixels are 25601562.5 ns
    EXPECT_EQ(times.period_ns, 102400000u);
    EXPECT_EQ(times.latch_check_delay_ns, 25601563u);
    EXPECT_TRUE(times.has_latch_check);
}

TEST(nvidia_latch_rules, mode_times_with_no_front_porch) {
    // An 18-byte descriptor's front porch may be 0, the smallest the EDID parser takes
    edid::timing timing = FULL_HD_60;
    timing.v_front_porch = 0;
    timing.v_back_porch = 40;

    mode_times times = {};
    ASSERT_TRUE(compute_mode_times(timing, times));

    // 2 lines of 2200 pixels at 148.5 MHz are 29629.6 ns
    EXPECT_EQ(times.period_ns, FULL_HD_60_TIMES.period_ns);
    EXPECT_EQ(times.latch_check_delay_ns, 29630u);
    EXPECT_TRUE(times.has_latch_check);
}

TEST(nvidia_latch_rules, mode_times_with_delay_reaching_half_period) {
    // 8 front porch lines make a delay of 10, half of the 20 lines the raster has
    edid::timing timing = {1000, 16, 2, 2, 2, 8, 8, 1, 3, true, true};

    mode_times times = {};
    ASSERT_TRUE(compute_mode_times(timing, times));
    EXPECT_FALSE(times.has_latch_check);

    timing.v_back_porch = 4;
    ASSERT_TRUE(compute_mode_times(timing, times));
    EXPECT_TRUE(times.has_latch_check);
}

TEST(nvidia_latch_rules, mode_times_refuse_zero_pixel_clock) {
    edid::timing timing = FULL_HD_60;
    timing.pixel_clock_khz = 0;

    mode_times times = {SENTINEL, SENTINEL, false};
    EXPECT_FALSE(compute_mode_times(timing, times));
    EXPECT_EQ(times.period_ns, SENTINEL);
    EXPECT_EQ(times.latch_check_delay_ns, SENTINEL);
    EXPECT_FALSE(times.has_latch_check);
}

TEST(nvidia_latch_rules, latch_step_reports_slot_written_at_service) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;

    latch_step_decision decision = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(decision.action, latch_step_action::report);
    EXPECT_EQ(decision.report.frame, FRAME);
    EXPECT_EQ(decision.report.timestamp_ns, SERVICE_NS);
}

TEST(nvidia_latch_rules, latch_step_schedules_check_for_unwritten_slot) {
    latch_step_decision decision = decide_latch_step(queued_present(), FULL_HD_60_TIMES);
    ASSERT_EQ(decision.action, latch_step_action::schedule_check);
    EXPECT_EQ(decision.check_ns, CHECK_NS);
}

TEST(nvidia_latch_rules, latch_step_without_latch_check_leaves_present_queued) {
    mode_times times = FULL_HD_60_TIMES;
    times.has_latch_check = false;

    EXPECT_EQ(decide_latch_step(queued_present(), times).action, latch_step_action::none);
}

TEST(nvidia_latch_rules, latch_step_without_latch_check_reports_written_slot) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    mode_times times = FULL_HD_60_TIMES;
    times.has_latch_check = false;

    latch_step_decision decision = decide_latch_step(inputs, times);
    ASSERT_EQ(decision.action, latch_step_action::report);
    EXPECT_EQ(decision.report.frame, FRAME);
    EXPECT_EQ(decision.report.timestamp_ns, SERVICE_NS);
}

TEST(nvidia_latch_rules, latch_check_reports_slot_written_at_check) {
    // The last report two frames back, so the check's frame, not one past the last report, numbers the latch
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    inputs.last_reported_frame = FRAME - 2;

    latch_report report = {};
    ASSERT_TRUE(decide_latch_check(inputs, FULL_HD_60_TIMES, CHECK_NS, report));
    EXPECT_EQ(report.frame, FRAME);
    EXPECT_EQ(report.timestamp_ns, SERVICE_NS);
}

TEST(nvidia_latch_rules, latch_check_numbers_past_last_report) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    inputs.last_reported_frame = FRAME + 1;

    latch_report report = {};
    ASSERT_TRUE(decide_latch_check(inputs, FULL_HD_60_TIMES, CHECK_NS, report));
    EXPECT_EQ(report.frame, FRAME + 2);
}

TEST(nvidia_latch_rules, latch_check_after_later_service_does_nothing) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    service_next_vertical_blank(inputs);

    expect_check_ends_nothing(inputs, CHECK_NS + FULL_HD_60_TIMES.period_ns);
}

TEST(nvidia_latch_rules, latch_check_after_late_handler_numbers_next_loadv_early) {
    // A service more than half a period late lets the check read a slot the next LOADV wrote, under this frame's number
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;

    latch_report report = {};
    ASSERT_TRUE(decide_latch_check(inputs, FULL_HD_60_TIMES, SERVICE_NS + HALF_PERIOD_NS - 1, report));
    EXPECT_EQ(report.frame, FRAME);
}

TEST(nvidia_latch_rules, latch_check_half_period_rounds_down) {
    // An odd period, so a half rounded up would let the check at this time report
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    mode_times times = FULL_HD_60_TIMES;
    times.period_ns += 1;

    latch_report report = {SENTINEL, SENTINEL};
    EXPECT_FALSE(decide_latch_check(inputs, times, SERVICE_NS + times.period_ns / 2, report));
    EXPECT_EQ(report.frame, SENTINEL);
    EXPECT_EQ(report.timestamp_ns, SENTINEL);
}

TEST(nvidia_latch_rules, present_unwritten_at_service_and_check_reported_at_next_vertical_blank) {
    // A present given after the vertical blank began, or between its LOADV and a late service, latches at the next
    latch_inputs inputs = queued_present();
    ASSERT_EQ(decide_latch_step(inputs, FULL_HD_60_TIMES).action, latch_step_action::schedule_check);
    expect_check_ends_nothing(inputs, CHECK_NS);

    service_next_vertical_blank(inputs);
    inputs.slot_written = true;

    latch_step_decision decision = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(decision.action, latch_step_action::report);
    EXPECT_EQ(decision.report.frame, FRAME + 1);
    EXPECT_EQ(decision.report.timestamp_ns, SERVICE_NS + FULL_HD_60_TIMES.period_ns);
}

TEST(nvidia_latch_rules, deadline_before_its_time_belongs_to_earlier_present) {
    // The timer ran for an earlier present, so it ends nothing, whether or not the queued present's slot is written
    latch_inputs inputs = queued_present();
    EXPECT_EQ(decide_deadline(inputs, DEADLINE_NS - 1).action, deadline_action::none);

    inputs.slot_written = true;
    EXPECT_EQ(decide_deadline(inputs, DEADLINE_NS - 1).action, deadline_action::none);
}

TEST(nvidia_latch_rules, deadline_reports_written_slot_past_last_report) {
    // Vertical blanks serviced since the last report leave the deadline's number one past that report, and a callback
    // run after the deadline reports its own clock
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;
    inputs.frame = FRAME + 2;
    uint64_t now_ns = DEADLINE_NS + 1000;

    deadline_decision decision = decide_deadline(inputs, now_ns);
    ASSERT_EQ(decision.action, deadline_action::report);
    EXPECT_EQ(decision.report.frame, FRAME);
    EXPECT_EQ(decision.report.timestamp_ns, now_ns);
}

TEST(nvidia_latch_rules, deadline_fails_unwritten_slot) {
    // A card gone stops the vertical blanks and never writes the slot
    EXPECT_EQ(decide_deadline(queued_present(), DEADLINE_NS).action, deadline_action::fail);
}

TEST(nvidia_latch_rules, unwritten_slot_without_queued_present_ends_nothing) {
    // With no present queued, an unwritten slot ends nothing either
    latch_inputs inputs = queued_present();
    inputs.present_queued = false;

    EXPECT_EQ(decide_latch_step(inputs, FULL_HD_60_TIMES).action, latch_step_action::none);
    expect_check_ends_nothing(inputs, CHECK_NS);
    EXPECT_EQ(decide_deadline(inputs, DEADLINE_NS).action, deadline_action::none);
}

TEST(nvidia_latch_rules, latch_ends_present_before_deadline) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;

    latch_step_decision decision = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(decision.action, latch_step_action::report);
    end_queued_present(inputs, decision.report);
    EXPECT_EQ(decide_deadline(inputs, DEADLINE_NS).action, deadline_action::none);
}

TEST(nvidia_latch_rules, deadline_ends_present_before_latch) {
    latch_inputs inputs = queued_present();
    inputs.slot_written = true;

    deadline_decision decision = decide_deadline(inputs, DEADLINE_NS);
    ASSERT_EQ(decision.action, deadline_action::report);
    end_queued_present(inputs, decision.report);
    EXPECT_EQ(decide_latch_step(inputs, FULL_HD_60_TIMES).action, latch_step_action::none);
    expect_check_ends_nothing(inputs, CHECK_NS);
}

TEST(nvidia_latch_rules, serviced_latch_after_two_deadline_reports_numbers_past_them) {
    // The vertical blank interrupt stopped, so the frame count stays at `FRAME` through both deadlines
    latch_inputs inputs = queued_present();
    for (uint64_t deadline_reports = 0; deadline_reports < 2; deadline_reports++) {
        inputs.slot_written = true;

        deadline_decision decision = decide_deadline(inputs, DEADLINE_NS);
        ASSERT_EQ(decision.action, deadline_action::report);
        EXPECT_EQ(decision.report.frame, FRAME + deadline_reports);
        end_queued_present(inputs, decision.report);
        queue_next_present(inputs);
    }

    service_next_vertical_blank(inputs);
    inputs.slot_written = true;

    latch_step_decision decision = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(decision.action, latch_step_action::report);
    EXPECT_EQ(decision.report.frame, FRAME + 2);
}

TEST(nvidia_latch_rules, late_report_followed_by_next_present_numbers_increase) {
    // The service reads the slot before the window channel writes it, and a late worker runs the check half a period
    // after the service, too late to report
    latch_inputs inputs = queued_present();
    ASSERT_EQ(decide_latch_step(inputs, FULL_HD_60_TIMES).action, latch_step_action::schedule_check);
    inputs.slot_written = true;
    expect_check_ends_nothing(inputs, SERVICE_NS + HALF_PERIOD_NS);

    // So the next vertical blank's latch step reports the present a frame late
    service_next_vertical_blank(inputs);
    latch_step_decision late = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(late.action, latch_step_action::report);
    EXPECT_EQ(late.report.frame, FRAME + 1);
    end_queued_present(inputs, late.report);

    // The next present, given at once, is taken at the LOADV after that service
    queue_next_present(inputs);
    service_next_vertical_blank(inputs);
    inputs.slot_written = true;

    latch_step_decision next = decide_latch_step(inputs, FULL_HD_60_TIMES);
    ASSERT_EQ(next.action, latch_step_action::report);
    EXPECT_EQ(next.report.frame, FRAME + 2);
}

// NVIDIA's `NVKMS_HEAD_SURFACE_MAX_NOTIFIERS_PER_HEAD`, the notifier slots its driver takes in turn for each head
static constexpr uint32_t NVIDIA_NOTIFIERS_PER_HEAD = 4;

TEST(nvidia_latch_rules, slots_taken_in_turn_wrap_after_four) {
    EXPECT_EQ(NOTIFIER_SLOTS, NVIDIA_NOTIFIERS_PER_HEAD);
    EXPECT_EQ(next_notifier_slot(0), 1u);
    EXPECT_EQ(next_notifier_slot(1), 2u);
    EXPECT_EQ(next_notifier_slot(2), 3u);
    EXPECT_EQ(next_notifier_slot(3), 0u);
}
