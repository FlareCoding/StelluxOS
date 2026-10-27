// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_LATCH_RULES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_LATCH_RULES_H

#include "common/types.h"
#include "drivers/graphics/edid.h"

/*
 * A head is the part of the display engine that scans a buffer out to a
 * monitor, and a vertical blank is the lines between one frame's last visible
 * line and the next frame's first. The driver writes each present as an
 * update into the output's window channel, the queue of commands that sets up
 * the image the head scans out. A head takes a present's buffer at LOADV, the
 * moment in a vertical blank when the state the present's update wrote
 * becomes the state the head scans. That is the present's latch, after which
 * the head never reads the buffer it showed before. The window notifier is a
 * page of completion notifiers, its slots, that the window channel writes. A
 * present's slot, cleared to not begun before the present's update is
 * written, reads begun from LOADV and finished once the update completes.
 * Presents take the slots in turn, so a status written late for one present
 * is never written into the slot the next present reads.
 *
 * Three rules decide what becomes of an output's queued present, each under
 * the output's spinlock. The latch step runs in the interrupt handler for
 * each vertical blank it services. The interrupt comes at LAST_DATA (the end
 * of a frame's visible pixels), which can come before LOADV, so the service
 * can read the slot before it reads begun. Raster lines are numbered from
 * line 0 at the start of vertical sync. The latch check runs from a timer set
 * for the service's time plus the time from LAST_DATA to one line past the
 * start of raster line 1, and reads the slot again. It reports only while no
 * later vertical blank was serviced and less than half a frame period has
 * passed since the service, so a check never reports a latch from the next
 * LOADV under the check's frame while the handler services each vertical
 * blank within half a period of its LAST_DATA. A latch that the service reads
 * too early and no latch check reports, since the mode has none or its check
 * ran too late, is reported at the next vertical blank's service, a frame
 * late. The deadline runs from a timer when neither found the latch by the
 * present's deadline.
 *
 * A rule that finds the latch ends the present with a latch report (a frame
 * number and a time). A report from the latch step or the latch check numbers
 * its latch the larger of its frame and one past the output's last report.
 * The deadline's report cannot tell which frame its latch happened in, so it
 * numbers the latch one past the last report. The caller must record each
 * report's frame as the output's last report, so the numbers a client reads
 * always increase.
 *
 * These functions only decide. The caller must read the output's flip state
 * into `latch_inputs` and apply the decision to the present, the flip state
 * and the timers under one hold of the output's spinlock, so no other rule
 * ends the present in between. The functions keep no state, touch no hardware
 * and never block.
 */

namespace drivers::nvidia {

// The window notifier's slots, which presents take in turn, NVIDIA's `NVKMS_HEAD_SURFACE_MAX_NOTIFIERS_PER_HEAD`
constexpr uint32_t NOTIFIER_SLOTS = 4;

// A mode's times the rules count with, computed once when its output lights
struct mode_times {
    uint64_t period_ns;            // one frame, every line of the raster at the pixel clock, rounded down
    uint64_t latch_check_delay_ns; // from LAST_DATA to one line past the start of raster line 1, rounded up
    bool     has_latch_check;      // the delay is below half the period, the check's bound, so the step schedules it
};

// What the rules read of an output's flip state
struct latch_inputs {
    bool     present_queued;      // a present the hardware was given has not ended, latched or not
    bool     slot_written;        // the queued present's slot reads begun or finished
    uint64_t frame;               // the output's frame count, which each serviced vertical blank advances
    uint64_t vertical_blank_ns;   // when the handler serviced the vertical blank `frame` counts, on the monotonic clock
    uint64_t check_frame;         // the frame the latch check is for
    uint64_t last_reported_frame; // the frame of the output's last latch report, 0 before the first
    uint64_t deadline_ns;         // the queued present's deadline, on the monotonic clock
};

// The frame and time a present that latched ends with. The caller must record `frame` as `last_reported_frame`
struct latch_report {
    uint64_t frame;
    uint64_t timestamp_ns;
};

// What the latch step does with the queued present
enum class latch_step_action : uint8_t {
    none,           // no present is queued, or its slot is unwritten in a mode without a latch check
    report,         // end the present latched with the report and cancel both timers
    schedule_check, // record `inputs.frame` as the check's frame and schedule the latch check timer for `check_ns`
};

// The latch step's action, with the report or the check's time it needs
struct latch_step_decision {
    latch_step_action action;
    latch_report      report;   // set for `report`
    uint64_t          check_ns; // set for `schedule_check`, on the monotonic clock
};

// What the deadline timer does with the queued present
enum class deadline_action : uint8_t {
    none,   // no present is queued, or its deadline is still ahead, so the timer ran for an earlier present
    report, // end the present latched with the report, an estimate, and log that the vertical blank interrupt stopped
    fail,   // end the present failed, mark the display failed and wake the driver's task
};

// The deadline timer's action, with the report it needs
struct deadline_decision {
    deadline_action action;
    latch_report    report; // set for `report`
};

/**
 * @brief Computes a mode's period and latch check delay from its timing. The
 * delay is the vertical front porch and two lines more: raster line 0, and
 * one line of margin since NVIDIA leaves unstated which line LAST_DATA falls
 * on.
 * @param timing The mode's timing, whose pixels per line and lines per frame
 * must each be at most 4194304, as every timing `edid::parse` gives is, so
 * the times' products fit in 64 bits.
 * @param times Receives the mode's times, written only on true.
 * @return true unless the timing's pixel clock is 0.
 */
bool compute_mode_times(const edid::timing& timing, mode_times& times);

/**
 * @brief Decides what the handler does with an output's queued present at a
 * vertical blank it just serviced, which `inputs.frame` counts.
 * @param inputs The output's flip state after the service.
 * @param times The output's mode times.
 * @return `none` when no present is queued, otherwise `report` when the slot
 * is written, numbered the larger of the frame count and one past the last
 * report, at the service's time, otherwise `schedule_check` for the service's
 * time plus the latch check delay, or `none` when the mode has no latch
 * check.
 */
latch_step_decision decide_latch_step(const latch_inputs& inputs, const mode_times& times);

/**
 * @brief Decides whether the latch check timer's callback ends the queued
 * present latched, which it does only when a present is still queued, the
 * frame count still equals the check's frame, `now_ns` is less than half the
 * period, rounded down, after the service of that vertical blank, and the
 * slot is written.
 * @param inputs The output's flip state when the check runs.
 * @param times The output's mode times.
 * @param now_ns The monotonic clock, which the caller must read after taking
 * the output's spinlock, so a wait for the lock counts toward the half
 * period.
 * @param report Receives the latch report, numbered the larger of the check's
 * frame and one past the last report, at the service's time, written only on
 * true.
 * @return true when the check ends the present latched with `report` and
 * cancels the deadline timer.
 */
bool decide_latch_check(const latch_inputs& inputs, const mode_times& times, uint64_t now_ns, latch_report& report);

/**
 * @brief Decides what the deadline timer's callback does with the queued
 * present.
 * @param inputs The output's flip state when the callback runs.
 * @param now_ns The monotonic clock, which the caller must read after taking
 * the output's spinlock, so the report's time never precedes a service that
 * found the slot unwritten.
 * @return `none` when no present is queued or `now_ns` is before its
 * deadline, otherwise `report` when the slot is written, numbered one past
 * the last report, at `now_ns`, or `fail` when it is not.
 */
deadline_decision decide_deadline(const latch_inputs& inputs, uint64_t now_ns);

/**
 * @brief Returns the slot an output's next present takes.
 * @param slot The slot the output's last present took.
 * @return The slot after `slot`, wrapping to 0 after the last, always below
 * `NOTIFIER_SLOTS`.
 */
uint32_t next_notifier_slot(uint32_t slot);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_LATCH_RULES_H
