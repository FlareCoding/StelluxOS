// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// NVIDIA's NVEvoScalerTapsToNum of NV_EVO_SCALER_2TAPS, the fewest taps NVIDIA's display classes give a scaler, which a
// scaler that does not scale takes
constexpr uint8_t UNSCALED_TAPS = 2;

// The fewest lines the sync and back porch may take together, as each needs at least one, below which a mode is refused
// without asking the bandwidth check, as NVIDIA's modesetting module refuses a mode of 1 line but asks about one of 0
constexpr uint32_t LEADING_IDLE_LINES_MIN = 2;

// NVIDIA's maxRasterWidth and maxRasterHeight, the largest totals a head's raster methods take, past which a mode is
// refused without asking the bandwidth check, as NVIDIA's modesetting module refuses it
constexpr uint32_t RASTER_SIZE_MAX = rm::HEAD_PAIR_MASK;

// NVIDIA's cursorSize32p for a 256-pixel cursor, in units of 32 pixels, the largest size, which NVIDIA's modesetting
// module assumes
constexpr uint8_t CURSOR_WIDTH_32 = 256 / 32;

// The bytes of a pixel in the formats the bandwidth check is asked about, and the multiple NVIDIA's modesetting module
// requires of the pitch of a surface laid out as rows of pixels in order
constexpr uint32_t BASE_BUFFER_BYTES_PER_PIXEL = 4;
constexpr uint32_t BASE_BUFFER_PITCH_ALIGNMENT = 256;

// The levels of a 10-bit color, the entries each of NVIDIA's identities gives before repeating its last, and what the
// input identity divides each entry's place by
constexpr uint32_t COLOR_LEVELS = 1024;

// A half-precision float's fraction bits and its exponent's bias, and where an entry's green and blue begin
constexpr uint32_t HALF_FRACTION_BITS = 10;
constexpr uint32_t HALF_EXPONENT_BIAS = 15;
constexpr uint32_t ENTRY_GREEN_SHIFT  = 16;
constexpr uint32_t ENTRY_BLUE_SHIFT   = 32;

// Where an output table entry's 16-bit colors hold a 10-bit level, NVIDIA's 16 - 10, and the surface's word where the
// output table begins
constexpr uint32_t OUTPUT_LEVEL_SHIFT      = 6;
constexpr uint32_t OUTPUT_TABLE_FIRST_WORD = OUTPUT_TABLE_OFFSET / sizeof(uint64_t);

// Fills `order` with the outputs below `output_count` in lighting order
static void fill_lighting_order(uint32_t first_outputs, uint8_t output_count, uint8_t* order) {
    uint8_t placed = 0;
    for (uint8_t output = 0; output < output_count; output++) {
        if ((first_outputs & (1u << output)) != 0) {
            order[placed] = output;
            placed++;
        }
    }

    for (uint8_t output = 0; output < output_count; output++) {
        if ((first_outputs & (1u << output)) == 0) {
            order[placed] = output;
            placed++;
        }
    }
}

light_target find_light_target(const display_heads& heads, const active_heads& active, const display_outputs& outputs) {
    light_target target = {};
    find_boot_head(active, target.head);
    target.first_outputs = find_shown_outputs(active, outputs);

    // The boot head and head 0 are below `heads.count`, and `display_heads` promises each such head a window
    target.window = static_cast<uint8_t>(__builtin_ctz(windows_of_head(heads, target.head)));

    return target;
}

head_raster raster_of(const edid::timing& timing) {
    uint32_t h_total = timing.h_total();
    uint32_t v_total = timing.v_total();
    uint32_t h_sync_start = timing.width + timing.h_front_porch;
    uint32_t v_sync_start = timing.height + timing.v_front_porch;
    uint32_t blank_end_y = (v_total - v_sync_start) - 1;
    uint32_t leading_idle_lines = blank_end_y + 1;

    return {
        .width = h_total,
        .height = v_total,
        .blank_start_x = timing.width + (h_total - h_sync_start) - 1,
        .blank_start_y = timing.height + blank_end_y,
        .blank_end_x = (h_total - h_sync_start) - 1,
        .blank_end_y = blank_end_y,
        .sync_end_x = timing.h_sync_width - 1,
        .sync_end_y = timing.v_sync_width - 1,
        .leading_idle_lines = leading_idle_lines,
        .trailing_idle_lines = v_total - (leading_idle_lines + timing.height),
    };
}

uint32_t base_buffer_pitch(uint32_t width) {
    uint32_t row_bytes = width * BASE_BUFFER_BYTES_PER_PIXEL;
    return (row_bytes + BASE_BUFFER_PITCH_ALIGNMENT - 1) & ~(BASE_BUFFER_PITCH_ALIGNMENT - 1);
}

// NVIDIA's nvUnorm10ToFp16: `level` / 1024 as a half-precision float, exact for every `level` below 1024, the only
// levels it takes. A `level` of 0 returns first, since `__builtin_clz` of 0 is undefined
static uint16_t half_of_level(uint32_t level) {
    if (level == 0) {
        return 0;
    }

    uint32_t top_bit = 31 - static_cast<uint32_t>(__builtin_clz(level));
    uint32_t exponent = top_bit + HALF_EXPONENT_BIAS - HALF_FRACTION_BITS;
    uint32_t fraction = (level << (HALF_FRACTION_BITS - top_bit)) & ((1u << HALF_FRACTION_BITS) - 1);

    return static_cast<uint16_t>(exponent << HALF_FRACTION_BITS | fraction);
}

uint64_t look_up_table_word(uint32_t index) {
    bool is_output_table = index >= OUTPUT_TABLE_FIRST_WORD;
    uint32_t entry = is_output_table ? index - OUTPUT_TABLE_FIRST_WORD : index;
    bool is_header = entry < LOOK_UP_TABLE_HEADER_ENTRIES;
    bool is_past_table = entry >= LOOK_UP_TABLE_SIZE;
    if (is_header || is_past_table) {
        return 0;
    }

    // The last entry repeats the one before, as both of NVIDIA's identities end
    uint32_t place = entry - LOOK_UP_TABLE_HEADER_ENTRIES;
    uint32_t level = place < COLOR_LEVELS ? place : COLOR_LEVELS - 1;
    uint64_t color = is_output_table ? level << OUTPUT_LEVEL_SHIFT : half_of_level(level);

    return color | color << ENTRY_GREEN_SHIFT | color << ENTRY_BLUE_SHIFT;
}

// Fills one head and its window of a request to show `timing` on `head` with `window` scanning it out, and gives
// whether its sync and back porch take enough lines for a request
static bool fill_head(
    const edid::timing& timing,
    uint8_t head,
    uint8_t window,
    rm::mode_possible_head& asked_head,
    rm::mode_possible_window& asked_window
) {
    head_raster raster = raster_of(timing);
    if (raster.leading_idle_lines < LEADING_IDLE_LINES_MIN) {
        return false;
    }

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

    asked_head.leading_idle_lines = static_cast<uint16_t>(raster.leading_idle_lines);
    asked_head.trailing_idle_lines = static_cast<uint16_t>(raster.trailing_idle_lines);
    asked_head.lut = rm::LUT_USAGE_1025;
    asked_head.cursor_width_32 = CURSOR_WIDTH_32;

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

// Fills `params` in ascending head order to ask whether the heads of `chosen` can show their modes while `head` shows
// `timing` with `window`, and gives whether that timing's sync and back porch take enough lines for a request
static bool fill_request(
    const light_set& chosen,
    const edid::timing& timing,
    uint8_t head,
    uint8_t window,
    rm::mode_possible_params& params
) {
    string::memset(&params, 0, sizeof(params));

    uint8_t slot = 0;
    for (uint8_t asked_head = 0; asked_head < DISPLAY_HEADS_MAX; asked_head++) {
        if (asked_head == head) {
            if (!fill_head(timing, head, window, params.heads[slot], params.windows[slot])) {
                return false;
            }

            slot++;
        }

        for (uint8_t index = 0; index < chosen.count; index++) {
            const output_light& light = chosen.lights[index];
            if (light.head == asked_head) {
                fill_head(light.timing, light.head, light.window, params.heads[slot], params.windows[slot]);
                slot++;
            }
        }
    }

    // Each head names only the window the driver shows on it, where NVIDIA names a window for each of the head's
    // usable layers, as windows the driver never shows need no bandwidth
    params.head_count = slot;
    params.window_count = slot;

    return true;
}

// Asks the bandwidth check whether `head` can show the output's mode `rank` with `window` beside the outputs `chosen`
// holds, logging a refusal
static int32_t check_mode(
    firmware& fw,
    rm::handle bandwidth_check,
    uint8_t output,
    const output_modes& modes,
    uint8_t rank,
    const light_set& chosen,
    uint8_t head,
    uint8_t window,
    rm::mode_possible_params& params,
    bool& is_possible
) {
    const edid::timing& timing = modes.timings[rank];
    uint64_t millihertz = timing.refresh_millihertz();
    uint32_t h_total = timing.h_total();
    uint32_t v_total = timing.v_total();

    is_possible = false;
    bool fits_raster = h_total <= RASTER_SIZE_MAX && v_total <= RASTER_SIZE_MAX;
    if (!fits_raster) {
        log::info(
            "nvidia: output %u's timing %u, %u by %u at %lu.%03lu Hz, is refused, as its totals of %u by %u pass a "
            "head's largest raster, %u by %u",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            h_total,
            v_total,
            RASTER_SIZE_MAX,
            RASTER_SIZE_MAX
        );
        return OK;
    }

    if (!fill_request(chosen, timing, head, window, params)) {
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

    const char* beside = "";
    if (chosen.count == 1) {
        beside = ", beside the output chosen before it";
    } else if (chosen.count > 1) {
        beside = ", beside the outputs chosen before it";
    }

    int32_t rc = fw.control(bandwidth_check, rm::CONTROL_IS_MODE_POSSIBLE, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::info(
            "nvidia: the firmware refuses the bandwidth check's request about output %u's timing %u, %u by %u at "
            "%lu.%03lu Hz, on head %u with window %u%s, so the mode is refused",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            head,
            window,
            beside
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
            "window %u%s",
            output,
            modes.indices[rank],
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            head,
            window,
            beside
        );
    }

    return OK;
}

// Asks about the output's modes in order until the bandwidth check accepts one beside `chosen`, giving its rank
static int32_t find_possible_mode(
    firmware& fw,
    rm::handle bandwidth_check,
    uint8_t output,
    const output_modes& modes,
    const light_set& chosen,
    uint8_t head,
    uint8_t window,
    rm::mode_possible_params& params,
    bool& is_found,
    uint8_t& rank
) {
    // NVIDIA lowers the usage bounds it asks for before refusing a timing, where the driver writes fixed usage bounds,
    // as its one window format, 4 bytes a pixel unscaled, leaves nothing to lower
    for (rank = 0; rank < modes.count; rank++) {
        int32_t rc = check_mode(fw, bandwidth_check, output, modes, rank, chosen, head, window, params, is_found);
        if (rc != OK || is_found) {
            return rc;
        }
    }

    return OK;
}

// The lowest head below `head_count` that `chosen` leaves free, which the caller must know exists
static uint8_t lowest_free_head(const light_set& chosen, uint8_t head_count) {
    uint32_t taken = 0;
    for (uint8_t index = 0; index < chosen.count; index++) {
        taken |= 1u << chosen.lights[index].head;
    }

    uint32_t free_heads = ~taken & ((1u << head_count) - 1);
    return static_cast<uint8_t>(__builtin_ctz(free_heads));
}

int32_t choose_lights(
    firmware& fw,
    rm::handle bandwidth_check,
    const display_heads& heads,
    const output_modes* modes,
    uint8_t output_count,
    const light_target& target,
    light_set& out
) {
    string::memset(&out, 0, sizeof(out));

    uint32_t params_bytes = sizeof(rm::mode_possible_params);
    auto* params = static_cast<rm::mode_possible_params*>(heap::uzalloc(params_bytes));
    if (params == nullptr) {
        log::error("nvidia: allocating %u bytes for the bandwidth check's request failed", params_bytes);
        return ERR_NO_MEMORY;
    }

    uint8_t order[DISPLAY_IDS_MAX] = {};
    fill_lighting_order(target.first_outputs, output_count, order);

    int32_t rc = OK;
    for (uint8_t tried = 0; tried < output_count; tried++) {
        uint8_t output = order[tried];
        if (modes[output].count == 0) {
            continue;
        }

        if (out.count == heads.count) {
            log::info("nvidia: output %u is not chosen to light, as no head is left for it", output);
            continue;
        }

        uint8_t head = target.head;
        uint8_t window = target.window;
        if (out.count > 0) {
            // `display_heads` promises each head below its count a window
            head = lowest_free_head(out, heads.count);
            window = static_cast<uint8_t>(__builtin_ctz(windows_of_head(heads, head)));
        }

        bool is_found = false;
        uint8_t rank = 0;
        rc = find_possible_mode(fw, bandwidth_check, output, modes[output], out, head, window, *params, is_found, rank);
        if (rc != OK) {
            break;
        }

        if (!is_found) {
            log::info("nvidia: output %u is not chosen to light, as every one of its modes is refused", output);
            continue;
        }

        out.lights[out.count] = {
            .output = output,
            .timing_index = modes[output].indices[rank],
            .timing = modes[output].timings[rank],
            .head = head,
            .window = window,
        };

        out.count++;
    }

    heap::ufree(params);

    if (rc != OK) {
        string::memset(&out, 0, sizeof(out));
    }

    return rc;
}

} // namespace drivers::nvidia
