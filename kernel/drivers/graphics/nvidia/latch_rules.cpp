// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/latch_rules.h"
#include "drivers/graphics/nvidia/nvidia.h"

namespace drivers::nvidia {

// In NVIDIA's raster for a mode that is not interlaced, `rasterSize.y - rasterBlankStart.y` lines are the front
// porch and one more, so the delay's `rasterSize.y - rasterBlankStart.y + 1` lines are the front porch and two more
static constexpr uint64_t LATCH_CHECK_LINES_PAST_FRONT_PORCH = 2;

static uint64_t number_latch(uint64_t frame, uint64_t last_reported_frame) {
    uint64_t next_frame = last_reported_frame + 1;
    return frame > next_frame ? frame : next_frame;
}

bool compute_mode_times(const edid::timing& timing, mode_times& times) {
    // Both times divide by the pixel clock
    if (timing.pixel_clock_khz == 0) {
        return false;
    }

    uint64_t line_pixels = timing.h_total();
    uint64_t frame_lines = timing.v_total();
    uint64_t delay_lines = timing.v_front_porch + LATCH_CHECK_LINES_PAST_FRONT_PORCH;
    uint64_t delay_ns = line_pixels * delay_lines * NS_PER_MS;

    times.period_ns = line_pixels * frame_lines * NS_PER_MS / timing.pixel_clock_khz;
    times.latch_check_delay_ns = (delay_ns + timing.pixel_clock_khz - 1) / timing.pixel_clock_khz;
    times.has_latch_check = 2 * delay_lines < frame_lines;

    return true;
}

latch_step_decision decide_latch_step(const latch_inputs& inputs, const mode_times& times) {
    latch_step_decision decision = {latch_step_action::none, {0, 0}, 0};
    if (!inputs.present_queued) {
        return decision;
    }

    if (inputs.slot_written) {
        decision.action = latch_step_action::report;
        decision.report = {number_latch(inputs.frame, inputs.last_reported_frame), inputs.vertical_blank_ns};
        return decision;
    }

    if (times.has_latch_check) {
        decision.action = latch_step_action::schedule_check;
        decision.check_ns = inputs.vertical_blank_ns + times.latch_check_delay_ns;
    }

    return decision;
}

bool decide_latch_check(const latch_inputs& inputs, const mode_times& times, uint64_t now_ns, latch_report& report) {
    bool no_later_service = inputs.frame == inputs.check_frame;
    bool within_half_period = now_ns < inputs.vertical_blank_ns + times.period_ns / 2;
    if (!inputs.present_queued || !no_later_service || !within_half_period || !inputs.slot_written) {
        return false;
    }

    report = {number_latch(inputs.check_frame, inputs.last_reported_frame), inputs.vertical_blank_ns};

    return true;
}

deadline_decision decide_deadline(const latch_inputs& inputs, uint64_t now_ns) {
    deadline_decision decision = {deadline_action::none, {0, 0}};
    if (!inputs.present_queued || now_ns < inputs.deadline_ns) {
        return decision;
    }

    if (inputs.slot_written) {
        decision.action = deadline_action::report;
        decision.report = {inputs.last_reported_frame + 1, now_ns};
        return decision;
    }

    decision.action = deadline_action::fail;

    return decision;
}

uint32_t next_notifier_slot(uint32_t slot) {
    return (slot + 1) % NOTIFIER_SLOTS;
}

} // namespace drivers::nvidia
