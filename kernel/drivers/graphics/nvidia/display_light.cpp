// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// NVIDIA's NV_EVO_SCALE_FACTOR_1X, a scale factor of one in 1024ths, and NVEvoScalerTapsToNum of NV_EVO_SCALER_2TAPS,
// the fewest taps NVIDIA's display classes give a scaler, which a scaler that does not scale takes
constexpr uint32_t SCALE_FACTOR_ONE = 1024;
constexpr uint8_t  UNSCALED_TAPS    = 2;

// The fewest lines the sync and back porch may take together, as each needs at least one, below which a mode is refused
// without asking the bandwidth check, as NVIDIA's modesetting module refuses a mode of 1 line but asks about one of 0
constexpr uint32_t LEADING_IDLE_LINES_MIN = 2;

// NVIDIA's cursorSize32p for a 256-pixel cursor, in units of 32 pixels, the largest size, which NVIDIA's modesetting
// module assumes
constexpr uint8_t CURSOR_WIDTH_32 = 256 / 32;

// The pixels a window fetches a line beyond its width when it does not scale, the worst case NVIDIA's modesetting
// module takes on every chip, (((width + 14) * 1024 + 1023) >> 10) + 8
constexpr uint32_t PIXELS_FETCHED_BEYOND_WIDTH = 22;

// The output tried after `tried` others: `first_output` first, then the others in ascending order
static uint8_t output_in_order(uint8_t tried, uint8_t first_output) {
    if (tried == 0) {
        return first_output;
    }

    return tried <= first_output ? tried - 1 : tried;
}

light_target find_light_target(const display_heads& heads, const active_heads& active, const display_outputs& outputs) {
    light_target target = {};
    if (find_boot_head(active, target.head)) {
        find_shown_output(active, target.head, outputs, target.first_output);
    }

    // The boot head and head 0 are below `heads.count`, and `display_heads` promises each such head a window
    target.window = static_cast<uint8_t>(__builtin_ctz(windows_of_head(heads, target.head)));

    return target;
}

head_raster raster_of(const edid::timing& timing) {
    uint32_t h_total = timing.h_total();
    uint32_t v_total = timing.v_total();
    uint32_t h_sync_start = timing.width + timing.h_front_porch;
    uint32_t v_sync_start = timing.height + timing.v_front_porch;

    return {
        .width = h_total,
        .height = v_total,
        .blank_start_x = timing.width + (h_total - h_sync_start) - 1,
        .blank_start_y = timing.height + (v_total - v_sync_start) - 1,
        .blank_end_x = (h_total - h_sync_start) - 1,
        .blank_end_y = (v_total - v_sync_start) - 1,
    };
}

// Fills `params` to ask whether `head` can show `timing` with `window` scanning it out, and gives whether its sync and
// back porch take enough lines for a request
static bool fill_request(const edid::timing& timing, uint8_t head, uint8_t window, rm::mode_possible_params& params) {
    head_raster raster = raster_of(timing);
    uint32_t leading_lines = raster.blank_end_y + 1;
    if (leading_lines < LEADING_IDLE_LINES_MIN) {
        return false;
    }

    string::memset(&params, 0, sizeof(params));
    params.head_count = 1;
    params.window_count = 1;

    rm::mode_possible_head& asked_head = params.heads[0];
    asked_head.head = head;
    asked_head.pixel_clock_khz = timing.pixel_clock_khz;
    asked_head.raster_width = raster.width;
    asked_head.raster_height = raster.height;

    asked_head.blank_start_x = raster.blank_start_x;
    asked_head.blank_start_y = raster.blank_start_y;
    asked_head.blank_end_x = raster.blank_end_x;
    asked_head.blank_end_y = raster.blank_end_y;

    asked_head.master_lock_mode = rm::LOCK_MODE_NONE;
    asked_head.master_lock_pin = rm::LOCK_PIN_NONE;
    asked_head.slave_lock_mode = rm::LOCK_MODE_NONE;
    asked_head.slave_lock_pin = rm::LOCK_PIN_NONE;

    asked_head.max_downscale_x = SCALE_FACTOR_ONE;
    asked_head.max_downscale_y = SCALE_FACTOR_ONE;
    asked_head.output_scaler_taps = UNSCALED_TAPS;

    asked_head.leading_idle_lines = static_cast<uint16_t>(leading_lines);
    asked_head.trailing_idle_lines = static_cast<uint16_t>(raster.height - (leading_lines + timing.height));
    asked_head.lut = rm::LUT_USAGE_1025;
    asked_head.cursor_width_32 = CURSOR_WIDTH_32;

    rm::mode_possible_window& asked_window = params.windows[0];
    asked_window.window = window;
    asked_window.head = head;
    asked_window.formats = rm::FORMAT_RGB_PACKED_4_BPP;
    asked_window.max_pixels_fetched = timing.width + PIXELS_FETCHED_BEYOND_WIDTH;

    asked_window.max_downscale_x = SCALE_FACTOR_ONE;
    asked_window.max_downscale_y = SCALE_FACTOR_ONE;
    asked_window.input_scaler_taps = UNSCALED_TAPS;
    asked_window.lut = rm::LUT_USAGE_1025;
    asked_window.tmo_lut = rm::LUT_USAGE_1025;

    return true;
}

// Asks the bandwidth check whether the target's head can show the output's mode `rank` with its window, logging a
// refusal
static int32_t check_mode(
    firmware& fw,
    rm::handle bandwidth_check,
    uint8_t output,
    const output_modes& modes,
    uint8_t rank,
    const light_target& target,
    rm::mode_possible_params& params,
    bool& is_possible
) {
    const edid::timing& timing = modes.timings[rank];
    uint64_t millihertz = timing.refresh_millihertz();

    is_possible = false;
    if (!fill_request(timing, target.head, target.window, params)) {
        log::info(
            "nvidia: output %u's timing %u, %u by %u at %lu.%03lu Hz, is refused, as its sync and back porch take "
            "fewer than %u lines",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            LEADING_IDLE_LINES_MIN
        );
        return OK;
    }

    int32_t rc = fw.control(bandwidth_check, rm::CONTROL_IS_MODE_POSSIBLE, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::info(
            "nvidia: the firmware refuses the bandwidth check's request about output %u's timing %u, %u by %u at "
            "%lu.%03lu Hz, on head %u with window %u, so the mode is refused",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            target.head,
            target.window
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: asking the bandwidth check about output %u's timing %u failed: %d",
            output,
            modes.indices[rank],
            rc
        );
        return rc;
    }

    is_possible = params.is_possible != 0;
    if (!is_possible) {
        log::info(
            "nvidia: the bandwidth check refuses output %u's timing %u, %u by %u at %lu.%03lu Hz, on head %u with "
            "window %u",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            target.head,
            target.window
        );
    }

    return OK;
}

// Asks about the output's modes in order until the bandwidth check accepts one, giving its rank
static int32_t find_possible_mode(
    firmware& fw,
    rm::handle bandwidth_check,
    uint8_t output,
    const output_modes& modes,
    const light_target& target,
    rm::mode_possible_params& params,
    bool& is_found,
    uint8_t& rank
) {
    for (rank = 0; rank < modes.count; rank++) {
        int32_t rc = check_mode(fw, bandwidth_check, output, modes, rank, target, params, is_found);
        if (rc != OK || is_found) {
            return rc;
        }
    }

    return OK;
}

int32_t choose_first_light(
    firmware& fw,
    rm::handle bandwidth_check,
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    first_light& out,
    bool& is_chosen
) {
    uint32_t params_bytes = sizeof(rm::mode_possible_params);
    auto* params = static_cast<rm::mode_possible_params*>(heap::uzalloc(params_bytes));
    if (params == nullptr) {
        log::error("nvidia: allocating %u bytes for the bandwidth check's request failed", params_bytes);
        return ERR_NO_MEMORY;
    }

    int32_t rc = OK;
    bool is_found = false;
    for (uint8_t tried = 0; tried < output_count; tried++) {
        uint8_t output = output_in_order(tried, target.first_output);
        uint8_t rank = 0;
        rc = find_possible_mode(fw, bandwidth_check, output, modes[output], target, *params, is_found, rank);
        if (rc != OK) {
            break;
        }

        if (is_found) {
            out = {
                .output = output,
                .timing_index = modes[output].indices[rank],
                .timing = modes[output].timings[rank],
                .head = target.head,
                .window = target.window,
            };

            break;
        }
    }

    heap::ufree(params);

    if (rc == OK) {
        is_chosen = is_found;
    }

    return rc;
}

} // namespace drivers::nvidia
