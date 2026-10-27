// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"

namespace drivers::nvidia {

static_assert(rm::WINDOW_HEAD_MASK_COUNT == DISPLAY_WINDOWS_MAX, "the reply gives a mask for every window a chip has");

static int32_t read_head_count(firmware& fw, rm::handle display_common, const chip_display& display, uint8_t& count) {
    rm::head_count_params params = {};
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_HEAD_COUNT, &params, sizeof(params));
    if (rc != OK) {
        return rc;
    }

    if (params.head_count == 0) {
        log::error("nvidia: display common reports no heads the display engine can use");
        return ERR_BAD_MESSAGE;
    }

    if (params.head_count > display.head_count) {
        log::error(
            "nvidia: display common reports %u heads the display engine can use, more than the chip's %u heads",
            params.head_count,
            display.head_count
        );
        return ERR_BAD_MESSAGE;
    }

    count = static_cast<uint8_t>(params.head_count);

    return OK;
}

static int32_t read_head_mask(firmware& fw, rm::handle display_common, const chip_display& display, uint32_t& mask) {
    rm::head_mask_params params = {};
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_HEAD_MASK, &params, sizeof(params));
    if (rc != OK) {
        return rc;
    }

    uint32_t chip_heads = (1u << display.head_count) - 1;
    if ((params.head_mask & ~chip_heads) != 0) {
        log::error(
            "nvidia: display common reports heads 0x%X usable, some past the chip's %u heads",
            params.head_mask,
            display.head_count
        );
        return ERR_BAD_MESSAGE;
    }

    mask = params.head_mask;

    return OK;
}

// The head the mask of a window names, outside a flexible mapping
static int32_t find_named_head(uint8_t mask, uint32_t window, const chip_display& display, uint8_t& head) {
    bool names_one_head = (mask & (mask - 1)) == 0;
    if (!names_one_head) {
        log::error(
            "nvidia: display common reports heads 0x%X for window %u, more than one while window 0's mask is not the "
            "0xFF that lets each window serve any head",
            mask,
            window
        );
        return ERR_BAD_MESSAGE;
    }

    uint8_t named = static_cast<uint8_t>(__builtin_ctz(mask));
    if (named >= display.head_count) {
        log::error(
            "nvidia: display common reports head %u for window %u, past the chip's %u heads",
            named,
            window,
            display.head_count
        );
        return ERR_BAD_MESSAGE;
    }

    head = named;

    return OK;
}

// Gives every window its head, or `NO_HEAD`, and the mask of the heads some window serves
static int32_t assign_window_heads(
    const rm::window_heads_params& params,
    const chip_display& display,
    display_heads& heads,
    uint32_t& served_heads
) {
    bool is_flexible = params.window_heads[0] == rm::FLEXIBLE_WINDOW_HEADS;
    for (uint32_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        uint8_t mask = params.window_heads[window];
        heads.window_head[window] = NO_HEAD;
        if (mask == 0) {
            continue;
        }

        if (window >= display.window_count) {
            log::error(
                "nvidia: display common reports heads 0x%X for window %u, past the chip's %u windows",
                mask,
                window,
                display.window_count
            );
            return ERR_BAD_MESSAGE;
        }

        uint8_t head = static_cast<uint8_t>(window >> 1);
        if (!is_flexible) {
            int32_t rc = find_named_head(mask, window, display, head);
            if (rc != OK) {
                return rc;
            }
        }

        heads.window_head[window] = head;
        served_heads |= 1u << head;
    }

    return OK;
}

int32_t read_display_heads(firmware& fw, rm::handle display_common, const chip_display& display, display_heads& out) {
    uint8_t reported_count = 0;
    int32_t rc = read_head_count(fw, display_common, display, reported_count);
    if (rc != OK) {
        return rc;
    }

    uint32_t head_mask = 0;
    rc = read_head_mask(fw, display_common, display, head_mask);
    if (rc != OK) {
        return rc;
    }

    rm::window_heads_params windows = {};
    rc = fw.control(display_common, rm::CONTROL_GET_WINDOW_HEADS, &windows, sizeof(windows));
    if (rc != OK) {
        return rc;
    }

    display_heads heads = {};
    uint32_t served_heads = 0;
    rc = assign_window_heads(windows, display, heads, served_heads);
    if (rc != OK) {
        return rc;
    }

    // Without a usable head 0 that a window serves, the packing below leaves the display engine no head
    if ((head_mask & 1u) == 0) {
        log::error(
            "nvidia: display common reports heads 0x%X usable, without head 0, which leaves the display engine no head",
            head_mask
        );
        return ERR_BAD_MESSAGE;
    }

    if ((served_heads & 1u) == 0) {
        log::error(
            "nvidia: display common reports windows serving heads 0x%X, without head 0, which leaves the display "
            "engine no head",
            served_heads
        );
        return ERR_BAD_MESSAGE;
    }

    // The heads the report leaves the display engine end at the first head the mask lacks or no window serves, as
    // NVIDIA keeps no head past a gap
    uint32_t served_usable_heads = head_mask & served_heads;
    uint8_t packed_count = 0;
    while ((served_usable_heads & (1u << packed_count)) != 0) {
        packed_count++;
    }

    heads.count = packed_count < reported_count ? packed_count : reported_count;
    for (uint8_t& head : heads.window_head) {
        if (head >= heads.count) {
            head = NO_HEAD;
        }
    }

    out = heads;

    return OK;
}

uint32_t windows_of_head(const display_heads& heads, uint8_t head) {
    if (head >= heads.count) {
        return 0;
    }

    uint32_t windows = 0;
    for (uint32_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        if (heads.window_head[window] == head) {
            windows |= 1u << window;
        }
    }

    return windows;
}

} // namespace drivers::nvidia
