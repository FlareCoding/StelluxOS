// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The ALPHA of an opaque desktop color, in bits 7:0 of ALPHA_RED, black with its red, green and blue left 0
constexpr uint32_t OPAQUE_ALPHA = 255;

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

// Appends one method to `out`, which `SHUTDOWN_METHODS_MAX` keeps from overflowing
static void append_method(shutdown_methods& out, uint32_t method, uint32_t value) {
    out.methods[out.count] = {.method = method, .value = value};
    out.count++;
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

} // namespace drivers::nvidia
