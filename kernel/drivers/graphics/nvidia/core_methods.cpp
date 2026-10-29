// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The ALPHA of an opaque desktop color, in bits 7:0 of ALPHA_RED, black with its red, green and blue left 0
constexpr uint32_t OPAQUE_ALPHA = 255;

// The hertz of a kilohertz, a timing's pixel clock being in kilohertz and the method's in hertz
constexpr uint32_t HERTZ_PER_KILOHERTZ = 1000;

// A scale factor of one both across and down, in the two halves of a scale factor method
constexpr uint32_t UNSCALED_FACTORS = SCALE_FACTOR_ONE | SCALE_FACTOR_ONE << rm::SCALE_FACTOR_VERTICAL_SHIFT;

// The head's output table control, its size counting the header's entries, and where the table lies in its surface
constexpr uint32_t OUTPUT_TABLE_CONTROL =
    rm::OUTPUT_TABLE_MODE_DIRECT10 | LOOK_UP_TABLE_SIZE << rm::OUTPUT_TABLE_SIZE_SHIFT;
constexpr uint32_t OUTPUT_TABLE_ORIGIN = static_cast<uint32_t>(OUTPUT_TABLE_OFFSET >> rm::OUTPUT_TABLE_OFFSET_SHIFT);

// The OCSC0 matrix NVIDIA computes for RGB at the default saturation, through 709 YCbCr and back, rounded to S5.14: the
// identity, each row adding the 1/2048 NVIDIA adds only with its output rounding fix on
constexpr uint32_t OCSC0_COEFFICIENTS = 12;
constexpr uint32_t OCSC0_ONE          = 0x10000;
constexpr uint32_t OCSC0_ROUNDING     = 0x20;
static const uint32_t OCSC0_RGB_MATRIX[OCSC0_COEFFICIENTS] = {
    OCSC0_ONE, 0,         0,         OCSC0_ROUNDING,
    0,         OCSC0_ONE, 0,         OCSC0_ROUNDING,
    0,         0,         OCSC0_ONE, OCSC0_ROUNDING,
};

// NVIDIA's output scaler coefficients by ratio, phase and value, each one's position its index. Each row omits the
// center value, which the hardware derives, and phase 0's row holds phase 0's two values, then edge phase 16's two
static const int16_t SCALER_COEFFICIENT_VALUES[SCALER_COEFFICIENTS] = {
    // ratio 1
     0,   0, -16, 144,
     0,  -5,   5,   0,
     0,  -9,  11,   0,
    -1, -12,  18,  -1,
    -1, -15,  25,  -1,
    -1, -18,  33,  -2,
    -2, -20,  42,  -3,
    -2, -21,  51,  -3,
    -3, -22,  60,  -5,
    -3, -22,  70,  -6,
    -4, -22,  81,  -7,
    -4, -22,  91,  -9,
    -5, -21, 102, -10,
    -5, -20, 113, -12,
    -5, -19, 125, -13,
    -6, -18, 136, -15,
    // ratio 2
     3,  60,  20, 108,
     3,  57,  63,   4,
     2,  54,  66,   4,
     2,  51,  69,   5,
     2,  48,  72,   6,
     1,  45,  75,   7,
     1,  43,  78,   7,
     1,  40,  81,   8,
     1,  37,  84,   9,
     0,  35,  88,  10,
     0,  33,  91,  12,
     0,  30,  94,  13,
     0,  28,  97,  14,
     0,  26,  99,  16,
     0,  24, 102,  17,
     0,  22, 105,  19,
    // ratio 4
     4,  62,  23, 105,
     4,  59,  64,   5,
     3,  56,  67,   6,
     3,  53,  70,   7,
     2,  51,  73,   8,
     2,  48,  76,   8,
     2,  45,  79,   9,
     1,  43,  81,  10,
     1,  40,  84,  12,
     1,  38,  87,  13,
     1,  36,  90,  14,
     0,  34,  92,  15,
     0,  31,  95,  17,
     0,  29,  97,  18,
     0,  27, 100,  20,
     0,  25, 102,  22,
};

bool core_initial_method(uint8_t head_count, uint32_t index, display_method& out) {
    if (index >= CORE_INITIAL_METHODS_PER_HEAD * head_count) {
        return false;
    }

    uint32_t coefficient_methods = SCALER_COEFFICIENTS * head_count;
    if (index < coefficient_methods) {
        uint32_t coefficient = index / head_count;
        uint32_t head = index % head_count;
        uint32_t value = static_cast<uint32_t>(SCALER_COEFFICIENT_VALUES[coefficient]);
        uint32_t data = value & rm::SCALER_COEFFICIENT_DATA_MASK;

        out.method = rm::CORE_HEAD_OUTPUT_SCALER_COEFFICIENT + head * rm::CORE_HEAD_METHODS_STRIDE;
        out.value = data | coefficient << rm::SCALER_COEFFICIENT_INDEX_SHIFT;
        return true;
    }

    uint32_t color_index = index - coefficient_methods;
    uint32_t head_methods = color_index / DESKTOP_COLOR_METHODS * rm::CORE_HEAD_METHODS_STRIDE;
    bool is_alpha_red = color_index % DESKTOP_COLOR_METHODS == 0;
    if (is_alpha_red) {
        out.method = rm::CORE_HEAD_DESKTOP_COLOR_ALPHA_RED + head_methods;
        out.value = OPAQUE_ALPHA;
    } else {
        out.method = rm::CORE_HEAD_DESKTOP_COLOR_GREEN_BLUE + head_methods;
        out.value = 0;
    }

    return true;
}

// Appends one method to `out`, whose array `SHUTDOWN_METHODS_MAX` or `HEAD_LIGHT_METHODS_MAX` keeps from overflowing
template <typename method_list>
static void append_method(method_list& out, uint32_t method, uint32_t value) {
    out.methods[out.count] = {.method = method, .value = value};
    out.count++;
}

// Appends one of `head`'s methods, given by its offset for head 0
static void append_head_method(head_light_methods& out, uint8_t head, uint32_t method, uint32_t value) {
    append_method(out, method + head * rm::CORE_HEAD_METHODS_STRIDE, value);
}

// Packs two values into one head method as NVIDIA lays out a raster's pairs, the first in bits 14:0, the second 30:16
static uint32_t head_pair(uint32_t first, uint32_t second) {
    return (first & rm::HEAD_PAIR_MASK) | (second & rm::HEAD_PAIR_MASK) << rm::HEAD_PAIR_SECOND_SHIFT;
}

// NVIDIA's default usage bounds for a window that does not scale, fetching a line of a viewport `viewport_width` wide
static uint32_t window_usage_bounds(uint32_t viewport_width) {
    uint32_t pixels_fetched = (viewport_width + PIXELS_FETCHED_BEYOND_WIDTH) & rm::USAGE_PIXELS_FETCHED_MASK;
    return rm::DEFAULT_WINDOW_USAGE_BOUNDS | pixels_fetched;
}

// Appends each present window's control naming the head it serves, or none, then each one's default usage bounds
static void append_window_mapping(const display_heads& heads, uint32_t present_windows, shutdown_methods& out) {
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        if ((present_windows & (1u << window)) == 0) {
            continue;
        }

        uint8_t head = heads.window_head[window];
        uint32_t owner = head == NO_HEAD ? WINDOW_OWNER_NONE : head;
        append_method(out, rm::CORE_WINDOW_CONTROL + window * rm::CORE_WINDOW_METHODS_STRIDE, owner);
    }

    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        if ((present_windows & (1u << window)) == 0) {
            continue;
        }

        uint32_t method = rm::CORE_WINDOW_USAGE_BOUNDS + window * rm::CORE_WINDOW_METHODS_STRIDE;
        append_method(out, method, rm::DEFAULT_WINDOW_USAGE_BOUNDS);
    }
}

void build_shutdown_methods(
    const display_heads& heads,
    uint32_t present_windows,
    const active_heads& active,
    bool maps_windows,
    shutdown_methods& out
) {
    out.count = 0;
    if (maps_windows) {
        append_window_mapping(heads, present_windows, out);
    }

    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        const active_head& state = active.heads[head];
        if (state.display_id == 0) {
            continue;
        }

        uint32_t head_methods = head * rm::CORE_HEAD_METHODS_STRIDE;
        uint32_t link_method = rm::CORE_SERIAL_LINK_CONTROL + state.link_index * rm::CORE_SERIAL_LINK_METHODS_STRIDE;

        append_method(out, rm::CORE_HEAD_CURSOR_CONTEXT_DMA + head_methods, 0);
        append_method(out, rm::CORE_HEAD_CURSOR_CONTROL + head_methods, rm::CURSOR_CONTROL_DISABLED);
        append_method(out, link_method, 0);
        append_method(out, rm::CORE_HEAD_DISPLAY_ID + head_methods, 0);
    }
}

// Appends the head's output table, read through the light's context DMA, then OCSC0 turned on after it
static void append_output_table(const head_light& light, head_light_methods& out) {
    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_TABLE_CONTROL, OUTPUT_TABLE_CONTROL);
    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_TABLE_OFFSET, OUTPUT_TABLE_ORIGIN);
    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_TABLE_NORM_SCALE, rm::OUTPUT_TABLE_NORM_SCALE);
    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_TABLE_CONTEXT_DMA, light.output_table_context_dma);
    append_head_method(out, light.head, rm::CORE_HEAD_OCSC0_CONTROL, rm::OCSC0_ENABLE);
}

// Appends the head's raster and pixel clock, then the raster features it goes without, as NVIDIA's raster setting does
static void append_raster(const head_light& light, head_light_methods& out) {
    head_raster raster = raster_of(light.timing);
    uint32_t pixel_clock = (light.timing.pixel_clock_khz * HERTZ_PER_KILOHERTZ) & rm::PIXEL_CLOCK_HERTZ_MASK;

    uint32_t size = head_pair(raster.width, raster.height);
    uint32_t sync_end = head_pair(raster.sync_end_x, raster.sync_end_y);
    uint32_t blank_end = head_pair(raster.blank_end_x, raster.blank_end_y);
    uint32_t blank_start = head_pair(raster.blank_start_x, raster.blank_start_y);
    uint32_t frame_idle = head_pair(raster.leading_idle_lines, raster.trailing_idle_lines);

    append_head_method(out, light.head, rm::CORE_HEAD_OVERSCAN_COLOR, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_RASTER_SIZE, size);
    append_head_method(out, light.head, rm::CORE_HEAD_RASTER_SYNC_END, sync_end);
    append_head_method(out, light.head, rm::CORE_HEAD_RASTER_BLANK_END, blank_end);
    append_head_method(out, light.head, rm::CORE_HEAD_RASTER_BLANK_START, blank_start);
    append_head_method(out, light.head, rm::CORE_HEAD_MIN_FRAME_IDLE, frame_idle);

    append_head_method(out, light.head, rm::CORE_HEAD_PIXEL_CLOCK, pixel_clock);
    append_head_method(out, light.head, rm::CORE_HEAD_PIXEL_CLOCK_CONFIGURATION, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_PIXEL_CLOCK_MAX, pixel_clock);

    append_head_method(out, light.head, rm::CORE_HEAD_FRAME_PACKED_COLOR, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_HDMI_CONTROL, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_TILE_POSITION, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_RASTER_HBLANK_DELAY, 0);
}

// Appends the head's control written whole, then no flip lock pin, lock chain or lock offset
static void append_head_control(uint8_t head, head_light_methods& out) {
    append_head_method(out, head, rm::CORE_HEAD_CONTROL, 0);
    append_method(out, rm::CORE_SET_CONTROL, 0);
    append_head_method(out, head, rm::CORE_HEAD_LOCK_CHAIN, 0);
    append_head_method(out, head, rm::CORE_HEAD_LOCK_OFFSET, 0);
}

// Appends the head's compression turned off, with no PPS packet
static void append_compression(uint8_t head, head_light_methods& out) {
    append_head_method(out, head, rm::CORE_HEAD_DSC_CONTROL, 0);
    append_head_method(out, head, rm::CORE_HEAD_DSC_PPS_CONTROL, 0);
}

// Appends the output resource's control for the mode's sync polarities
static void append_output_resource(const head_light& light, head_light_methods& out) {
    uint32_t hsync_polarity = light.timing.h_sync_positive ? 0 : rm::OUTPUT_RESOURCE_HSYNC_NEGATIVE;
    uint32_t vsync_polarity = light.timing.v_sync_positive ? 0 : rm::OUTPUT_RESOURCE_VSYNC_NEGATIVE;
    uint32_t fixed_fields =
        rm::OUTPUT_RESOURCE_CRC_WHOLE_RASTER | rm::OUTPUT_RESOURCE_24_BITS_444 | rm::OUTPUT_RESOURCE_NO_PACKET_WINDOW;
    uint32_t control = fixed_fields | hsync_polarity | vsync_polarity;

    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_RESOURCE_CONTROL, control);
}

// Appends full-range RGB with no chroma filters, NVIDIA's OCSC0 matrix for it and no OCSC1 matrix
static void append_colors(uint8_t head, head_light_methods& out) {
    append_head_method(out, head, rm::CORE_HEAD_PROCAMP, 0);
    for (uint32_t index = 0; index < OCSC0_COEFFICIENTS; index++) {
        uint32_t method = rm::CORE_HEAD_OCSC0_COEFFICIENTS + index * sizeof(uint32_t);
        append_head_method(out, head, method, OCSC0_RGB_MATRIX[index]);
    }

    append_head_method(out, head, rm::CORE_HEAD_CLAMP_GREEN, rm::CLAMP_FULL_RANGE);
    append_head_method(out, head, rm::CORE_HEAD_CLAMP_RED_BLUE, rm::CLAMP_FULL_RANGE);
    append_head_method(out, head, rm::CORE_HEAD_OCSC1_CONTROL, 0);
}

// Appends the serial link's control carrying the head alone in DisplayPort on its sublink, then the head's display id
static void append_link(const head_light& light, head_light_methods& out) {
    uint32_t link_method = rm::CORE_SERIAL_LINK_CONTROL + light.link.index * rm::CORE_SERIAL_LINK_METHODS_STRIDE;
    uint32_t protocol = rm::SERIAL_LINK_PROTOCOL_DP_A + light.link.sublink;
    uint32_t link_control = 1u << light.head | protocol << rm::SERIAL_LINK_PROTOCOL_SHIFT;

    append_method(out, link_method, link_control);
    append_head_method(out, light.head, rm::CORE_HEAD_DISPLAY_ID, light.display_id);
}

// Appends a viewport the size of the mode and unscaled, the usage bounds of each window serving the head and then the
// head's own, then the viewport's origin
static void append_viewport(const head_light& light, head_light_methods& out) {
    uint32_t mode_size = head_pair(light.timing.width, light.timing.height);
    uint32_t usage_bounds = window_usage_bounds(light.timing.width);

    append_head_method(out, light.head, rm::CORE_HEAD_VIEWPORT_SIZE_IN, mode_size);
    append_head_method(out, light.head, rm::CORE_HEAD_VIEWPORT_POINT_OUT_ADJUST, 0);
    append_head_method(out, light.head, rm::CORE_HEAD_VIEWPORT_SIZE_OUT, mode_size);
    append_head_method(out, light.head, rm::CORE_HEAD_MAX_OUTPUT_SCALE_FACTOR, UNSCALED_FACTORS);

    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        if ((light.windows & (1u << window)) == 0) {
            continue;
        }

        append_method(out, rm::CORE_WINDOW_USAGE_BOUNDS + window * rm::CORE_WINDOW_METHODS_STRIDE, usage_bounds);
    }

    append_head_method(out, light.head, rm::CORE_HEAD_USAGE_BOUNDS, rm::UNSCALED_HEAD_USAGE_BOUNDS);
    append_head_method(out, light.head, rm::CORE_HEAD_VIEWPORT_POINT_IN, 0);
}

void build_head_light_methods(const head_light& light, head_light_methods& out) {
    out.count = 0;

    append_output_table(light, out);
    append_raster(light, out);
    append_head_control(light.head, out);
    append_compression(light.head, out);

    append_output_resource(light, out);
    append_colors(light.head, out);
    append_link(light, out);
    append_viewport(light, out);
    append_head_method(out, light.head, rm::CORE_HEAD_OUTPUT_SCALER_CONTROL, rm::OUTPUT_SCALER_TWO_TAPS);
}

void build_window_usage_methods(uint8_t window, uint32_t viewport_width, window_usage_methods& out) {
    uint32_t window_methods = window * rm::CORE_WINDOW_METHODS_STRIDE;

    out.methods[0] = {rm::CORE_WINDOW_FORMAT_USAGE_BOUNDS + window_methods, rm::FORMAT_USAGE_RGB_PACKED_4_BPP};
    out.methods[1] = {rm::CORE_WINDOW_MAX_INPUT_SCALE_FACTOR + window_methods, UNSCALED_FACTORS};
    out.methods[2] = {rm::CORE_WINDOW_USAGE_BOUNDS + window_methods, window_usage_bounds(viewport_width)};
}

} // namespace drivers::nvidia
