// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_ids.h"
#include "drivers/graphics/nvidia/display_info.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_link.h"
#include "drivers/graphics/nvidia/display_modes.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/display_vertical_blank.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/displayport.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/window_methods.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/barrier.h"
#include "hw/mmio.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "sched/sched.h"

namespace drivers::nvidia {

// NVIDIA's NV_EVO_SURFACE_ALIGNMENT, the alignment of the surfaces NVIDIA's modesetting module scans out and the least
// alignment of its look-up table surfaces, which it aligns to 4 KiB in any case
constexpr uint64_t SURFACE_ALIGNMENT = 0x1000;

// The cursor memory holds the 256 by 256 pixels of 4 bytes the cursor's control shows
constexpr uint32_t CURSOR_DIMENSION = 256;
constexpr uint64_t CURSOR_MEMORY_BYTES = CURSOR_DIMENSION * CURSOR_DIMENSION * sizeof(uint32_t);

// The cursor's coordinates past which none of its pixels shows on any mode, to which a written position is clamped
constexpr int64_t CURSOR_COORDINATE_MIN = -256;
constexpr int64_t CURSOR_COORDINATE_MAX = 32767;

// The cursor's position at (-256, -256), x in bits 15:0 and y in bits 31:16, where none of its pixels shows
constexpr uint32_t PARKED_CURSOR_POSITION = 0xFF00FF00;

// An update's window interlocks naming no window channel, so the update takes effect with no window channel's update
constexpr uint32_t NO_WINDOW_INTERLOCKS = 0;

// Where the core channel's armed state lies in BAR0, the methods the display engine took at its last update
constexpr uint32_t CORE_ARMED_STATE = CORE_CHANNEL_AREA + CORE_CHANNEL_ARMED_STATE;

constexpr uint64_t MILLIHERTZ_PER_HERTZ = 1000;

// The least time between two logs of a latch the deadline estimated, which repeats while the interrupt stays stopped
constexpr uint64_t ESTIMATE_LOG_INTERVAL_NS = 1000 * NS_PER_MS;

// The bindings to window channels that dropping one light undoes, last first: its base buffer's to its own window's
// channel, the look-up table's and two for each other head
struct light_bindings {
    uint8_t      count;
    rm::handle   objects[2 * DISPLAY_HEADS_MAX];
    uint8_t      windows[2 * DISPLAY_HEADS_MAX];
    engine_head* holders[2 * DISPLAY_HEADS_MAX]; // the head whose base buffer each binds, null for the table's
};

// Logs the maker, name and timings an output's EDID gives
static void log_monitor(uint8_t output_index, const edid::monitor& monitor) {
    log::info(
        "nvidia: output %u's monitor is %s \"%s\", its EDID giving %u %s, %s, with %u more that did not fit and %u "
        "extension %s skipped",
        output_index,
        monitor.maker,
        monitor.name,
        monitor.timing_count,
        monitor.timing_count == 1 ? "timing" : "timings",
        monitor.first_timing_is_preferred ? "the first preferred" : "none preferred",
        monitor.dropped_timings,
        monitor.skipped_extensions,
        monitor.skipped_extensions == 1 ? "block" : "blocks"
    );

    for (uint32_t index = 0; index < monitor.timing_count; index++) {
        const edid::timing& timing = monitor.timings[index];
        uint64_t millihertz = timing.refresh_millihertz();
        log::info(
            "nvidia: output %u's timing %u is %u by %u at %lu.%03lu Hz, with a pixel clock of %u kHz",
            output_index,
            index,
            timing.width,
            timing.height,
            millihertz / 1000,
            millihertz % 1000,
            timing.pixel_clock_khz
        );
    }
}

// Writes each of `count` bytes, at least one, into `text` as two hex digits, with a space between bytes and a null
// character after the last, so `text` holds 3 characters per byte
static void format_bytes(const uint8_t* bytes, uint32_t count, char* text) {
    static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
    for (uint32_t index = 0; index < count; index++) {
        uint8_t byte = bytes[index];
        char* pair = text + index * 3;
        pair[0] = HEX_DIGITS[byte >> 4];
        pair[1] = HEX_DIGITS[byte & 0xF];
        pair[2] = index + 1 < count ? ' ' : '\0';
    }
}

// Logs the bytes of the DPCD capabilities an output's monitor reports, with its PHY repeaters' when they were read
static void log_dpcd(uint8_t output_index, const dpcd_capabilities& dpcd) {
    char receiver[DPCD_RECEIVER_CAPABILITY_BYTES * 3];
    format_bytes(dpcd.receiver, DPCD_RECEIVER_CAPABILITY_BYTES, receiver);
    uint32_t receiver_address = dpcd.is_extended ? DPCD_EXTENDED_RECEIVER_CAPABILITIES : DPCD_RECEIVER_CAPABILITIES;

    log::info(
        "nvidia: output %u's monitor reports the DPCD capabilities %s from 0x%05X",
        output_index,
        receiver,
        receiver_address
    );

    if (!dpcd.has_phy_repeater_bytes) {
        return;
    }

    char phy_repeaters[DPCD_PHY_REPEATER_CAPABILITY_BYTES * 3];
    format_bytes(dpcd.phy_repeaters, DPCD_PHY_REPEATER_CAPABILITY_BYTES, phy_repeaters);
    log::info(
        "nvidia: output %u's PHY repeater capabilities from 0x%05X are %s",
        output_index,
        DPCD_PHY_REPEATER_CAPABILITIES,
        phy_repeaters
    );
}

// Logs the most lanes and the highest rate a link on an output's connector takes, the rate capped by the GPU's highest
// and as DisplayPort codes it
static void log_connector_link_limits(
    uint8_t output_index,
    const display_output& output,
    displayport::link_rate gpu_max_rate
) {
    displayport::link_limits limits = connector_link_limits(output, gpu_max_rate);
    log::info(
        "nvidia: a link on output %u's connector takes up to %u %s at DisplayPort rate 0x%02X",
        output_index,
        limits.max_lanes,
        limits.max_lanes == 1 ? "lane" : "lanes",
        static_cast<uint32_t>(limits.max_rate)
    );
}

// Logs the most lanes and the highest rate a link to an output's monitor takes, the rate as DisplayPort codes it, and
// whether the monitor takes enhanced framing
static void log_monitor_link_limits(uint8_t output_index, const dpcd_capabilities& dpcd) {
    displayport::link_limits limits = monitor_link_limits(dpcd);
    log::info(
        "nvidia: output %u's link to its monitor takes up to %u %s at DisplayPort rate 0x%02X %s enhanced framing",
        output_index,
        limits.max_lanes,
        limits.max_lanes == 1 ? "lane" : "lanes",
        static_cast<uint32_t>(limits.max_rate),
        takes_enhanced_framing(dpcd) ? "with" : "without"
    );
}

// Keeps the output's modes `find_mode_candidates` ranks, logs its mode, the first, with the lowest link configuration
// carrying it, or why it has none, as for a monitor below DPCD 1.0, which NVIDIA drives only under a registry key
static void keep_output_modes(
    uint8_t output_index,
    const display_output& output,
    const edid::monitor& monitor,
    const dpcd_capabilities& dpcd,
    const chip_displayport& gpu,
    output_modes& modes
) {
    if (!is_at_least_revision_1_0(dpcd)) {
        log::info("nvidia: output %u has no mode, as its monitor's DPCD gives a revision below 1.0", output_index);
        return;
    }

    if (monitor.timing_count == 0) {
        log::info("nvidia: output %u has no mode, as its EDID gives no timing", output_index);
        return;
    }

    // A limited output also drops timings its link carries, so its mode line counts only the timings below the bound
    static_assert(LIMITED_REFRESH_BOUND_MILLIHERTZ == 60600, "the lines give the bound as 60.600 Hz");
    const char* counted = output.is_limited_to_60_hz ? ", counting only those below 60.600 Hz" : "";

    displayport::link_facts facts = output_link_facts(output, dpcd, gpu);
    mode_candidates candidates = {};
    find_mode_candidates(monitor, facts, output.is_limited_to_60_hz, candidates);
    if (candidates.count == 0 && output.is_limited_to_60_hz) {
        log::info(
            "nvidia: output %u has no mode, as its EDID gives %u %s and its link carries none below 60.600 Hz",
            output_index,
            monitor.timing_count,
            monitor.timing_count == 1 ? "timing" : "timings"
        );
        return;
    }

    if (candidates.count == 0) {
        log::info(
            "nvidia: output %u has no mode, as its EDID gives %u %s and its link carries none",
            output_index,
            monitor.timing_count,
            monitor.timing_count == 1 ? "timing" : "timings"
        );
        return;
    }

    modes.count = candidates.count;
    string::memcpy(&modes.dpcd, &dpcd, sizeof(modes.dpcd));
    for (uint8_t rank = 0; rank < candidates.count; rank++) {
        modes.indices[rank] = candidates.timings[rank];
        modes.timings[rank] = monitor.timings[candidates.timings[rank]];
    }

    // A candidate's timing always has a configuration, as `find_mode_candidates` keeps only those
    uint8_t timing_index = candidates.timings[0];
    const edid::timing& timing = monitor.timings[timing_index];
    displayport::link_configuration config = {};
    displayport::find_lowest_configuration(facts, timing, config);

    uint64_t millihertz = timing.refresh_millihertz();
    log::info(
        "nvidia: output %u's mode is its EDID's timing %u, %u by %u at %lu.%03lu Hz, over %u %s at DisplayPort rate "
        "0x%02X, ranked first of the %u %s its link carries%s",
        output_index,
        timing_index,
        timing.width,
        timing.height,
        millihertz / 1000,
        millihertz % 1000,
        config.lanes,
        config.lanes == 1 ? "lane" : "lanes",
        static_cast<uint32_t>(config.rate),
        candidates.count,
        candidates.count == 1 ? "timing" : "timings",
        counted
    );
}

// Reads and logs each connected output's EDID, DPCD capabilities and mode, keeping its modes in `modes`, each EDID read
// into one `edid::monitor`, too big for the stack, on the unprivileged heap, and fills `power_down` for every output
static int32_t read_connected_monitors(
    firmware& fw,
    rm::handle display_common,
    const chip_displayport& gpu,
    const display_outputs& outputs,
    output_modes*& modes,
    power_down_facts& power_down
) {
    power_down = {};

    if (!gpu.fec_supported) {
        log::info("nvidia: no output has a mode, as the GPU's links lack FEC support");
    }

    if (outputs.count == 0) {
        return OK;
    }

    uint32_t modes_bytes = outputs.count * sizeof(output_modes);
    modes = static_cast<output_modes*>(heap::uzalloc(modes_bytes));
    if (modes == nullptr) {
        log::error("nvidia: allocating %u bytes for the outputs' modes failed", modes_bytes);
        return ERR_NO_MEMORY;
    }

    uint32_t monitor_bytes = sizeof(edid::monitor);
    auto* monitor = static_cast<edid::monitor*>(heap::uzalloc(monitor_bytes));
    if (monitor == nullptr) {
        log::error("nvidia: allocating %u bytes for parsing the outputs' EDIDs failed", monitor_bytes);
        return ERR_NO_MEMORY;
    }

    int32_t rc = OK;
    for (uint8_t index = 0; index < outputs.count; index++) {
        const display_output& output = outputs.outputs[index];
        power_down.rates[index] = connector_link_limits(output, gpu.max_rate).max_rate;

        if (!output.is_connected) {
            continue;
        }

        bool is_parsed = false;
        rc = read_output_edid(fw, display_common, output.display_id, *monitor, is_parsed);
        if (rc != OK) {
            break;
        }

        if (is_parsed) {
            log_monitor(index, *monitor);
        }

        dpcd_capabilities dpcd = {};
        bool is_read = false;
        rc = read_output_dpcd(fw, display_common, output.display_id, gpu.trains_phy_repeaters, dpcd, is_read);
        if (rc != OK) {
            break;
        }

        if (is_read) {
            log_dpcd(index, dpcd);
            log_monitor_link_limits(index, dpcd);

            displayport::link_rate monitor_rate = monitor_link_limits(dpcd).max_rate;
            if (static_cast<uint8_t>(monitor_rate) < static_cast<uint8_t>(power_down.rates[index])) {
                power_down.rates[index] = monitor_rate;
            }
        }

        if (is_read && is_at_least_revision_1_1(dpcd)) {
            power_down.d3_outputs |= 1u << index;
        }

        if (is_parsed && is_read && gpu.fec_supported) {
            keep_output_modes(index, output, *monitor, dpcd, gpu, modes[index]);
        }
    }

    heap::ufree(monitor);

    return rc;
}

// Frees the outputs' modes `read_connected_monitors` kept, if it kept any
static void free_output_modes(output_modes*& modes) {
    if (modes == nullptr) {
        return;
    }

    heap::ufree(modes);
    modes = nullptr;
}

static int32_t read_display_facts(
    firmware& fw,
    const display_objects& objects,
    const chip_display& display,
    bool& has_crossbar,
    display_heads& heads,
    display_outputs& outputs
) {
    int32_t rc = read_display_capabilities(fw, objects, has_crossbar);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the display engine can reach push buffers, semaphores and notifiers in system memory, and display "
        "common reports %s",
        has_crossbar ? "a crossbar" : "no crossbar"
    );

    rc = read_display_heads(fw, objects.display_common, display, heads);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: display common's report leaves the display engine %u of the chip's %u heads",
        heads.count,
        display.head_count
    );
    for (uint8_t head = 0; head < heads.count; head++) {
        log::info("nvidia: head %u is served by windows 0x%X", head, windows_of_head(heads, head));
    }

    display_ids ids = {};
    rc = read_display_ids(fw, objects.display_common, ids);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: display common reports display ids 0x%X supported, 0x%X of them connected, 0x%X digital and 0x%X "
        "of the digital ones static",
        ids.supported,
        ids.connected,
        ids.digital,
        ids.static_digital
    );

    rc = read_display_outputs(fw, objects.display_common, ids, outputs);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the static digital display ids give %u DisplayPort %s",
        outputs.count,
        outputs.count == 1 ? "output" : "outputs"
    );
    for (uint8_t index = 0; index < outputs.count; index++) {
        const display_output& output = outputs.outputs[index];
        log::info(
            "nvidia: output %u is display id 0x%X, %s, %s and %s, with lane code %u and link rate code %u",
            index,
            output.display_id,
            output.is_connected ? "connected" : "not connected",
            output.is_limited_to_60_hz ? "limited to 60 Hz" : "not limited to 60 Hz",
            output.prefers_rbr ? "preferring RBR" : "not preferring RBR",
            output.lanes_code,
            output.link_rate_code
        );

        log_connector_link_limits(index, output, display.displayport.max_rate);
    }

    // NVIDIA's library takes the DisplayPort hardware only as it makes a DisplayPort connector's main link (its link to
    // the monitor)
    if (outputs.count != 0) {
        rc = take_displayport_from_firmware(fw, objects.display_common);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

// Logs each active head's display id and link with the output the head shows, or that the display id names no output,
// then the boot head and the boot output, or that there is none
static void log_active_heads(const active_heads& active, const display_outputs& outputs) {
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        const active_head& state = active.heads[head];
        if (state.display_id == 0) {
            continue;
        }

        uint8_t output = 0;
        if (find_shown_output(active, head, outputs, output)) {
            log::info(
                "nvidia: head %u shows display id 0x%X, which is output %u, through link %u of NVIDIA's output "
                "resource type %u",
                head,
                state.display_id,
                output,
                state.link_index,
                state.link_type
            );
        } else {
            log::info(
                "nvidia: head %u shows display id 0x%X, which names no output, through link %u of NVIDIA's output "
                "resource type %u",
                head,
                state.display_id,
                state.link_index,
                state.link_type
            );
        }
    }

    uint8_t boot_head = 0;
    if (!find_boot_head(active, boot_head)) {
        log::info("nvidia: no head is active, so there is no boot head");
        return;
    }

    uint8_t boot_output = 0;
    if (!find_shown_output(active, boot_head, outputs, boot_output)) {
        log::info(
            "nvidia: head %u is the boot head, and its display id 0x%X names no output, so there is no boot output",
            boot_head,
            active.heads[boot_head].display_id
        );
        return;
    }

    log::info(
        "nvidia: head %u is the boot head, and output %u, which it shows, is the boot output",
        boot_head,
        boot_output
    );
}

// Chooses the outputs to light and their modes with `choose_lights`, the first on the head and window
// `find_light_target` gives, and logs each choice or why none is made
static int32_t choose_and_log_lights(
    firmware& fw,
    rm::handle bandwidth_check,
    const display_heads& heads,
    const active_heads& active,
    const display_outputs& outputs,
    const output_modes* modes,
    light_set& set
) {
    uint8_t outputs_with_modes = 0;
    for (uint8_t index = 0; index < outputs.count; index++) {
        if (modes[index].count > 0) {
            outputs_with_modes++;
        }
    }

    if (outputs_with_modes == 0) {
        log::info("nvidia: no output is chosen to light, as no connected output has a mode");
        return OK;
    }

    light_target target = find_light_target(heads, active, outputs);
    int32_t rc = choose_lights(fw, bandwidth_check, heads, modes, outputs.count, target, set);

    // Lighting nothing leaves the driver attached, so a request that cannot be allocated ends only the choice
    if (rc == ERR_NO_MEMORY) {
        log::error("nvidia: no output is chosen to light, as the bandwidth check's request cannot be allocated");
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (set.count == 0) {
        log::info(
            "nvidia: no output is chosen to light, as every mode of the %u %s with modes is refused",
            outputs_with_modes,
            outputs_with_modes == 1 ? "output" : "outputs"
        );
        return OK;
    }

    for (uint8_t index = 0; index < set.count; index++) {
        const output_light& light = set.lights[index];
        uint64_t millihertz = light.timing.refresh_millihertz();
        log::info(
            "nvidia: output %u is chosen to light %s, at its EDID's timing %u, %u by %u at %lu.%03lu Hz, on head %u "
            "with window %u, which the bandwidth check accepts",
            light.output,
            index == 0 ? "first" : "next",
            light.timing_index,
            light.timing.width,
            light.timing.height,
            millihertz / 1000,
            millihertz % 1000,
            light.head,
            light.window
        );
    }

    return OK;
}

// Allocates a channel's push buffer in DMA pages the card reaches, cached only when the card's DMA is coherent
static int32_t allocate_push_buffer(const dma::device_profile& card, dma::buffer& out) {
    size_t pages = pmm::page_align_up(PUSH_BUFFER_BYTES) / pmm::PAGE_SIZE;
    int32_t rc = dma::OK;
    RUN_ELEVATED(rc = dma::alloc_pages(card, pages, dma::caching::cached_when_coherent, paging::PAGE_USER, out));

    return rc;
}

// Allocates and logs the chosen window's channel and push buffer, or logs why it has none and returns `OK` (heads with
// no output scaler, the window not present, no room or a refused request), else the failed request's error
static int32_t set_up_window_channel(
    firmware& fw,
    registers& regs,
    const dma::device_profile& card,
    rm::handle display,
    const chip_display& chip,
    uint32_t present_windows,
    uint8_t window,
    dma::buffer& push_buffer,
    rm::handle& channel
) {
    if (!chip.heads_have_output_scalers) {
        log::error(
            "nvidia: window %u has no channel, as the chip's heads report no output scaler, and the driver loads no "
            "window's scaler coefficients",
            window
        );
        return OK;
    }

    if ((present_windows & (1u << window)) == 0) {
        log::error(
            "nvidia: window %u has no channel, as the GSP's firmware does not report the window present",
            window
        );
        return OK;
    }

    int32_t rc = allocate_push_buffer(card, push_buffer);
    if (rc != dma::OK) {
        log::error("nvidia: window %u has no channel, as allocating its push buffer failed: %d", window, rc);
        return OK;
    }

    rc = allocate_window_channel(fw, display, chip, window, push_buffer.phys, channel);
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: window %u has no channel, as the GSP's firmware refused a request for it", window);
        return OK;
    }

    if (rc == ERR_NO_MEMORY) {
        log::error("nvidia: window %u has no channel, as a request for it cannot be allocated", window);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the GSP's firmware keeps window %u's channel 0x%X, whose push buffer lies at physical address 0x%lX, "
        "and the channel takes no initial method, its PUT and GET reading 0x%X and 0x%X, offsets into the push buffer",
        window,
        channel,
        push_buffer.phys,
        regs.read(window_channel_area(window) + CHANNEL_PUT),
        regs.read(window_channel_area(window) + CHANNEL_GET)
    );

    return OK;
}

// Allocates a chosen output's base buffer at its mode's size and clears it through a mapping for the CPU, logging it,
// or returns `OK` with why the output has no base buffer logged, else the error of the call that failed, logged
static int32_t set_up_base_buffer(video_memory& memory, const output_light& light, vram_range& out) {
    uint32_t pitch = base_buffer_pitch(light.timing.width);
    uint64_t bytes = static_cast<uint64_t>(pitch) * light.timing.height;

    vram_range allocated = {};
    int32_t rc = memory.allocate(bytes, SURFACE_ALIGNMENT, vram_use::scanout, allocated);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: output %u has no base buffer, as 0x%lX bytes of video memory for it cannot be allocated",
            light.output,
            bytes
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory for output %u's base buffer failed: %d",
            bytes,
            light.output,
            rc
        );
        return rc;
    }

    cpu_mapping mapping = {};
    rc = memory.map_for_cpu(allocated, mapping);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: output %u has no base buffer, as mapping it for the CPU to clear it found no room, so its video "
            "memory is released",
            light.output
        );
        memory.release(allocated);
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: mapping output %u's base buffer at 0x%lX in video memory for the CPU to clear it failed: %d, so "
            "the memory is released",
            light.output,
            allocated.offset,
            rc
        );
        memory.release(allocated);
        return rc;
    }

    for (uint64_t offset = 0; offset < allocated.size; offset += sizeof(uint64_t)) {
        mmio::write64(mapping.virt + offset, 0);
    }

    memory.unmap_for_cpu(allocated);
    out = allocated;

    log::info(
        "nvidia: output %u's base buffer lies at 0x%lX in video memory, its %u %s of %u bytes, and is cleared to black",
        light.output,
        allocated.offset,
        light.timing.height,
        light.timing.height == 1 ? "row" : "rows",
        pitch
    );

    return OK;
}

// Binds `surface`, video memory for `window` to read, to the window's channel under a new handle given in
// `context_dma`, or on a failed binding gives 0 there and releases and empties `surface`, since nothing names it then
static int32_t bind_to_window_channel(
    firmware& fw,
    instance_memory& instances,
    video_memory& memory,
    uint8_t window,
    vram_range& surface,
    rm::handle& context_dma
) {
    context_dma = fw.new_handle();
    context_dma_memory surface_memory = {
        .in_video_memory = true,
        .address = surface.offset,
        .bytes = surface.size,
    };

    int32_t rc = instances.bind(context_dma, window_channel_number(window), surface_memory);
    if (rc != OK) {
        memory.release(surface);
        surface = {};
        context_dma = 0;
    }

    return rc;
}

static int32_t set_up_scanout_buffer(video_memory& memory, uint32_t width, uint32_t height, scanout_buffer& buffer) {
    uint32_t pitch = base_buffer_pitch(width);
    uint64_t bytes = static_cast<uint64_t>(pitch) * height;

    vram_range range = {};
    int32_t rc = memory.allocate(bytes, SURFACE_ALIGNMENT, vram_use::scanout, range);
    if (rc != OK) {
        return rc == ERR_NO_MEMORY ? gfxfb::ERR_NO_MEMORY : gfxfb::ERR_REFUSED;
    }

    cpu_mapping mapping = {};
    rc = memory.map_for_cpu(range, mapping);
    if (rc == OK) {
        for (uint64_t offset = 0; offset < range.size; offset += sizeof(uint64_t)) {
            mmio::write64(mapping.virt + offset, 0);
        }

        // Taking the mapping down completes the clear before the head can scan the buffer
        memory.unmap_for_cpu(range);
        rc = memory.map_for_cpu(range, mapping);
    }

    bool is_mapped_without_address = rc == OK && mapping.phys == 0;
    if (is_mapped_without_address) {
        memory.unmap_for_cpu(range);
    }

    if (rc != OK || is_mapped_without_address) {
        memory.release(range);
        return rc == ERR_NO_MEMORY ? gfxfb::ERR_NO_MEMORY : gfxfb::ERR_REFUSED;
    }

    static_cast<gfxfb::buffer&>(buffer) = {
        .width     = width,
        .height    = height,
        .pitch     = pitch,
        .size      = range.size,
        .phys      = mapping.phys,
        .cacheable = false,
    };

    buffer.range = range;

    return gfxfb::OK;
}

// Binds a chosen output's base buffer to its window's channel as `bind_to_window_channel` binds a surface, returning
// `OK` when the binding finds no room and its error when it fails otherwise, each outcome logged
static int32_t bind_base_buffer(
    firmware& fw,
    instance_memory& instances,
    video_memory& memory,
    const output_light& light,
    vram_range& base_buffer,
    rm::handle& context_dma
) {
    int32_t rc = bind_to_window_channel(fw, instances, memory, light.window, base_buffer, context_dma);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: output %u has no base buffer, as binding it to window %u's channel found no room, so its video "
            "memory is released",
            light.output,
            light.window
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: binding output %u's base buffer to window %u's channel failed: %d, so its video memory is "
            "released",
            light.output,
            light.window,
            rc
        );
        return rc;
    }

    log::info(
        "nvidia: output %u's base buffer is bound to window %u's channel as context DMA 0x%X",
        light.output,
        light.window,
        context_dma
    );

    return OK;
}

// Maps a chosen output's bound base buffer for userland, describing the mapping in `out`, or returns `OK` with `out`
// unchanged when the mapping finds no room or has no physical address, else its error, each logged
static int32_t map_base_buffer_for_userland(
    video_memory& memory,
    const output_light& light,
    const vram_range& base_buffer,
    gfxfb::buffer& out
) {
    cpu_mapping mapping = {};
    int32_t rc = memory.map_for_cpu(base_buffer, mapping);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: output %u cannot light, as mapping its base buffer for userland found no room",
            light.output
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: mapping output %u's base buffer at 0x%lX in video memory for userland failed: %d",
            light.output,
            base_buffer.offset,
            rc
        );
        return rc;
    }

    if (mapping.phys == 0) {
        memory.unmap_for_cpu(base_buffer);
        log::error(
            "nvidia: output %u cannot light, as the mapping of its base buffer for userland has no physical address",
            light.output
        );
        return OK;
    }

    out = {
        .width     = light.timing.width,
        .height    = light.timing.height,
        .pitch     = base_buffer_pitch(light.timing.width),
        .size      = base_buffer.size,
        .phys      = mapping.phys,
        .cacheable = false,
    };

    log::info(
        "nvidia: output %u's base buffer is mapped for userland at physical address 0x%lX in BAR1, 0x%lX bytes",
        light.output,
        mapping.phys,
        base_buffer.size
    );

    return OK;
}

// Allocates the look-up table surface and writes `look_up_table_word`'s words through a mapping for the CPU, or
// returns `OK` when a call finds no room, else its error, each logged, the memory released after a failed mapping
static int32_t set_up_look_up_table(video_memory& memory, vram_range& out) {
    vram_range allocated = {};
    int32_t rc = memory.allocate(LOOK_UP_TABLE_SURFACE_BYTES, SURFACE_ALIGNMENT, vram_use::general, allocated);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: the display engine has no look-up table surface, as 0x%lX bytes of video memory for it cannot be "
            "allocated",
            LOOK_UP_TABLE_SURFACE_BYTES
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory for the look-up table surface failed: %d",
            LOOK_UP_TABLE_SURFACE_BYTES,
            rc
        );
        return rc;
    }

    cpu_mapping mapping = {};
    rc = memory.map_for_cpu(allocated, mapping);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: the display engine has no look-up table surface, as mapping it for the CPU to fill it found no "
            "room, so its video memory is released"
        );
        memory.release(allocated);
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: mapping the look-up table surface at 0x%lX in video memory for the CPU to fill it failed: %d, so "
            "the memory is released",
            allocated.offset,
            rc
        );
        memory.release(allocated);
        return rc;
    }

    uint64_t words = allocated.size / sizeof(uint64_t);
    for (uint32_t index = 0; index < words; index++) {
        mmio::write64(mapping.virt + index * sizeof(uint64_t), look_up_table_word(index));
    }

    memory.unmap_for_cpu(allocated);
    out = allocated;

    log::info(
        "nvidia: the look-up table surface lies at 0x%lX in video memory, its input and output tables passing each "
        "color through unchanged",
        allocated.offset
    );

    return OK;
}

// Binds `table` under one new handle to the core channel, then `window`'s, returning the first error that is not
// `ERR_NO_MEMORY` or else `OK`, each outcome logged. On failure the handle is 0 and `table` empty, released if unbound
static int32_t bind_look_up_table(
    firmware& fw,
    instance_memory& instances,
    video_memory& memory,
    uint8_t window,
    vram_range& table,
    rm::handle& context_dma
) {
    context_dma = fw.new_handle();
    context_dma_memory table_memory = {
        .in_video_memory = true,
        .address = table.offset,
        .bytes = table.size,
    };

    int32_t rc = instances.bind(context_dma, CORE_CHANNEL_NUMBER, table_memory);
    bool is_core_bound = rc == OK;
    if (is_core_bound) {
        rc = instances.bind(context_dma, window_channel_number(window), table_memory);
    }

    if (rc == OK) {
        log::info(
            "nvidia: the look-up table surface is bound to the core channel and window %u's channel as context DMA "
            "0x%X",
            window,
            context_dma
        );
        return OK;
    }

    // No method has named the context DMA, so the core channel's binding can be undone before the memory goes
    int32_t unbind_rc = is_core_bound ? instances.unbind(context_dma, CORE_CHANNEL_NUMBER) : OK;
    bool is_unbound = unbind_rc == OK;
    if (is_unbound) {
        memory.release(table);
    }

    const char* failed_channel = is_core_bound ? "window's channel" : "core channel";
    const char* memory_outcome = is_unbound ? "its video memory is released"
                                            : "its video memory stays allocated, still bound to the core channel";
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: the display engine has no look-up table surface for window %u, as binding it to the %s found no "
            "room, so %s",
            window,
            failed_channel,
            memory_outcome
        );
    } else {
        log::error(
            "nvidia: binding the look-up table surface for window %u to the %s failed: %d, so %s",
            window,
            failed_channel,
            rc,
            memory_outcome
        );
    }

    table = {};
    context_dma = 0;

    if (rc != ERR_NO_MEMORY) {
        return rc;
    }

    return unbind_rc == ERR_NO_MEMORY ? OK : unbind_rc;
}

// Logs whether the firmware reported the core channel idle before the wait for it gave up
static void log_core_channel_idle(bool is_idle) {
    static_assert(CORE_CHANNEL_IDLE_WAIT_NS == 100 * NS_PER_MS, "the line gives the wait as 100 milliseconds");

    if (is_idle) {
        log::info("nvidia: the GSP's firmware reports the core channel idle");
    } else {
        log::error("nvidia: the GSP's firmware did not report the core channel idle within 100 milliseconds");
    }
}

// Logs the owner of each present window in the core channel's armed state, warning of one naming a head the chip lacks
static void log_window_owners(const window_owners& owners, uint32_t present_windows, uint8_t head_count) {
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool is_present = (present_windows & (1u << window)) != 0;
        if (!is_present) {
            continue;
        }

        uint8_t owner = owners.owners[window];
        if (owner == WINDOW_OWNER_NONE) {
            log::info("nvidia: no head owns window %u in the core channel's armed state", window);
        } else if (owner < head_count) {
            log::info("nvidia: head %u owns window %u in the core channel's armed state", owner, window);
        } else {
            log::warn(
                "nvidia: window %u's owner in the core channel's armed state reads %u, a head the chip lacks",
                window,
                owner
            );
        }
    }
}

// Writes `method` with its `count` values into `queue`, and on `ERR_QUEUE_FULL` kicks, waits for GET to reach PUT and
// writes it again, as `display_channel::write` asks of a writer that may wait, one wait sufficing up to 504 values
static int32_t write_waiting(display_channel& queue, uint32_t method, const uint32_t* values, uint32_t count) {
    int32_t rc = queue.write(method, values, count);
    if (rc != ERR_QUEUE_FULL) {
        return rc;
    }

    queue.kick();
    rc = queue.wait_idle(clock::now_ns() + IDLE_TIMEOUT_NS);
    if (rc != OK) {
        return rc;
    }

    return queue.write(method, values, count);
}

// Writes a core channel method of one value into `queue`, kicking and waiting once when the queue is full
static int32_t write_waiting(display_channel& queue, const display_method& method) {
    return write_waiting(queue, method.method, &method.value, 1);
}

// Writes the core channel's initial methods for `head_count` heads, counting them in `written`
static int32_t write_core_initial_methods(display_channel& queue, uint8_t head_count, uint32_t& written) {
    display_method method = {};
    while (core_initial_method(head_count, written, method)) {
        int32_t rc = write_waiting(queue, method);
        if (rc != OK) {
            return rc;
        }

        written++;
    }

    return OK;
}

// Sends the start or the end, `phase`, of the mode set, naming `display_id`
static int32_t send_display_change(firmware& fw, rm::handle display_common, uint32_t display_id, uint32_t phase) {
    rm::display_change_params params = {};
    params.display_ids = display_id;
    params.phase = phase;

    return fw.control(display_common, rm::CONTROL_DISPLAY_CHANGE, &params, sizeof(params));
}

// Writes `count` of `methods` into `queue` with `write_waiting`
static int32_t write_methods(display_channel& queue, const display_method* methods, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
        int32_t rc = write_waiting(queue, methods[index]);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

// Writes and kicks an update interlocked with no cursor and with the window channels of `window_interlocks`, bit i for
// window i, naming the notifier's context DMA `notifier_context_dma` with write-awaken, or none for 0
static int32_t write_update(display_channel& queue, rm::handle notifier_context_dma, uint32_t window_interlocks) {
    uint32_t notifier_control = 0;
    if (notifier_context_dma != 0) {
        notifier_control = rm::NOTIFIER_CONTROL_MODE_WRITE_AWAKEN | rm::NOTIFIER_CONTROL_NOTIFY;
    }

    const display_method methods[] = {
        {rm::CORE_SET_NOTIFIER_CONTEXT_DMA, notifier_context_dma},
        {rm::CORE_SET_NOTIFIER_CONTROL, notifier_control},
        {rm::CORE_SET_INTERLOCK_FLAGS, 0},
        {rm::CORE_SET_WINDOW_INTERLOCK_FLAGS, window_interlocks},
        {rm::CORE_UPDATE, rm::CORE_UPDATE_RELEASE_ELV},
    };

    int32_t rc = write_methods(queue, methods, sizeof(methods) / sizeof(methods[0]));
    if (rc != OK) {
        return rc;
    }

    queue.kick();
    return OK;
}

// Stops later updates from writing the completion notifier, kicked with no update of their own, as NVIDIA's modesetting
// module does after each wait for the notifier, since the hardware may otherwise write it again
static int32_t stop_notifier_writes(display_channel& queue) {
    const display_method methods[] = {
        {rm::CORE_SET_NOTIFIER_CONTEXT_DMA, 0},
        {rm::CORE_SET_NOTIFIER_CONTROL, 0},
    };

    int32_t rc = write_methods(queue, methods, sizeof(methods) / sizeof(methods[0]));
    if (rc != OK) {
        return rc;
    }

    queue.kick();
    return OK;
}

// Writes `head`'s cursor methods into `queue`, showing the cursor memory `context_dma` names, or no cursor for 0
static int32_t write_cursor_methods(display_channel& queue, uint8_t head, rm::handle context_dma) {
    cursor_methods methods = {};
    build_cursor_methods(head, context_dma, methods);

    return write_methods(queue, methods.methods, CURSOR_METHODS);
}

// Writes one method once `channel`'s free register shows room, its last try coming once `wait_ns` has passed
static int32_t write_when_free(cursor_channel& channel, uint32_t offset, uint32_t value, uint64_t wait_ns) {
    uint64_t deadline_ns = clock::now_ns() + wait_ns;

    while (true) {
        bool is_last_try = clock::now_ns() > deadline_ns;
        int32_t rc = channel.write(offset, value);
        if (rc != ERR_QUEUE_FULL || is_last_try) {
            return rc;
        }
    }
}

// One coordinate of the cursor's top left pixel as `HOT_SPOT_POINT` takes it, clamped to where none of it shows
static uint32_t cursor_coordinate(int64_t coordinate) {
    if (coordinate < CURSOR_COORDINATE_MIN) {
        return static_cast<uint16_t>(CURSOR_COORDINATE_MIN);
    }

    if (coordinate > CURSOR_COORDINATE_MAX) {
        return static_cast<uint16_t>(CURSOR_COORDINATE_MAX);
    }

    return static_cast<uint16_t>(coordinate);
}

// The `HOT_SPOT_POINT` value putting the hotspot (`hot_x`, `hot_y`) at (`x`, `y`)
static uint32_t cursor_position(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y) {
    uint32_t column = cursor_coordinate(static_cast<int64_t>(x) - hot_x);
    uint32_t row = cursor_coordinate(static_cast<int64_t>(y) - hot_y);

    return row << 16 | column;
}

// The pixel of `image` at (`column`, `row`), transparent outside it
static uint32_t cursor_pixel(const gfxfb::cursor_image& image, uint32_t column, uint32_t row) {
    bool is_inside = column < image.width && row < image.height;
    return is_inside ? image.pixels[row * image.width + column] : 0;
}

// Writes `row` of the cursor memory mapped at `cursor` from `image`, two pixels a write to halve the row's lock hold
static void write_cursor_row(uintptr_t cursor, const gfxfb::cursor_image& image, uint32_t row) {
    uintptr_t row_start = cursor + static_cast<uintptr_t>(row) * CURSOR_DIMENSION * sizeof(uint32_t);
    for (uint32_t column = 0; column < CURSOR_DIMENSION; column += 2) {
        uint64_t left = cursor_pixel(image, column, row);
        uint64_t right = cursor_pixel(image, column + 1, row);
        mmio::write64(row_start + column * sizeof(uint32_t), right << 32 | left);
    }
}

// Writes a window image's methods into `queue` with `write_waiting`, kicking only on a wrap or a full queue
static int32_t write_window_methods(display_channel& queue, const window_image_methods& image) {
    for (const window_method& method : image.methods) {
        int32_t rc = write_waiting(queue, method.method, method.values, method.count);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

// The push buffer bytes a window image's methods take, a header and the values for each method
static uint32_t window_image_bytes(const window_image_methods& image) {
    uint32_t words = 0;
    for (const window_method& method : image.methods) {
        words += 1 + method.count;
    }

    return words * 4;
}

// Writes a window image's methods into room `display_channel::reserve` made, so no write reads GET or waits
static int32_t write_reserved_window_methods(display_channel& queue, const window_image_methods& image) {
    for (const window_method& method : image.methods) {
        int32_t rc = queue.write(method.method, method.values, method.count);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

// Whether giving each present window its head changes any window's owner in the armed state, a window with no head
// always counting as changed, as NVIDIA's modesetting module compares the owner field with the head it gives
static bool changes_window_owners(const display_heads& heads, uint32_t present_windows, const window_owners& owners) {
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool is_present = (present_windows & (1u << window)) != 0;
        if (is_present && owners.owners[window] != heads.window_head[window]) {
            return true;
        }
    }

    return false;
}

// The present windows serving `head`, bit i for window i
static uint32_t served_windows(const display_heads& heads, uint32_t present_windows, uint8_t head) {
    uint32_t windows = 0;
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool is_present = (present_windows & (1u << window)) != 0;
        if (is_present && heads.window_head[window] == head) {
            windows |= 1u << window;
        }
    }

    return windows;
}

// Counts the heads `read_active_heads` found showing a display
static uint8_t count_active_heads(const active_heads& active) {
    uint8_t count = 0;
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        if (active.heads[head].display_id != 0) {
            count++;
        }
    }

    return count;
}

// Powers down the link of each output in `dark_outputs` as `power_down` gives it until one fails. The boot firmware lit
// each of these links and the driver cannot tell their FEC, so each monitor is set to D3 after its link goes down
static int32_t power_down_links(
    firmware& fw,
    rm::handle display_common,
    const display_outputs& outputs,
    uint32_t dark_outputs,
    const power_down_facts& power_down
) {
    for (uint8_t index = 0; index < outputs.count; index++) {
        uint32_t bit = 1u << index;
        if ((dark_outputs & bit) == 0) {
            continue;
        }

        uint32_t display_id = outputs.outputs[index].display_id;
        bool sets_monitor_to_d3 = (power_down.d3_outputs & bit) != 0;
        log::info(
            "nvidia: output %u stays dark, so bring-up powers down its link, display id 0x%X's%s",
            index,
            display_id,
            sets_monitor_to_d3 ? "" : ", its monitor left out of D3 as no DPCD of revision 1.1 or later was read"
        );

        int32_t rc = power_down_link(fw, display_common, display_id, power_down.rates[index], sets_monitor_to_d3, true);
        if (rc != OK) {
            log::error(
                "nvidia: display id 0x%X's link is not powered down, nor is the link of any dark output after it, so "
                "bring-up takes the display engine down",
                display_id
            );
            return rc;
        }
    }

    return OK;
}

// Logs each active head's display id and its link's control as the core channel's armed state reads them, both 0 once
// the shutdown update took effect
static void log_heads_after_shutdown(const registers& regs, const active_heads& active) {
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        const active_head& state = active.heads[head];
        if (state.display_id == 0) {
            continue;
        }

        uint32_t display_id_method = rm::CORE_HEAD_DISPLAY_ID + head * rm::CORE_HEAD_METHODS_STRIDE;
        uint32_t link_method = rm::CORE_SERIAL_LINK_CONTROL + state.link_index * rm::CORE_SERIAL_LINK_METHODS_STRIDE;
        log::info(
            "nvidia: after the shutdown update, head %u's display id reads 0x%X and serial link %u's control 0x%X in "
            "the core channel's armed state",
            head,
            regs.read(CORE_ARMED_STATE + display_id_method),
            state.link_index,
            regs.read(CORE_ARMED_STATE + link_method)
        );
    }
}

// Logs the lit head's display id and its link's control as the core channel's armed state reads them once the light's
// update finished, which hold the light's display id and the lit head in the link's owner mask once it took effect
static void log_lit_head(const registers& regs, const head_light& light) {
    uint32_t display_id_method = rm::CORE_HEAD_DISPLAY_ID + light.head * rm::CORE_HEAD_METHODS_STRIDE;
    uint32_t link_method = rm::CORE_SERIAL_LINK_CONTROL + light.link.index * rm::CORE_SERIAL_LINK_METHODS_STRIDE;
    log::info(
        "nvidia: after the light, head %u's display id reads 0x%X and serial link %u's control 0x%X in the core "
        "channel's armed state",
        light.head,
        regs.read(CORE_ARMED_STATE + display_id_method),
        light.link.index,
        regs.read(CORE_ARMED_STATE + link_method)
    );
}

static void log_lit_color_controls(const registers& regs, const head_light& light, uint8_t window) {
    uint32_t head_state = CORE_ARMED_STATE + light.head * rm::CORE_HEAD_METHODS_STRIDE;
    log::info(
        "nvidia: after the light, head %u's output table control reads 0x%X and its OCSC0 control 0x%X in the core "
        "channel's armed state",
        light.head,
        regs.read(head_state + rm::CORE_HEAD_OUTPUT_TABLE_CONTROL),
        regs.read(head_state + rm::CORE_HEAD_OCSC0_CONTROL)
    );

    uint32_t window_state = window_channel_area(window) + WINDOW_CHANNEL_ARMED_STATE;
    log::info(
        "nvidia: after the light, window %u's size reads 0x%X, its CSC0 controls 0x%X, 0x%X and 0x%X and its CSC1 "
        "controls 0x%X and 0x%X in its channel's armed state",
        window,
        regs.read(window_state + rm::WINDOW_SET_SIZE),
        regs.read(window_state + rm::WINDOW_SET_CSC00_CONTROL),
        regs.read(window_state + rm::WINDOW_SET_CSC0_TABLE_CONTROL),
        regs.read(window_state + rm::WINDOW_SET_CSC01_CONTROL),
        regs.read(window_state + rm::WINDOW_SET_CSC10_CONTROL),
        regs.read(window_state + rm::WINDOW_SET_CSC1_TABLE_CONTROL)
    );
}

display_engine::display_engine(
    firmware& fw,
    video_memory& memory,
    operation_queue& operations,
    const failed_mark& failed
)
    : m_firmware(fw)
    , m_memory(memory)
    , m_instance_memory(memory, fw)
    , m_operations(operations)
    , m_failed(failed) {
    for (uint8_t& output : m_head_outputs) {
        output = NO_OUTPUT;
    }

    for (engine_head& head : m_heads) {
        head.engine = this;
        timer::init_deadline_timer(&head.latch_check_timer, check_latch);
        timer::init_deadline_timer(&head.deadline_timer, expire_deadline);
    }

    m_take_down_queue.init();
    m_hotplug_record.work = serve_hotplug_record;
    m_hotplug_record.engine = this;
    m_short_pulse_record.work = serve_short_pulse_record;
    m_short_pulse_record.engine = this;
}

int32_t display_engine::set_up(
    registers& regs,
    const dma::device_profile& card,
    const chip_info& chip,
    const gpu_objects& gpu
) {
    const chip_display& display = chip.display;
    m_registers = regs;

    // The boot firmware or an earlier driver can leave a head's interrupt enabled or pending
    m_head_count = display.head_count;
    for (uint8_t head = 0; head < display.head_count; head++) {
        disable_vertical_blank(regs, head);
    }

    uint32_t present_heads = 0;
    uint32_t present_windows = 0;
    int32_t rc = read_present_heads_and_windows(m_firmware, display, present_heads, present_windows);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the GSP's firmware reports windows 0x%X present, of the chip's %u windows",
        present_windows,
        display.window_count
    );
    log::info("nvidia: the GSP's firmware reports heads 0x%X present", present_heads);

    rc = m_instance_memory.set_up();
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the display's instance memory lies at 0x%lX in video memory, and the GSP's firmware accepted its "
        "address",
        m_instance_memory.range().offset
    );

    rc = allocate_display_objects(m_firmware, gpu.device, display, m_objects);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the GSP's firmware keeps the display engine's objects: display common 0x%X, the bandwidth check's "
        "object 0x%X and the display 0x%X",
        m_objects.display_common,
        m_objects.bandwidth_check,
        m_objects.display
    );

    m_firmware.set_event_sink(this);
    rc = subscribe_to_event(gpu.subdevice, rm::NOTIFIER_HOTPLUG, "hotplug", m_hotplug_event);
    if (rc != OK) {
        return rc;
    }

    rc = subscribe_to_event(gpu.subdevice, rm::NOTIFIER_SHORT_PULSE, "short pulse", m_short_pulse_event);
    if (rc != OK) {
        return rc;
    }

    bool has_crossbar = false;
    display_heads heads = {};
    display_outputs outputs = {};
    rc = read_display_facts(m_firmware, m_objects, display, has_crossbar, heads, outputs);
    if (rc != OK) {
        return rc;
    }

    string::memcpy(&m_outputs, &outputs, sizeof(m_outputs));

    bool fits_gfxfb = outputs.count <= gfxfb::GFXFB_OUTPUTS_MAX;
    m_output_count = fits_gfxfb ? outputs.count : static_cast<uint8_t>(gfxfb::GFXFB_OUTPUTS_MAX);

    rc = read_connected_monitors(
        m_firmware,
        m_objects.display_common,
        display.displayport,
        outputs,
        m_output_modes,
        m_power_down
    );
    if (rc != OK) {
        return rc;
    }

    rc = allocate_push_buffer(card, m_core_push_buffer);
    if (rc != dma::OK) {
        log::error("nvidia: allocating the core channel's push buffer failed: %d", rc);
        return ERR_NO_MEMORY;
    }

    rc = allocate_core_channel(m_firmware, m_objects.display, display, m_core_push_buffer.phys, m_core_channel);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the GSP's firmware keeps the core channel 0x%X, whose push buffer lies at physical address 0x%lX, "
        "and the channel's PUT and GET read 0x%X and 0x%X, offsets into the push buffer",
        m_core_channel,
        m_core_push_buffer.phys,
        regs.read(CORE_CHANNEL_AREA + CHANNEL_PUT),
        regs.read(CORE_CHANNEL_AREA + CHANNEL_GET)
    );

    rc = m_core_notifier.allocate(card);
    if (rc != OK) {
        return rc;
    }

    rm::handle notifier_context_dma = m_firmware.new_handle();
    context_dma_memory notifier_memory = {
        .in_video_memory = false,
        .address = m_core_notifier.physical_address(),
        .bytes = COMPLETION_NOTIFIER_BYTES,
    };

    rc = m_instance_memory.bind(notifier_context_dma, CORE_CHANNEL_NUMBER, notifier_memory);
    if (rc != OK) {
        return rc;
    }

    m_notifier_context_dma = notifier_context_dma;
    log::info(
        "nvidia: the core channel's completion notifier lies at physical address 0x%lX, and its context DMA 0x%X is "
        "bound to the core channel",
        notifier_memory.address,
        notifier_context_dma
    );

    uint32_t* push_buffer = reinterpret_cast<uint32_t*>(m_core_push_buffer.virt);
    m_core_queue = new (m_core_queue_bytes) display_channel(push_buffer, regs.at(CORE_CHANNEL_AREA));

    uint32_t written = 0;
    rc = write_core_initial_methods(*m_core_queue, heads.count, written);
    if (rc != OK) {
        log::error("nvidia: writing the core channel's initial methods failed after %u of them: %d", written, rc);
        return rc;
    }

    m_core_queue->kick();
    rc = m_core_queue->wait_idle(clock::now_ns() + IDLE_TIMEOUT_NS);
    if (rc != OK) {
        log::error(
            "nvidia: the core channel's GET did not reach PUT once its %u initial methods were kicked: %d",
            written,
            rc
        );
        return rc;
    }

    log::info(
        "nvidia: the core channel took its %u initial methods, kicked with no update, and its PUT and GET read 0x%X "
        "and 0x%X, offsets into the push buffer",
        written,
        regs.read(CORE_CHANNEL_AREA + CHANNEL_PUT),
        regs.read(CORE_CHANNEL_AREA + CHANNEL_GET)
    );

    active_heads active = {};
    rc = read_active_heads(
        m_firmware,
        m_objects.display_common,
        heads.count,
        has_crossbar,
        display.serial_link_count,
        active
    );
    if (rc != OK) {
        return rc;
    }

    log_active_heads(active, outputs);
    m_boot_outputs = find_shown_outputs(active, outputs);

    light_set set = {};
    rc = choose_and_log_lights(m_firmware, m_objects.bandwidth_check, heads, active, outputs, m_output_modes, set);

    dpcd_capabilities dpcds[DISPLAY_HEADS_MAX] = {};
    for (uint8_t index = 0; index < set.count; index++) {
        string::memcpy(&dpcds[index], &m_output_modes[set.lights[index].output].dpcd, sizeof(dpcds[index]));
    }

    free_output_modes(m_output_modes);
    if (rc != OK || set.count == 0) {
        return rc;
    }

    uint8_t chosen_count = set.count;
    rc = prepare_lights(regs, card, display, present_heads, present_windows, set, dpcds);
    if (rc != OK || set.count == 0) {
        return rc;
    }

    bool is_core_channel_idle = false;
    rc = wait_core_channel_idle(m_firmware, m_objects.display, display, is_core_channel_idle);
    if (rc != OK) {
        return rc;
    }

    log_core_channel_idle(is_core_channel_idle);

    window_owners owners = read_window_owners(regs, present_windows);
    log_window_owners(owners, present_windows, display.head_count);

    return bring_up(
        regs,
        heads,
        present_windows,
        owners,
        active,
        has_crossbar,
        outputs,
        m_power_down,
        set,
        dpcds,
        set.count != chosen_count
    );
}

// Removes the light at `index` from `set` and its capabilities from `dpcds`, keeping the others in lighting order
static void drop_light(light_set& set, dpcd_capabilities* dpcds, uint8_t index) {
    for (uint8_t next = index + 1; next < set.count; next++) {
        set.lights[next - 1] = set.lights[next];
        string::memcpy(&dpcds[next - 1], &dpcds[next], sizeof(dpcds[next]));
    }

    set.count--;
}

// Allocates and binds `head`'s window notifier. Only presents need it, so a shortage leaves the head lit with presents
// on its output unsupported
static int32_t set_up_window_notifier(
    firmware& fw,
    instance_memory& memory,
    const dma::device_profile& card,
    engine_head& head
) {
    int32_t rc = head.notifier.allocate(card);
    if (rc == OK) {
        for (uint32_t slot = 0; slot < NOTIFIER_SLOTS; slot++) {
            head.notifier.clear(slot);
        }

        rm::handle context_dma = fw.new_handle();
        context_dma_memory notifier_memory = {
            .in_video_memory = false,
            .address = head.notifier.physical_address(),
            .bytes = WINDOW_NOTIFIER_BYTES,
        };

        rc = memory.bind(context_dma, window_channel_number(head.window), notifier_memory);
        if (rc == OK) {
            head.notifier_context_dma = context_dma;
            log::info(
                "nvidia: window %u's notifier lies at physical address 0x%lX, and its context DMA 0x%X is bound to "
                "the window's channel",
                head.window,
                notifier_memory.address,
                context_dma
            );
        }
    }

    if (rc == ERR_NO_MEMORY) {
        log::warn("nvidia: window %u has no notifier, so presents on its output are unsupported", head.window);
        return OK;
    }

    return rc;
}

int32_t display_engine::prepare_lights(
    registers& regs,
    const dma::device_profile& card,
    const chip_display& display,
    uint32_t present_heads,
    uint32_t present_windows,
    light_set& set,
    dpcd_capabilities* dpcds
) {
    light_set prepared = {};
    uint8_t index = 0;
    while (index < set.count) {
        const output_light& light = set.lights[index];
        bool is_prepared = false;
        int32_t rc = prepare_light(regs, card, display, present_windows, prepared, light, is_prepared);
        if (rc != OK) {
            return rc;
        }

        if (!is_prepared) {
            log::error(
                "nvidia: output %u is left out of the mode set, as its window channel or a surface it scans out is not "
                "set up",
                light.output
            );
            drop_light(set, dpcds, index);
            continue;
        }

        prepared.lights[prepared.count++] = light;
        rc = set_up_window_notifier(m_firmware, m_instance_memory, card, m_heads[light.head]);
        if (rc == OK) {
            rc = set_up_cursor(regs, display, present_heads, light);
        }

        if (rc != OK) {
            return rc;
        }

        index++;
    }

    return OK;
}

// The memory `head`'s base buffer lies in, as a context DMA names it
static context_dma_memory base_buffer_memory(const engine_head& head) {
    return {
        .in_video_memory = true,
        .address = head.base_buffer.offset,
        .bytes = head.base_buffer.size,
    };
}

// Records in `made` that the surface `object` names, `holder`'s base buffer or the table when null, is bound to
// `window`'s channel
static void record_binding(light_bindings& made, rm::handle object, uint8_t window, engine_head* holder) {
    made.objects[made.count] = object;
    made.windows[made.count] = window;
    made.holders[made.count] = holder;
    made.count++;
}

// Binds the surface `object` names, lying in `memory`, to `window`'s channel and records the binding in `made`, and in
// the mask of `holder` when that head's base buffer is the surface
static int32_t bind_for_light(
    instance_memory& instances,
    rm::handle object,
    const context_dma_memory& memory,
    uint8_t window,
    engine_head* holder,
    light_bindings& made
) {
    int32_t rc = instances.bind(object, window_channel_number(window), memory);
    if (rc != OK) {
        return rc;
    }

    if (holder != nullptr) {
        holder->base_buffer_windows |= 1u << window;
    }

    record_binding(made, object, window, holder);
    return OK;
}

// Takes down the userland mapping of `head`'s base buffer and undoes `made`'s bindings, last first, then releases the
// buffer unless an undoing fails, as the display engine may read the memory through a binding left
static void drop_base_buffer(
    instance_memory& instances,
    video_memory& memory,
    engine_head& head,
    const light_bindings& made
) {
    if (head.userland_base_buffer.size != 0) {
        memory.unmap_for_cpu(head.base_buffer);
        head.userland_base_buffer = {};
    }

    for (uint8_t index = made.count; index > 0; index--) {
        rm::handle object = made.objects[index - 1];
        uint8_t window = made.windows[index - 1];
        engine_head* holder = made.holders[index - 1];
        int32_t rc = instances.unbind(object, window_channel_number(window));
        if (rc != OK) {
            log::error(
                "nvidia: output %u's base buffer stays allocated, as undoing context DMA 0x%X's binding to window %u's "
                "channel failed: %d",
                head.output,
                object,
                window,
                rc
            );
            return;
        }

        if (holder != nullptr) {
            holder->base_buffer_windows &= ~(1u << window);
        }
    }

    memory.release(head.base_buffer);
    head.base_buffer = {};
    head.base_buffer_context_dma = 0;
    log::info("nvidia: output %u's base buffer is unbound and released", head.output);
}

int32_t display_engine::prepare_light(
    registers& regs,
    const dma::device_profile& card,
    const chip_display& display,
    uint32_t present_windows,
    const light_set& prepared,
    const output_light& light,
    bool& is_prepared
) {
    is_prepared = false;
    engine_head& head = m_heads[light.head];
    head.window = light.window;
    head.output = light.output;

    int32_t rc = set_up_window_channel(
        m_firmware,
        regs,
        card,
        m_objects.display,
        display,
        present_windows,
        light.window,
        head.window_push_buffer,
        head.window_channel
    );
    if (rc != OK || head.window_channel == 0) {
        return rc;
    }

    uint32_t* window_push_buffer = reinterpret_cast<uint32_t*>(head.window_push_buffer.virt);
    registers window_area = regs.at(window_channel_area(light.window));
    head.window_queue = new (head.window_queue_bytes) display_channel(window_push_buffer, window_area);

    rc = set_up_base_buffer(m_memory, light, head.base_buffer);
    if (rc != OK || head.base_buffer.size == 0) {
        return rc;
    }

    rc = bind_base_buffer(
        m_firmware,
        m_instance_memory,
        m_memory,
        light,
        head.base_buffer,
        head.base_buffer_context_dma
    );
    if (rc != OK || head.base_buffer.size == 0) {
        return rc;
    }

    light_bindings made = {};
    head.base_buffer_windows |= 1u << light.window;
    record_binding(made, head.base_buffer_context_dma, light.window, &head);

    rc = map_base_buffer_for_userland(m_memory, light, head.base_buffer, head.userland_base_buffer);
    if (rc != OK || head.userland_base_buffer.size == 0) {
        drop_base_buffer(m_instance_memory, m_memory, head, made);
        return rc;
    }

    head.mode = {
        .width              = light.timing.width,
        .height             = light.timing.height,
        .refresh_millihertz = light.timing.refresh_millihertz(),
    };

    // The first light sets up the one look-up table surface, which every later light binds to its own window's channel
    if (prepared.count == 0) {
        rc = set_up_look_up_table(m_memory, m_look_up_table);
        if (rc == OK && m_look_up_table.size != 0) {
            rc = bind_look_up_table(
                m_firmware,
                m_instance_memory,
                m_memory,
                light.window,
                m_look_up_table,
                m_look_up_table_context_dma
            );
        }

        if (rc != OK || m_look_up_table.size == 0) {
            drop_base_buffer(m_instance_memory, m_memory, head, made);
            return rc;
        }

        is_prepared = true;
        return OK;
    }

    context_dma_memory table_memory = {
        .in_video_memory = true,
        .address = m_look_up_table.offset,
        .bytes = m_look_up_table.size,
    };

    rc = bind_for_light(m_instance_memory, m_look_up_table_context_dma, table_memory, light.window, nullptr, made);

    // gfxfb's `present()` may hand any base buffer to any output at its mode, so each is bound to every chosen
    // window's channel
    for (uint8_t index = 0; rc == OK && index < prepared.count; index++) {
        engine_head& other = m_heads[prepared.lights[index].head];
        rc = bind_for_light(
            m_instance_memory,
            head.base_buffer_context_dma,
            base_buffer_memory(head),
            other.window,
            &head,
            made
        );
        if (rc == OK) {
            rc = bind_for_light(
                m_instance_memory,
                other.base_buffer_context_dma,
                base_buffer_memory(other),
                light.window,
                &other,
                made
            );
        }
    }

    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: output %u cannot light, as binding the look-up table surface and the base buffers to the chosen "
            "outputs' window channels found no room",
            light.output
        );
    } else if (rc != OK) {
        log::error(
            "nvidia: binding the look-up table surface and the base buffers to the chosen outputs' window channels for "
            "output %u failed: %d",
            light.output,
            rc
        );
    }

    if (rc != OK) {
        drop_base_buffer(m_instance_memory, m_memory, head, made);
        return rc == ERR_NO_MEMORY ? OK : rc;
    }

    is_prepared = true;
    return OK;
}

// How a log line names the display ids of `display_ids`, one or several
static const char* display_ids_noun(uint32_t display_ids) {
    return __builtin_popcount(display_ids) == 1 ? "display id" : "display ids";
}

// The display ids of the outputs of `set`, each a single bit
static uint32_t light_set_display_ids(const light_set& set, const display_outputs& outputs) {
    uint32_t display_ids = 0;
    for (uint8_t index = 0; index < set.count; index++) {
        display_ids |= outputs.outputs[set.lights[index].output].display_id;
    }

    return display_ids;
}

// A bit for each output of `set`, bit 0 output 0's
static uint32_t light_set_outputs(const light_set& set) {
    uint32_t bits = 0;
    for (uint8_t index = 0; index < set.count; index++) {
        bits |= 1u << set.lights[index].output;
    }

    return bits;
}

int32_t display_engine::bring_up(
    registers& regs,
    const display_heads& heads,
    uint32_t present_windows,
    const window_owners& owners,
    const active_heads& active,
    bool has_crossbar,
    const display_outputs& outputs,
    const power_down_facts& power_down,
    light_set& set,
    const dpcd_capabilities* dpcds,
    bool is_set_reduced
) {
    rm::handle display_common = m_objects.display_common;
    uint32_t started_ids = light_set_display_ids(set, outputs);
    int32_t rc = send_display_change(m_firmware, display_common, started_ids, rm::DISPLAY_CHANGE_START);
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused the start of the mode set naming %s 0x%X",
            display_ids_noun(started_ids),
            started_ids
        );
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: display common took the start of the mode set naming %s 0x%X",
        display_ids_noun(started_ids),
        started_ids
    );
    int32_t link_rc = OK;
    head_light lights[DISPLAY_HEADS_MAX] = {};
    uint8_t ready_count = 0;
    rc = count_active_heads(active) > 0 ? shut_down_heads(regs, heads, present_windows, active) : OK;
    if (rc != OK) {
        log::error("nvidia: the shutdown update failed: %d, so bring-up takes the display engine down", rc);
    } else {
        // The set's outputs have their links trained again at once, so they are not powered down
        uint32_t dark_boot_outputs = m_boot_outputs & ~light_set_outputs(set);
        link_rc = power_down_links(m_firmware, display_common, outputs, dark_boot_outputs, power_down);
        if (link_rc == OK) {
            uint8_t asked_count = set.count;
            link_rc = set_up_links(has_crossbar, outputs, power_down, set, dpcds, lights, ready_count);
            is_set_reduced = is_set_reduced || ready_count != asked_count;
        }
    }

    active_heads lit_heads = {};
    for (uint8_t index = 0; index < ready_count; index++) {
        lights[index].windows = served_windows(heads, present_windows, lights[index].head);
        lit_heads.heads[lights[index].head] = {
            .display_id = lights[index].display_id,
            .link_index = lights[index].link.index,
            .link_type  = rm::OUTPUT_RESOURCE_TYPE_SERIAL,
        };
    }

    // The firmware accepted the set as it was asked, so a set that lost an output is asked again
    if (link_rc == OK && ready_count > 0 && is_set_reduced) {
        bool is_possible = false;
        const char* noun = ready_count == 1 ? "output" : "outputs";
        rc = check_light_set(m_firmware, m_objects.bandwidth_check, set, is_possible);
        if (rc != OK) {
            log::error(
                "nvidia: the bandwidth check gives no answer about the %u %s left, so bring-up takes the display "
                "engine down",
                ready_count,
                noun
            );
        } else if (!is_possible) {
            log::error(
                "nvidia: the bandwidth check does not accept the %u %s left, so bring-up takes the display engine down",
                ready_count,
                noun
            );
        }

        if (rc != OK || !is_possible) {
            link_rc = rc;
            ready_count = 0;
        }
    }

    bool is_lit = false;
    bool shows_cursor_memory[DISPLAY_HEADS_MAX] = {};
    if (link_rc == OK && ready_count > 0) {
        rc = light_heads(regs, heads, present_windows, owners, lights, ready_count, shows_cursor_memory);
        is_lit = rc == OK;
        if (!is_lit && ready_count == 1) {
            log::error(
                "nvidia: lighting head %u failed: %d, so bring-up takes the display engine down",
                lights[0].head,
                rc
            );
        } else if (!is_lit) {
            log::error(
                "nvidia: lighting the %u heads failed: %d, so bring-up takes the display engine down",
                ready_count,
                rc
            );
        }
    }

    uint32_t lit_ids = 0;
    if (is_lit) {
        for (uint8_t index = 0; index < ready_count; index++) {
            mark_head_lit(regs, lights[index], shows_cursor_memory[index]);
            lit_ids |= lights[index].display_id;
        }
    } else {
        rc = take_down(heads, present_windows, active, lit_heads);
        if (rc != OK) {
            log::error("nvidia: taking the display engine down failed: %d, so its heads may still scan out", rc);
        }
    }

    // NVIDIA's end names the set its start named, which a set that lost outputs no longer is, so the end names the
    // displays lit, or the start's when none lit
    uint32_t ended_ids = is_lit ? lit_ids : started_ids;
    rc = send_display_change(m_firmware, display_common, ended_ids, rm::DISPLAY_CHANGE_END);
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused the end of the mode set naming %s 0x%X",
            display_ids_noun(ended_ids),
            ended_ids
        );
        return link_rc;
    }

    if (rc != OK) {
        return link_rc != OK ? link_rc : rc;
    }

    log::info(
        "nvidia: display common took the end of the mode set naming %s 0x%X",
        display_ids_noun(ended_ids),
        ended_ids
    );
    return link_rc;
}

int32_t display_engine::set_up_links(
    bool has_crossbar,
    const display_outputs& outputs,
    const power_down_facts& power_down,
    light_set& set,
    const dpcd_capabilities* dpcds,
    head_light* lights,
    uint8_t& ready_count
) {
    ready_count = 0;
    uint8_t assigned_links = 0;
    for (uint8_t index = 0; index < set.count; index++) {
        const output_light light = set.lights[index];
        const display_output& output = outputs.outputs[light.output];
        uint8_t links_before = assigned_links;

        output_link link = {};
        bool is_set_up = false;
        int32_t rc = set_up_output_link(
            m_firmware,
            m_objects.display_common,
            has_crossbar,
            output,
            dpcds[index],
            light,
            assigned_links,
            link,
            is_set_up
        );
        if (rc != OK) {
            log::error(
                "nvidia: display id 0x%X's link is not set up, so bring-up takes the display engine down",
                output.display_id
            );
            set.count = ready_count;
            return rc;
        }

        if (is_set_up) {
            set.lights[ready_count] = light;
            lights[ready_count] = {
                .head                     = light.head,
                .windows                  = 0,
                .timing                   = light.timing,
                .display_id               = output.display_id,
                .link                     = link,
                .output_table_context_dma = m_look_up_table_context_dma,
            };

            ready_count++;
            continue;
        }

        bool leaves_none_set_up = ready_count == 0 && index + 1 == set.count;
        if (leaves_none_set_up) {
            log::error(
                "nvidia: display id 0x%X's link is not set up, so bring-up takes the display engine down",
                output.display_id
            );
        } else {
            log::error(
                "nvidia: display id 0x%X's link is not set up, so output %u stays dark",
                output.display_id,
                light.output
            );
        }

        // The boot firmware's assignment still drives a boot output's link, so it is powered down unassigned too, and
        // as the driver leaves FEC off on the links it trains, only a boot output's link takes the FEC order
        uint32_t bit = 1u << light.output;
        bool is_boot_output = (m_boot_outputs & bit) != 0;
        if (assigned_links != links_before || is_boot_output) {
            bool sets_monitor_to_d3 = (power_down.d3_outputs & bit) != 0;
            rc = power_down_link(
                m_firmware,
                m_objects.display_common,
                output.display_id,
                power_down.rates[light.output],
                sets_monitor_to_d3,
                is_boot_output
            );
            if (rc != OK) {
                log::error(
                    "nvidia: display id 0x%X's link is not powered down, so bring-up takes the display engine down",
                    output.display_id
                );
                set.count = ready_count;
                return rc;
            }
        }

        drop_prepared_light(light);
    }

    set.count = ready_count;
    return OK;
}

void display_engine::drop_prepared_light(const output_light& dropped) {
    engine_head& head = m_heads[dropped.head];
    light_bindings made = {};
    record_binding(made, head.base_buffer_context_dma, dropped.window, &head);
    record_binding(made, m_look_up_table_context_dma, dropped.window, nullptr);
    for (uint8_t window = 0; window < DISPLAY_WINDOWS_MAX; window++) {
        bool binds_other_window = window != dropped.window && (head.base_buffer_windows & (1u << window)) != 0;
        if (binds_other_window) {
            record_binding(made, head.base_buffer_context_dma, window, &head);
        }
    }

    for (engine_head& other : m_heads) {
        bool binds_dropped_window = &other != &head && (other.base_buffer_windows & (1u << dropped.window)) != 0;
        if (binds_dropped_window) {
            record_binding(made, other.base_buffer_context_dma, dropped.window, &other);
        }
    }

    drop_base_buffer(m_instance_memory, m_memory, head, made);
}

int32_t display_engine::send_notified_update(uint32_t window_interlocks, uint32_t& status_word) {
    m_core_notifier.clear();
    int32_t rc = write_update(*m_core_queue, m_notifier_context_dma, window_interlocks);
    if (rc != OK) {
        return rc;
    }

    m_has_sent_update = true;
    kick_window_queues(window_interlocks);
    if (m_update_seam != nullptr) {
        m_update_seam(*this);
    }

    rc = m_core_notifier.wait_finished(*m_core_queue, clock::now_ns() + m_notifier_wait_ns);
    if (rc != OK) {
        return rc;
    }

    status_word = m_core_notifier.status_word();

    return stop_notifier_writes(*m_core_queue);
}

int32_t display_engine::shut_down_heads(
    const registers& regs,
    const display_heads& heads,
    uint32_t present_windows,
    const active_heads& active
) {
    shutdown_methods methods = {};
    build_shutdown_methods(heads, present_windows, active, true, methods);

    int32_t rc = write_methods(*m_core_queue, methods.methods, methods.count);
    if (rc != OK) {
        return rc;
    }

    uint32_t status_word = 0;
    rc = send_notified_update(NO_WINDOW_INTERLOCKS, status_word);
    if (rc != OK) {
        return rc;
    }

    uint8_t active_count = count_active_heads(active);
    log::info(
        "nvidia: the core channel read the shutdown update for %u active %s, its completion notifier reading 0x%08X",
        active_count,
        active_count == 1 ? "head" : "heads",
        status_word
    );
    log_heads_after_shutdown(regs, active);

    return OK;
}

void display_engine::kick_window_queues(uint32_t window_interlocks) {
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        const engine_head& state = m_heads[head];
        bool is_named = state.window_queue != nullptr && (window_interlocks & (1u << state.window)) != 0;
        if (is_named) {
            state.window_queue->kick();
        }
    }
}

int32_t display_engine::set_up_cursor(
    registers& regs,
    const chip_display& display,
    uint32_t present_heads,
    const output_light& light
) {
    engine_head& head = m_heads[light.head];
    int32_t rc = head.cursor.allocate(m_firmware, regs, m_objects.display, display, light.head, present_heads);
    if (rc == ERR_INVALID_ARG) {
        log::error("nvidia: head %u has no cursor, as the GSP's firmware does not report the head present", light.head);
        return OK;
    }

    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: head %u has no cursor, as the GSP's firmware refused a request for its channel",
            light.head
        );
        return OK;
    }

    if (rc == ERR_NO_MEMORY) {
        log::error("nvidia: head %u has no cursor, as a request for its channel cannot be allocated", light.head);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    vram_range memory = {};
    rc = m_memory.allocate(CURSOR_MEMORY_BYTES, SURFACE_ALIGNMENT, vram_use::scanout, memory);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: head %u has no cursor, as 0x%lX bytes of video memory for output %u's cursor cannot be allocated",
            light.head,
            CURSOR_MEMORY_BYTES,
            light.output
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: allocating 0x%lX bytes of video memory for output %u's cursor failed: %d",
            CURSOR_MEMORY_BYTES,
            light.output,
            rc
        );
        return rc;
    }

    cpu_mapping mapping = {};
    rc = m_memory.map_for_cpu(memory, mapping);
    if (rc == ERR_NO_MEMORY) {
        m_memory.release(memory);
        log::error(
            "nvidia: head %u has no cursor, as mapping output %u's cursor memory for the CPU found no room, so the "
            "memory is released",
            light.head,
            light.output
        );
        return OK;
    }

    if (rc != OK) {
        m_memory.release(memory);
        log::error(
            "nvidia: mapping output %u's cursor memory at 0x%lX in video memory for the CPU failed: %d, so the memory "
            "is released",
            light.output,
            memory.offset,
            rc
        );
        return rc;
    }

    rm::handle context_dma = m_firmware.new_handle();
    context_dma_memory cursor_memory = {
        .in_video_memory = true,
        .address = memory.offset,
        .bytes = memory.size,
    };

    rc = m_instance_memory.bind(context_dma, CORE_CHANNEL_NUMBER, cursor_memory);
    if (rc == ERR_NO_MEMORY) {
        m_memory.release(memory);
        log::error(
            "nvidia: head %u has no cursor, as binding output %u's cursor memory to the core channel found no room, so "
            "the memory is released",
            light.head,
            light.output
        );
        return OK;
    }

    if (rc != OK) {
        m_memory.release(memory);
        log::error(
            "nvidia: binding output %u's cursor memory to the core channel failed: %d, so the memory is released",
            light.output,
            rc
        );
        return rc;
    }

    head.cursor_memory = memory;
    head.cursor_mapping = mapping;
    head.cursor_context_dma = context_dma;
    log::info(
        "nvidia: head %u has a cursor channel, and output %u's cursor memory lies at 0x%lX in video memory, mapped for "
        "the CPU and bound to the core channel as context DMA 0x%X",
        light.head,
        light.output,
        memory.offset,
        context_dma
    );

    return OK;
}

int32_t display_engine::prepare_cursor(uint8_t head, rm::handle& cursor_context_dma) {
    cursor_context_dma = 0;
    engine_head& state = m_heads[head];
    if (state.cursor_context_dma == 0) {
        return OK;
    }

    for (uint64_t offset = 0; offset < state.cursor_memory.size; offset += sizeof(uint64_t)) {
        mmio::write64(state.cursor_mapping.virt + offset, 0);
    }

    // The read back through BAR1 completes the clear's writes before an update can show the memory
    barrier::dma_full();
    mmio::read64(state.cursor_mapping.virt);

    int32_t rc = write_when_free(
        state.cursor,
        cursor_channel::HOT_SPOT_POINT,
        PARKED_CURSOR_POSITION,
        m_cursor_wait_ns
    );
    if (rc == OK) {
        rc = write_when_free(state.cursor, cursor_channel::UPDATE, 0, m_cursor_wait_ns);
    }

    if (rc == ERR_QUEUE_FULL) {
        log::error("nvidia: head %u has no cursor, as its cursor channel found no room to park it", head);
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: parking head %u's cursor failed, as its cursor channel's free register reads all ones",
            head
        );
        return rc;
    }

    cursor_context_dma = state.cursor_context_dma;
    return OK;
}

int32_t display_engine::write_first_image(
    const head_light& light,
    rm::handle cursor_context_dma,
    uint32_t window_interlocks
) {
    int32_t rc = write_cursor_methods(*m_core_queue, light.head, cursor_context_dma);
    if (rc != OK) {
        return rc;
    }

    const engine_head& head = m_heads[light.head];
    window_usage_methods usage = {};
    build_window_usage_methods(head.window, light.timing.width, usage);

    rc = write_methods(*m_core_queue, usage.methods, WINDOW_USAGE_METHODS);
    if (rc != OK) {
        return rc;
    }

    const edid::timing& timing = light.timing;
    window_image image = {
        .window                    = head.window,
        .surface_context_dma       = head.base_buffer_context_dma,
        .width                     = timing.width,
        .height                    = timing.height,
        .pitch                     = base_buffer_pitch(timing.width),
        .look_up_table_context_dma = m_look_up_table_context_dma,
    };

    window_image_update update = {
        .notifier_context_dma = 0,
        .notifier_slot        = 0,
        .interlocks_core      = true,
        .window_interlocks    = window_interlocks,
    };

    window_image_methods methods = {};
    build_window_image_methods(image, update, methods);

    return write_window_methods(*head.window_queue, methods);
}

int32_t display_engine::write_first_images(
    const head_light* lights,
    const rm::handle* cursor_context_dmas,
    uint8_t count,
    uint32_t window_interlocks
) {
    for (uint8_t index = 0; index < count; index++) {
        int32_t rc = write_first_image(lights[index], cursor_context_dmas[index], window_interlocks);
        if (rc != OK) {
            return rc;
        }
    }

    return OK;
}

int32_t display_engine::light_heads(
    const registers& regs,
    const display_heads& heads,
    uint32_t present_windows,
    const window_owners& owners,
    const head_light* lights,
    uint8_t count,
    bool* shows_cursor_memory
) {
    rm::handle cursor_context_dmas[DISPLAY_HEADS_MAX] = {};
    for (uint8_t index = 0; index < count; index++) {
        int32_t rc = prepare_cursor(lights[index].head, cursor_context_dmas[index]);
        if (rc != OK) {
            return rc;
        }

        shows_cursor_memory[index] = cursor_context_dmas[index] != 0;
    }

    bool is_first_update = !m_has_sent_update;
    bool is_image_apart = is_first_update && changes_window_owners(heads, present_windows, owners);
    uint32_t window_interlocks = 0;
    for (uint8_t index = 0; index < count; index++) {
        window_interlocks |= 1u << m_heads[lights[index].head].window;
    }

    if (is_first_update) {
        // The first update since the channel's allocation gives every present window its head, with no head to stop
        shutdown_methods mapping = {};
        build_shutdown_methods(heads, present_windows, {}, true, mapping);

        int32_t rc = write_methods(*m_core_queue, mapping.methods, mapping.count);
        if (rc != OK) {
            return rc;
        }
    }

    for (uint8_t index = 0; index < count; index++) {
        head_light_methods methods = {};
        build_head_light_methods(lights[index], methods);

        int32_t rc = write_methods(*m_core_queue, methods.methods, methods.count);
        if (rc != OK) {
            return rc;
        }
    }

    if (is_image_apart) {
        // An update showing no window image shows no cursor either
        for (uint8_t index = 0; index < count; index++) {
            int32_t rc = write_cursor_methods(*m_core_queue, lights[index].head, 0);
            if (rc != OK) {
                return rc;
            }
        }
    } else {
        int32_t rc = write_first_images(lights, cursor_context_dmas, count, window_interlocks);
        if (rc != OK) {
            return rc;
        }
    }

    uint32_t status_word = 0;
    int32_t rc = send_notified_update(is_image_apart ? NO_WINDOW_INTERLOCKS : window_interlocks, status_word);
    if (rc != OK) {
        return rc;
    }

    for (uint8_t index = 0; index < count; index++) {
        log::info(
            "nvidia: the core channel read the mode set's update, lighting head %u, its completion notifier reading "
            "0x%08X",
            lights[index].head,
            status_word
        );
        log_lit_head(regs, lights[index]);
        log_lit_color_controls(regs, lights[index], m_heads[lights[index].head].window);
    }

    if (!is_image_apart) {
        return OK;
    }

    rc = write_first_images(lights, cursor_context_dmas, count, window_interlocks);
    if (rc != OK) {
        return rc;
    }

    rc = write_update(*m_core_queue, 0, window_interlocks);
    if (rc != OK) {
        return rc;
    }

    kick_window_queues(window_interlocks);
    for (uint8_t index = 0; index < count; index++) {
        log::info(
            "nvidia: window %u's first image follows in the update after the light's, naming no notifier, since the "
            "update lighting head %u changed a window's owner or a present window serves no head",
            m_heads[lights[index].head].window,
            lights[index].head
        );
    }

    return OK;
}

int32_t display_engine::take_down(
    const display_heads& heads,
    uint32_t present_windows,
    const active_heads& active,
    const active_heads& lit
) {
    shutdown_methods methods = {};
    build_shutdown_methods(heads, present_windows, active, !m_has_sent_update, methods);

    int32_t rc = write_methods(*m_core_queue, methods.methods, methods.count);
    if (rc != OK) {
        return rc;
    }

    build_shutdown_methods(heads, present_windows, lit, false, methods);
    rc = write_methods(*m_core_queue, methods.methods, methods.count);
    if (rc != OK) {
        return rc;
    }

    rc = write_update(*m_core_queue, 0, NO_WINDOW_INTERLOCKS);
    if (rc != OK) {
        return rc;
    }

    m_has_sent_update = true;
    rc = m_core_queue->wait_idle(clock::now_ns() + IDLE_TIMEOUT_NS);
    if (rc != OK) {
        return rc;
    }

    log::info("nvidia: the display engine is taken down, its update read by the core channel");
    return OK;
}

int32_t display_engine::release() {
    m_firmware.set_event_sink(nullptr);

    // Nothing ends a present once the engine is released, so each queued one ends here, failed, and a timer callback
    // already running finds none
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        fail_queued_presents();
        for (engine_head& head : m_heads) {
            head.is_lit = false;
        }

        m_is_lit = false;
        m_is_released = true;
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    m_core_queue = nullptr;
    m_registers = registers(0);
    free_output_modes(m_output_modes);

    int32_t cursor_channel_rc = OK;
    for (engine_head& head : m_heads) {
        int32_t rc = head.cursor.release(m_firmware);
        cursor_channel_rc = cursor_channel_rc != OK ? cursor_channel_rc : rc;
    }

    bool keeps_hardware = !m_has_sent_update;
    int32_t window_channel_rc = OK;
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        engine_head& state = m_heads[head];
        state.window_queue = nullptr;
        if (state.window_channel == 0) {
            continue;
        }

        int32_t rc = release_display_channel(m_firmware, m_objects.display, state.window_channel, keeps_hardware);
        window_channel_rc = window_channel_rc != OK ? window_channel_rc : rc;
        state.window_channel = 0;
    }

    int32_t core_channel_rc = OK;
    if (m_core_channel != 0) {
        core_channel_rc = release_display_channel(m_firmware, m_objects.display, m_core_channel, keeps_hardware);
        m_core_channel = 0;
    }

    int32_t events_rc = OK;
    rm::handle* events[] = {&m_hotplug_event, &m_short_pulse_event};
    for (rm::handle* event : events) {
        if (*event == 0) {
            continue;
        }

        int32_t rc = m_firmware.release(*event);
        events_rc = events_rc != OK ? events_rc : rc;
        *event = 0;
    }

    int32_t objects_rc = OK;
    if (m_objects.display_common != 0) {
        objects_rc = release_display_objects(m_firmware, m_objects);
        m_objects = {};
    }

    if (cursor_channel_rc != OK) {
        return cursor_channel_rc;
    }

    if (window_channel_rc != OK) {
        return window_channel_rc;
    }

    if (core_channel_rc != OK) {
        return core_channel_rc;
    }

    if (events_rc != OK) {
        return events_rc;
    }

    return objects_rc;
}

int32_t display_engine::subscribe_to_event(
    rm::handle subdevice,
    uint32_t notifier,
    const char* name,
    rm::handle& event
) {
    rm::event_params params = {};
    params.parent_client = m_firmware.client();
    params.class_id = rm::CLASS_EVENT;
    params.notify_index = notifier | rm::EVENT_POSTED_TO_DRIVER;

    rm::handle object = m_firmware.new_handle();
    int32_t rc = m_firmware.allocate(subdevice, object, rm::CLASS_EVENT, &params, sizeof(params));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: the GSP's firmware refused the %s event's object, so that event stays off", name);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    event = object;

    rm::event_notification_params arming = {};
    arming.notifier = notifier;
    arming.action = rm::EVENT_NOTIFICATION_REPEAT;
    rc = m_firmware.control(subdevice, rm::CONTROL_SET_EVENT_NOTIFICATION, &arming, sizeof(arming));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: the GSP's firmware refused to arm the %s notifier, so that event stays off", name);
        return OK;
    }

    return rc;
}

static uint32_t display_ids_of(const display_outputs& outputs) {
    uint32_t display_ids = 0;
    for (uint8_t output = 0; output < outputs.count; output++) {
        display_ids |= outputs.outputs[output].display_id;
    }

    return display_ids;
}

// Whether `event` is for `object`, by the notifier when it stands for the notify list, never for an object of 0
static bool is_event_for(const firmware_event& event, rm::handle object, uint32_t notifier) {
    if (object == 0) {
        return false;
    }

    return event.is_for_notify_list ? event.notifier == notifier : event.event == object;
}

bool display_engine::on_firmware_event(const firmware_event& event) {
    if (event.client != m_firmware.client()) {
        return false;
    }

    // A notify list's event names one of the event objects too, since NVIDIA finds the list through it
    bool names_an_event_object = event.event == m_hotplug_event || event.event == m_short_pulse_event;
    if (event.event == 0 || !names_an_event_object) {
        return false;
    }

    event_record* record = nullptr;
    if (is_event_for(event, m_hotplug_event, rm::NOTIFIER_HOTPLUG)) {
        record = &m_hotplug_record;
    } else if (is_event_for(event, m_short_pulse_event, rm::NOTIFIER_SHORT_PULSE)) {
        record = &m_short_pulse_record;
    }

    if (record == nullptr) {
        return false;
    }

    RUN_ELEVATED(m_operations.post_unless_queued(*record));

    return true;
}

void display_engine::serve_hotplug_record(operation& op) {
    static_cast<event_record&>(op).engine->serve_hotplug();
}

void display_engine::serve_short_pulse_record(operation&) {
    log::info("nvidia: a DisplayPort short pulse event arrived, which is only logged");
}

void display_engine::serve_hotplug() {
    bool is_released = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        is_released = m_is_released;
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (is_released) {
        log::info("nvidia: a hotplug event arrived once the display was released, so nothing is read");
        return;
    }

    if (m_is_display_failed.load_acquire()) {
        log::info("nvidia: a hotplug event arrived once the display failed, so nothing is read");
        return;
    }

    if (m_failed.is_set()) {
        log::info("nvidia: a hotplug event arrived once the GPU failed, so nothing is read");
        return;
    }

    rm::hotplug_state_params state = {};
    int32_t rc = m_firmware.control(m_objects.display_common, rm::CONTROL_GET_HOTPLUG_STATE, &state, sizeof(state));
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: reading the hotplug state after a hotplug event failed: %d", rc);
        return;
    }

    if (rc != OK) {
        log::error("nvidia: reading the hotplug state after a hotplug event failed: %d, so the display failed", rc);
        fail_display();
        return;
    }

    log::info(
        "nvidia: the GSP's firmware reports display ids 0x%X plugged in and 0x%X unplugged",
        state.plugged_ids,
        state.unplugged_ids
    );
    if (state.plugged_ids != 0 || state.unplugged_ids != 0) {
        update_connected_records(state.plugged_ids, state.unplugged_ids);
        return;
    }

    // Both masks read 0 when an earlier read took the hotplug or the firmware posted the event without recording one,
    // so the connect state tells it
    uint32_t connected_ids = 0;
    rc = read_connected_ids(m_firmware, m_objects.display_common, display_ids_of(m_outputs), connected_ids);
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: reading the connect state after a hotplug event failed: %d", rc);
        return;
    }

    if (rc != OK) {
        log::error("nvidia: reading the connect state after a hotplug event failed: %d, so the display failed", rc);
        fail_display();
        return;
    }

    uint32_t recorded_ids = 0;
    for (uint8_t output = 0; output < m_outputs.count; output++) {
        const display_output& entry = m_outputs.outputs[output];
        recorded_ids |= entry.is_connected ? entry.display_id : 0;
    }

    log::info("nvidia: the connect state reports display ids 0x%X connected", connected_ids);
    update_connected_records(connected_ids & ~recorded_ids, recorded_ids & ~connected_ids);
}

void display_engine::update_connected_records(uint32_t plugged_ids, uint32_t unplugged_ids) {
    uint32_t output_ids = display_ids_of(m_outputs);
    uint32_t stray_ids = (plugged_ids | unplugged_ids) & ~output_ids;
    if (stray_ids != 0) {
        log::info("nvidia: hotplugged display ids 0x%X name no output, so they are left", stray_ids);
    }

    uint32_t doubled_ids = plugged_ids & unplugged_ids & output_ids;
    if (doubled_ids != 0) {
        log::info("nvidia: display ids 0x%X are both plugged in and unplugged, so taken as plugged in", doubled_ids);
    }

    for (uint8_t output = 0; output < m_outputs.count; output++) {
        display_output& entry = m_outputs.outputs[output];
        if ((unplugged_ids & ~plugged_ids & entry.display_id) != 0) {
            entry.is_connected = false;
            m_power_down.d3_outputs &= ~(1u << output);
            log::info("nvidia: output %u, display id 0x%X, is unplugged", output, entry.display_id);
            take_down_output(output);
        }
    }

    for (uint8_t output = 0; output < m_outputs.count; output++) {
        display_output& entry = m_outputs.outputs[output];
        if ((plugged_ids & entry.display_id) != 0) {
            entry.is_connected = true;
            log::info("nvidia: output %u, display id 0x%X, is plugged in", output, entry.display_id);
        }
    }
}

void display_engine::take_down_output(uint8_t output) {
    uint32_t staying_ids = 0;
    engine_head* head = nullptr;
    RUN_ELEVATED(head = darken_head(output, staying_ids));
    if (head == nullptr) {
        return;
    }

    log::info("nvidia: output %u is marked dark, taking head %u down", output, static_cast<uint32_t>(head - m_heads));
    stop_dark_head(*head, staying_ids);

    // gfxfb drops the output's buffers at the report, which a failed display's head may still read
    bool is_reported = output < m_output_count && !m_is_display_failed.load_acquire();
    if (is_reported) {
        RUN_ELEVATED(m_output_changed(this, output));
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE engine_head* display_engine::darken_head(uint8_t output, uint32_t& staying_ids) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
    engine_head* head = find_lit_head(output);
    bool may_take_down = head != nullptr && !m_is_released && !m_is_display_failed.load_acquire();
    if (!may_take_down) {
        sync::spin_unlock_irqrestore(m_output_lock, irq);
        return nullptr;
    }

    head->is_closing = true;
    uint64_t deadline_ns = clock::now_ns() + m_take_down_wait_ns;
    while (true) {
        bool is_waiting = head->has_queued_present && !m_is_display_failed.load_acquire();
        if (!is_waiting || clock::now_ns() >= deadline_ns) {
            break;
        }

        irq = sync::wait_until(m_take_down_queue, m_output_lock, irq, deadline_ns);
    }

    if (m_is_display_failed.load_acquire()) {
        sync::spin_unlock_irqrestore(m_output_lock, irq);
        return nullptr;
    }

    uint8_t index = static_cast<uint8_t>(head - m_heads);
    head->is_lit = false;
    head->is_closing = false;
    head->is_cursor_shown = false;
    disable_vertical_blank(m_registers, index);
    sync::atomic_ref<uint8_t>{m_head_outputs[index]}.store_release(NO_OUTPUT);

    staying_ids = 0;
    for (const engine_head& other : m_heads) {
        staying_ids |= other.is_lit ? other.display_id : 0;
    }

    sync::spin_unlock_irqrestore(m_output_lock, irq);
    return head;
}

// Sends the end of head `index`'s take-down naming `staying_ids`, logged, returning false when the request fails other
// than by a refusal
static bool end_take_down(firmware& fw, rm::handle display_common, uint8_t index, uint32_t staying_ids) {
    int32_t rc = send_display_change(fw, display_common, staying_ids, rm::DISPLAY_CHANGE_END);
    if (rc == ERR_FIRMWARE) {
        log::error(
            "nvidia: display common refused the end of head %u's take-down naming the %s 0x%X still lit",
            index,
            display_ids_noun(staying_ids),
            staying_ids
        );
        return true;
    }

    if (rc != OK) {
        log::error("nvidia: the end of head %u's take-down failed: %d, leaving the display failed", index, rc);
        return false;
    }

    log::info(
        "nvidia: display common took the end of head %u's take-down naming the %s 0x%X still lit",
        index,
        display_ids_noun(staying_ids),
        staying_ids
    );
    return true;
}

void display_engine::stop_dark_head(engine_head& head, uint32_t staying_ids) {
    uint8_t index = static_cast<uint8_t>(&head - m_heads);
    rm::handle display_common = m_objects.display_common;
    int32_t rc = send_display_change(m_firmware, display_common, staying_ids, rm::DISPLAY_CHANGE_START);
    if (rc != OK && rc != ERR_FIRMWARE) {
        log::error("nvidia: the start of head %u's take-down failed: %d, so the display failed", index, rc);
        fail_display();
        return;
    }

    bool is_started = rc == OK;
    if (is_started) {
        log::info(
            "nvidia: display common took the start of head %u's take-down naming the %s 0x%X still lit",
            index,
            display_ids_noun(staying_ids),
            staying_ids
        );
    } else {
        log::error(
            "nvidia: display common refused the start of head %u's take-down naming the %s 0x%X still lit, so it "
            "goes on with no end",
            index,
            display_ids_noun(staying_ids),
            staying_ids
        );
    }

    active_heads stopping = {};
    stopping.heads[index] = {
        .display_id = head.display_id,
        .link_index = head.link.index,
        .link_type  = rm::OUTPUT_RESOURCE_TYPE_SERIAL,
    };

    shutdown_methods methods = {};
    build_shutdown_methods({}, 0, stopping, false, methods);

    uint32_t status_word = 0;
    rc = write_methods(*m_core_queue, methods.methods, methods.count);
    if (rc == OK) {
        rc = send_notified_update(NO_WINDOW_INTERLOCKS, status_word);
    }

    if (rc != OK) {
        log::error("nvidia: the update taking head %u down failed: %d, so the display failed", index, rc);
        fail_display();
        if (is_started) {
            end_take_down(m_firmware, display_common, index, staying_ids);
        }

        return;
    }

    log::info(
        "nvidia: the core channel read the update taking head %u down, its completion notifier reading 0x%08X",
        index,
        status_word
    );

    bool has_ended_dark = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        has_ended_dark = head.has_queued_present;
        if (has_ended_dark) {
            timer::cancel(&head.latch_check_timer);
            timer::cancel(&head.deadline_timer);
            end_present(head, gfxfb::GFXFB_PRESENT_DARK, {});
        }

        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (has_ended_dark) {
        log::warn(
            "nvidia: output %u's present was still queued once head %u stopped, so it ended dark",
            head.output,
            index
        );
    }

    // The light trained the link with FEC off, so a monitor still marked for D3 sleeps before the link goes down
    bool sets_monitor_to_d3 = (m_power_down.d3_outputs & (1u << head.output)) != 0;
    rc = power_down_link(
        m_firmware,
        display_common,
        head.display_id,
        m_power_down.rates[head.output],
        sets_monitor_to_d3,
        false
    );
    if (rc != OK) {
        log::error("nvidia: display id 0x%X's link is not powered down, so the display failed", head.display_id);
        fail_display();

        // Unlike after a failed update, no end follows, since it would wait on the firmware that just failed a request
        return;
    }

    if (is_started && !end_take_down(m_firmware, display_common, index, staying_ids)) {
        fail_display();
    }
}

void display_engine::fail_display() {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        m_is_display_failed.store_release(true);
        fail_queued_presents();
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });
}

void display_engine::turn_on_flips() {
    m_are_flips_on.store_release(true);
}

bool display_engine::are_flips_on() const {
    return m_are_flips_on.load_acquire();
}

bool display_engine::are_presents_supported() const {
    if (!are_flips_on()) {
        return false;
    }

    // `set_up` binds the notifiers before flips turn on, so reading flips with acquire orders these bare reads
    for (const engine_head& head : m_heads) {
        if (head.notifier_context_dma != 0) {
            return true;
        }
    }

    return false;
}

void display_engine::mark_head_lit(registers& regs, const head_light& light, bool has_cursor) {
    engine_head& head = m_heads[light.head];
    mode_times times = {};
    bool has_times = compute_mode_times(light.timing, times);
    if (!has_times || !times.has_latch_check) {
        log::warn(
            "nvidia: head %u's mode leaves no time for a latch check, so a present its vertical blank finds "
            "unlatched is reported at the next",
            light.head
        );
    }

    uint64_t millihertz = head.mode.refresh_millihertz;
    uint64_t frames_per_second = (millihertz + MILLIHERTZ_PER_HERTZ / 2) / MILLIHERTZ_PER_HERTZ;

    // The handler counts a head's frames once its entry names the output, so the entry precedes the enable
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        head.has_cursor = has_cursor;
        head.light_generation++;
        head.display_id = light.display_id;
        head.link = light.link;

        head.is_cursor_shown = false;
        head.is_cursor_update_owed = false;
        head.cursor_hot_x = 0;
        head.cursor_hot_y = 0;

        head.is_lit = true;
        head.times = times;
        head.frames_per_second = static_cast<uint32_t>(frames_per_second);
        m_is_lit = true;

        sync::atomic_ref<uint8_t>{m_head_outputs[light.head]}.store_release(head.output);
        enable_vertical_blank(regs, light.head);
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });
}

// Counts a vertical blank of `head` serviced at `now_ns`, under the output lock, and returns whether it completes the
// head's first second
static bool count_vertical_blank(engine_head& head, uint64_t now_ns, uint32_t position) {
    head.frame_count++;
    head.vertical_blank_ns = now_ns;
    head.serviced_frame = raster_frame(position);
    if (head.frame_count > head.frames_per_second) {
        return false;
    }

    uint16_t line = raster_line(position);
    uint16_t frame = raster_frame(position);
    if (head.frame_count == 1) {
        head.first_second = {now_ns, now_ns, frame, frame, line, line};
    }

    head.first_second.end_ns = now_ns;
    head.first_second.end_frame = frame;
    head.first_second.lowest_line = line < head.first_second.lowest_line ? line : head.first_second.lowest_line;
    head.first_second.highest_line = line > head.first_second.highest_line ? line : head.first_second.highest_line;

    return head.frame_count == head.frames_per_second;
}

// `head`'s flip state as the latch rules read it, under the output lock
static latch_inputs read_latch_inputs(const engine_head& head) {
    return {
        .present_queued      = head.has_queued_present,
        .slot_written        = head.has_queued_present && head.notifier.written(head.queued_slot),
        .frame               = head.frame_count,
        .vertical_blank_ns   = head.vertical_blank_ns,
        .check_frame         = head.check_frame,
        .last_reported_frame = head.last_reported_frame,
        .deadline_ns         = head.queued_deadline_ns,
    };
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool display_engine::service_vertical_blanks(registers& regs, uint32_t pending_heads) {
    bool completes_first_second = false;
    for (uint8_t head = 0; head < m_head_count; head++) {
        // Clearing each event before its count, not after as NVIDIA's driver does, keeps one arriving meanwhile pending
        bool is_pending = (pending_heads & (1u << head)) != 0;
        if (!is_pending || !take_vertical_blank(regs, head)) {
            continue;
        }

        uint8_t output = sync::atomic_ref<uint8_t>{m_head_outputs[head]}.load_acquire();
        if (output == NO_OUTPUT) {
            continue;
        }

        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        engine_head& state = m_heads[head];
        if (state.is_lit && state.output == output) {
            completes_first_second |= count_vertical_blank(state, clock::now_ns(), raster_position(regs, head));
            run_latch_step(state);
        }

        sync::spin_unlock_irqrestore(m_output_lock, irq);
    }

    return completes_first_second;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void display_engine::run_latch_step(engine_head& head) {
    latch_step_decision decision = decide_latch_step(read_latch_inputs(head), head.times);
    switch (decision.action) {
        case latch_step_action::none:
            break;
        case latch_step_action::report:
            timer::cancel(&head.latch_check_timer);
            timer::cancel(&head.deadline_timer);
            end_present(head, gfxfb::GFXFB_PRESENT_LATCHED, decision.report);
            break;
        case latch_step_action::schedule_check:
            head.check_frame = head.frame_count;
            timer::schedule(&head.latch_check_timer, decision.check_ns);
            break;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void display_engine::end_present(engine_head& head, uint32_t status, const latch_report& report) {
    gfxfb::present_result result = {};
    result.status = status;
    if (status == gfxfb::GFXFB_PRESENT_LATCHED) {
        result.frame = report.frame;
        result.timestamp_ns = report.timestamp_ns;
        head.last_reported_frame = report.frame;
    }

    head.has_queued_present = false;
    m_present_done(this, head.output, result);
    if (head.is_closing) {
        sync::wake_all(m_take_down_queue);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void display_engine::fail_queued_presents() {
    for (engine_head& head : m_heads) {
        timer::cancel(&head.latch_check_timer);
        timer::cancel(&head.deadline_timer);
        if (head.has_queued_present) {
            m_is_display_failed.store_release(true);
            end_present(head, gfxfb::GFXFB_PRESENT_FAILED, {});
        }
    }
}

void display_engine::check_latch(timer::deadline_timer* timer) {
    engine_head* head = timer::owner_of<engine_head, &engine_head::latch_check_timer>(timer);
    display_engine* engine = head->engine;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(engine->m_output_lock);
        latch_report report = {};
        if (head->is_lit && decide_latch_check(read_latch_inputs(*head), head->times, clock::now_ns(), report)) {
            timer::cancel(&head->deadline_timer);
            engine->end_present(*head, gfxfb::GFXFB_PRESENT_LATCHED, report);
        }

        sync::spin_unlock_irqrestore(engine->m_output_lock, irq);
    });
}

void display_engine::expire_deadline(timer::deadline_timer* timer) {
    engine_head* head = timer::owner_of<engine_head, &engine_head::deadline_timer>(timer);
    display_engine* engine = head->engine;
    uint8_t output = 0;
    deadline_decision decision = {};
    bool must_log_estimate = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(engine->m_output_lock);
        uint64_t now_ns = clock::now_ns();
        output = head->output;

        // A take-down ends the present of a head it marked dark once the head stops, so the deadline ends nothing
        if (head->is_lit) {
            decision = decide_deadline(read_latch_inputs(*head), now_ns);
        }

        switch (decision.action) {
            case deadline_action::none:
                break;
            case deadline_action::report:
                timer::cancel(&head->latch_check_timer);
                engine->end_present(*head, gfxfb::GFXFB_PRESENT_LATCHED, decision.report);
                must_log_estimate = head->last_estimate_log_ns == 0 ||
                                    now_ns - head->last_estimate_log_ns >= ESTIMATE_LOG_INTERVAL_NS;
                if (must_log_estimate) {
                    head->last_estimate_log_ns = now_ns;
                }

                break;
            case deadline_action::fail:
                engine->m_is_display_failed.store_release(true);
                engine->fail_queued_presents();
                break;
        }

        sync::spin_unlock_irqrestore(engine->m_output_lock, irq);
    });

    if (must_log_estimate) {
        log::error(
            "nvidia: output %u's present reached its deadline with its head holding the buffer but no vertical blank "
            "serviced, so the vertical blank interrupt stopped, and it ended latched at an estimated frame %lu and "
            "time %lu ns",
            output,
            decision.report.frame,
            decision.report.timestamp_ns
        );
    }

    if (decision.action == deadline_action::fail) {
        log::error(
            "nvidia: output %u's present reached its deadline before its head took the buffer, so the display failed",
            output
        );
    }
}

engine_head* display_engine::find_lit_head(uint32_t output) {
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        if (m_heads[head].is_lit && m_heads[head].output == output) {
            return &m_heads[head];
        }
    }

    return nullptr;
}

void display_engine::log_first_second() {
    for (uint8_t head = 0; head < DISPLAY_HEADS_MAX; head++) {
        bool must_log = false;
        uint32_t frames = 0;
        uint64_t millihertz = 0;
        first_second_statistics statistics = {};
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
            engine_head& state = m_heads[head];
            bool has_first_second = state.frames_per_second != 0 && state.frame_count >= state.frames_per_second;
            must_log = has_first_second && !state.has_logged_first_second;
            if (must_log) {
                state.has_logged_first_second = true;
                frames = state.frames_per_second;
                millihertz = state.mode.refresh_millihertz;
                statistics = state.first_second;
            }

            sync::spin_unlock_irqrestore(m_output_lock, irq);
        });

        if (!must_log) {
            continue;
        }

        // The head's frame count has 16 bits and starts anywhere, so its advance wraps
        uint16_t head_frames = static_cast<uint16_t>(statistics.end_frame - statistics.start_frame);
        log::info(
            "nvidia: the handler serviced head %u's vertical blanks 1 to %u since the light over %lu us, at its "
            "mode's %lu.%03lu Hz, at raster lines %u to %u, while the head's own frame count advanced %u",
            head,
            frames,
            (statistics.end_ns - statistics.start_ns) / NS_PER_US,
            millihertz / MILLIHERTZ_PER_HERTZ,
            millihertz % MILLIHERTZ_PER_HERTZ,
            statistics.lowest_line,
            statistics.highest_line,
            head_frames
        );
    }
}

int32_t display_engine::read_output(uint32_t output, gfxfb::output_state& out) {
    if (output >= m_output_count) {
        return gfxfb::ERR_INVALID_ARG;
    }

    bool is_lit = false;
    bool has_cursor = false;
    gfxfb::gfxfb_mode mode = {};
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        const engine_head* head = find_lit_head(output);
        is_lit = head != nullptr;
        if (is_lit) {
            mode = head->mode;
            has_cursor = head->has_cursor;
        }

        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    // No output relights, so `relighting` stays false
    out = {};
    out.lit = is_lit;
    out.boot_output = (m_boot_outputs & (1u << output)) != 0;
    if (is_lit) {
        out.current = mode;
    }

    uint32_t cursor_max = is_lit && !has_cursor ? 0 : CURSOR_DIMENSION;
    out.cursor_max_width = cursor_max;
    out.cursor_max_height = cursor_max;

    return gfxfb::OK;
}

int32_t display_engine::read_modes(uint32_t output, gfxfb::gfxfb_mode* out, uint32_t capacity, uint32_t& count) {
    gfxfb::output_state state = {};
    int32_t rc = read_output(output, state);
    if (rc != gfxfb::OK) {
        return rc;
    }

    count = state.lit ? 1 : 0;
    if (count > 0 && capacity > 0) {
        out[0] = state.current;
    }

    return gfxfb::OK;
}

gfxfb::buffer* display_engine::base_buffer(uint32_t output) {
    gfxfb::buffer* base = nullptr;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        engine_head* head = find_lit_head(output);
        if (head != nullptr) {
            base = &head->userland_base_buffer;
        }

        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    return base;
}

int32_t display_engine::present(uint32_t output, gfxfb::buffer* buf, const gfxfb::gfxfb_rect*, uint32_t) {
    if (!are_presents_supported()) {
        return gfxfb::ERR_UNSUPPORTED;
    }

    int32_t rc = gfxfb::OK;
    uint8_t window = 0;
    int32_t write_rc = OK;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        rc = queue_present(output, *buf, window, write_rc);
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (write_rc != OK) {
        log::error(
            "nvidia: writing a present into window %u's channel failed: %d, so the display failed",
            window,
            write_rc
        );
    }

    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t display_engine::queue_present(
    uint32_t output,
    const gfxfb::buffer& buf,
    uint8_t& window,
    int32_t& write_rc
) {
    engine_head* head = find_lit_head(output);
    if (head != nullptr && head->notifier_context_dma == 0) {
        return gfxfb::ERR_UNSUPPORTED;
    }

    if (m_is_released || m_is_display_failed.load_acquire()) {
        return gfxfb::ERR_DEVICE;
    }

    if (head == nullptr || head->is_closing) {
        return gfxfb::ERR_DARK;
    }

    if (buf.width != head->mode.width || buf.height != head->mode.height) {
        return gfxfb::ERR_INVALID_ARG;
    }

    // gfxfb presents only buffers `base_buffer()` and `create_buffer()` handed out, a base buffer keeping its bindings
    // once its output is taken down
    const engine_head* source = nullptr;
    for (const engine_head& candidate : m_heads) {
        if (&buf == &candidate.userland_base_buffer) {
            source = &candidate;
        }
    }

    rm::handle surface_context_dma = 0;
    uint32_t bound_windows = 0;
    if (source != nullptr) {
        surface_context_dma = source->base_buffer_context_dma;
        bound_windows = source->base_buffer_windows;
    } else {
        const auto& created = static_cast<const scanout_buffer&>(buf);
        surface_context_dma = created.context_dma;
        bound_windows = created.bound_windows;
    }

    if ((bound_windows & (1u << head->window)) == 0) {
        return gfxfb::ERR_REFUSED;
    }

    window = head->window;
    window_image image = {
        .window                    = head->window,
        .surface_context_dma       = surface_context_dma,
        .width                     = buf.width,
        .height                    = buf.height,
        .pitch                     = buf.pitch,
        .look_up_table_context_dma = m_look_up_table_context_dma,
    };

    uint32_t slot = head->next_slot;
    window_image_update update = {
        .notifier_context_dma = head->notifier_context_dma,
        .notifier_slot        = slot,
        .interlocks_core      = false,
        .window_interlocks    = 1u << head->window,
    };

    window_image_methods methods = {};
    build_window_image_methods(image, update, methods);

    // The whole present fits before anything is written, so a full push buffer never holds part of one
    int32_t rc = head->window_queue->reserve(window_image_bytes(methods));
    if (rc == ERR_QUEUE_FULL) {
        return gfxfb::ERR_REFUSED;
    }

    if (rc == OK) {
        head->notifier.clear(slot);
        rc = write_reserved_window_methods(*head->window_queue, methods);
    }

    if (rc != OK) {
        write_rc = rc;
        m_is_display_failed.store_release(true);
        fail_queued_presents();
        return gfxfb::ERR_DEVICE;
    }

    head->window_queue->kick();
    head->next_slot = next_notifier_slot(slot);

    head->has_queued_present = true;
    head->queued_slot = slot;
    head->queued_deadline_ns = clock::now_ns() + gfxfb::GFXFB_PRESENT_TIMEOUT_NS;
    timer::schedule(&head->deadline_timer, head->queued_deadline_ns);

    return gfxfb::OK;
}

int32_t display_engine::flush(uint32_t output, const gfxfb::gfxfb_rect*, uint32_t) {
    bool is_released = false;
    bool is_lit = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        is_released = m_is_released;
        is_lit = find_lit_head(output) != nullptr;
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (is_released || m_is_display_failed.load_acquire()) {
        return gfxfb::ERR_DEVICE;
    }

    return is_lit ? gfxfb::OK : gfxfb::ERR_DARK;
}

// Unbinds `buffer` from each window's channel a failed binding left it bound to, keeping it bound for good, logged,
// once an unbinding fails too
static void undo_bindings(instance_memory& memory, scanout_buffer& buffer) {
    while (buffer.bound_windows != 0) {
        uint8_t window = static_cast<uint8_t>(__builtin_ctz(buffer.bound_windows));
        int32_t rc = memory.unbind(buffer.context_dma, window_channel_number(window));
        if (rc != OK) {
            log::error(
                "nvidia: unbinding the display buffer at 0x%lX in video memory from window %u's channel failed: %d, "
                "so the buffer is kept for good",
                buffer.range.offset,
                window,
                rc
            );
            return;
        }

        buffer.bound_windows &= ~(1u << window);
    }
}

int32_t display_engine::create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) {
    if (!are_presents_supported()) {
        return gfxfb::ERR_UNSUPPORTED;
    }

    bool is_released = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        is_released = m_is_released;
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (is_released || m_is_display_failed.load_acquire()) {
        return gfxfb::ERR_DEVICE;
    }

    bool fits_the_window = width <= rm::SIZE_FIELD_MAX && height <= rm::SIZE_FIELD_MAX;
    if (format != gfxfb::GFXFB_FORMAT_XRGB8888 || !fits_the_window) {
        return gfxfb::ERR_INVALID_ARG;
    }

    auto* buffer = heap::ualloc_new<scanout_buffer>();
    if (buffer == nullptr) {
        return gfxfb::ERR_NO_MEMORY;
    }

    int32_t rc = set_up_scanout_buffer(m_memory, width, height, *buffer);
    if (rc != gfxfb::OK) {
        heap::ufree_delete(buffer);
        return rc;
    }

    buffer->task_operation.work = bind_buffer;
    buffer->task_operation.engine = this;
    buffer->task_operation.buffer = buffer;
    RUN_ELEVATED(rc = m_operations.run_on_task(buffer->task_operation));
    if (rc != gfxfb::OK) {
        // A buffer whose bindings could not all be undone stays named by a channel, so it is kept for good
        if (buffer->bound_windows == 0) {
            m_memory.unmap_for_cpu(buffer->range);
            m_memory.release(buffer->range);
            heap::ufree_delete(buffer);
        }

        return rc;
    }

    *out = buffer;

    return gfxfb::OK;
}

void display_engine::bind_buffer(operation& op) {
    auto& binding = static_cast<buffer_operation&>(op);
    display_engine& engine = *binding.engine;
    scanout_buffer& buffer = *binding.buffer;

    bool is_released = false;
    uint32_t lit_windows = 0;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
        is_released = engine.m_is_released;
        for (const engine_head& head : engine.m_heads) {
            if (head.is_lit) {
                lit_windows |= 1u << head.window;
            }
        }

        sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
    });

    if (is_released || engine.m_is_display_failed.load_acquire()) {
        op.result = gfxfb::ERR_DEVICE;
        return;
    }

    if (lit_windows == 0) {
        op.result = gfxfb::OK;
        return;
    }

    buffer.context_dma = engine.m_firmware.new_handle();
    context_dma_memory buffer_memory = {
        .in_video_memory = true,
        .address = buffer.range.offset,
        .bytes = buffer.range.size,
    };

    while (buffer.bound_windows != lit_windows) {
        uint8_t window = static_cast<uint8_t>(__builtin_ctz(lit_windows & ~buffer.bound_windows));
        int32_t rc = engine.m_instance_memory.bind(buffer.context_dma, window_channel_number(window), buffer_memory);
        if (rc != OK) {
            op.result = rc == ERR_NO_MEMORY ? gfxfb::ERR_NO_MEMORY : gfxfb::ERR_REFUSED;
            undo_bindings(engine.m_instance_memory, buffer);
            return;
        }

        buffer.bound_windows |= 1u << window;
        log::info(
            "nvidia: a display buffer of %ux%u pixels at 0x%lX in video memory is bound to window %u's channel as "
            "context DMA 0x%X",
            buffer.width,
            buffer.height,
            buffer.range.offset,
            window,
            buffer.context_dma
        );
    }

    op.result = gfxfb::OK;
}

void display_engine::release_buffer(gfxfb::buffer* buf) {
    for (const engine_head& head : m_heads) {
        if (buf == &head.userland_base_buffer) {
            return;
        }
    }

    auto* buffer = static_cast<scanout_buffer*>(buf);
    buffer->task_operation.work = release_scanout_buffer;
    RUN_ELEVATED(m_operations.post_to_task(buffer->task_operation));
}

void display_engine::release_scanout_buffer(operation& op) {
    auto& release = static_cast<buffer_operation&>(op);
    display_engine& engine = *release.engine;
    scanout_buffer* buffer = release.buffer;

    bool is_released = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
        is_released = engine.m_is_released;
        sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
    });

    if (is_released) {
        log::warn(
            "nvidia: the display buffer at 0x%lX in video memory is kept for good, as the engine was released",
            buffer->range.offset
        );
        return;
    }

    if (engine.m_is_display_failed.load_acquire()) {
        log::warn(
            "nvidia: the display buffer at 0x%lX in video memory is kept for good, as the display failed",
            buffer->range.offset
        );
        return;
    }

    while (buffer->bound_windows != 0) {
        uint8_t window = static_cast<uint8_t>(__builtin_ctz(buffer->bound_windows));
        bool is_shown = false;
        uint8_t head = 0;
        uint64_t serviced = 0;
        uint16_t head_frames = 0;
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
            for (uint8_t index = 0; index < DISPLAY_HEADS_MAX; index++) {
                const engine_head& state = engine.m_heads[index];
                if (state.is_lit && state.window == window) {
                    is_shown = true;
                    head = index;
                    serviced = state.frame_count;
                    head_frames = static_cast<uint16_t>(state.serviced_frame - state.first_second.start_frame);
                }
            }

            sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
        });

        int32_t rc = engine.m_instance_memory.unbind(buffer->context_dma, window_channel_number(window));
        if (rc != OK) {
            log::error(
                "nvidia: unbinding the display buffer at 0x%lX in video memory from window %u's channel failed: %d, "
                "so the buffer is kept for good",
                buffer->range.offset,
                window,
                rc
            );
            return;
        }

        buffer->bound_windows &= ~(1u << window);
        if (!is_shown) {
            log::info(
                "nvidia: the display buffer at 0x%lX in video memory is unbound from window %u's channel, whose head "
                "is dark",
                buffer->range.offset,
                window
            );
            continue;
        }

        log::info(
            "nvidia: the display buffer at 0x%lX in video memory is unbound from window %u's channel, once the handler "
            "serviced head %u's vertical blanks 1 to %lu since the light while the head's own frame count advanced %u",
            buffer->range.offset,
            window,
            head,
            serviced,
            head_frames
        );
    }

    engine.m_memory.unmap_for_cpu(buffer->range);
    engine.m_memory.release(buffer->range);
    heap::ufree_delete(buffer);
}

int32_t display_engine::set_cursor(uint32_t output, const gfxfb::cursor_image& image) {
    if (image.width == 0) {
        return hide_cursor(output);
    }

    return show_cursor_picture(output, image);
}

int32_t display_engine::move_cursor(uint32_t output, int32_t x, int32_t y) {
    uint64_t deadline_ns = 0;
    while (true) {
        int32_t rc = gfxfb::OK;
        int32_t write_rc = OK;
        engine_head* head = nullptr;
        uint64_t tried_ns = clock::now_ns();
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
            rc = check_cursor_call(output, head);
            if (rc == gfxfb::OK) {
                head->cursor_x = x;
                head->cursor_y = y;
            }

            if (rc == gfxfb::OK && head->is_cursor_shown) {
                uint32_t position = cursor_position(x, y, head->cursor_hot_x, head->cursor_hot_y);
                write_rc = write_cursor_position(*head, position);
            }

            sync::spin_unlock_irqrestore(m_output_lock, irq);
        });

        if (write_rc != ERR_QUEUE_FULL || !retries_cursor_write(tried_ns, deadline_ns)) {
            return end_cursor_call(output, head, "move", rc, write_rc);
        }
    }
}

int32_t display_engine::hide_cursor(uint32_t output) {
    uint64_t deadline_ns = 0;
    while (true) {
        int32_t rc = gfxfb::OK;
        int32_t write_rc = OK;
        engine_head* head = nullptr;
        uint64_t tried_ns = clock::now_ns();
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
            rc = check_cursor_call(output, head);
            if (rc == gfxfb::OK && head->is_cursor_shown) {
                write_rc = write_cursor_position(*head, PARKED_CURSOR_POSITION);
                head->is_cursor_shown = write_rc != OK;
            }

            sync::spin_unlock_irqrestore(m_output_lock, irq);
        });

        if (write_rc != ERR_QUEUE_FULL || !retries_cursor_write(tried_ns, deadline_ns)) {
            return end_cursor_call(output, head, "hide", rc, write_rc);
        }
    }
}

int32_t display_engine::show_cursor_picture(uint32_t output, const gfxfb::cursor_image& image) {
    int32_t rc = gfxfb::OK;
    engine_head* head = nullptr;
    uint32_t generation = 0;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        rc = check_cursor_call(output, head);
        if (rc == gfxfb::OK) {
            generation = head->light_generation;
        }

        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    bool fits = image.width <= CURSOR_DIMENSION && image.height <= CURSOR_DIMENSION;
    if (rc == gfxfb::OK && !fits) {
        rc = gfxfb::ERR_INVALID_ARG;
    }

    if (rc != gfxfb::OK) {
        return rc;
    }

    // Each row's fence keeps its writes ahead of whatever the spinlock's next holder writes to the card
    for (uint32_t row = 0; row < CURSOR_DIMENSION; row++) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
            rc = check_picture(*head, output, generation);
            if (rc == gfxfb::OK) {
                write_cursor_row(head->cursor_mapping.virt, image, row);
                barrier::io_full();
            }

            sync::spin_unlock_irqrestore(m_output_lock, irq);
        });

        if (rc != gfxfb::OK) {
            return rc;
        }

        if (m_cursor_step_seam != nullptr) {
            m_cursor_step_seam(*this);
        }
    }

    // The read back waits for every row's writes to reach video memory, so it runs outside the spinlock
    mmio::read64(head->cursor_mapping.virt);
    uint64_t deadline_ns = clock::now_ns() + m_cursor_wait_ns;
    while (true) {
        int32_t write_rc = OK;
        uint64_t tried_ns = clock::now_ns();
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
            rc = check_picture(*head, output, generation);
            if (rc == gfxfb::OK) {
                head->cursor_hot_x = image.hot_x;
                head->cursor_hot_y = image.hot_y;
                head->is_cursor_shown = true;

                uint32_t position = cursor_position(head->cursor_x, head->cursor_y, image.hot_x, image.hot_y);
                write_rc = write_cursor_position(*head, position);
            }

            sync::spin_unlock_irqrestore(m_output_lock, irq);
        });

        if (write_rc != ERR_QUEUE_FULL || !retries_cursor_write(tried_ns, deadline_ns)) {
            return end_cursor_call(output, head, "picture", rc, write_rc);
        }
    }
}

int32_t display_engine::check_cursor_call(uint32_t output, engine_head*& head) {
    head = find_lit_head(output);
    if (head != nullptr && !head->has_cursor) {
        return gfxfb::ERR_UNSUPPORTED;
    }

    if (m_is_released || m_is_display_failed.load_acquire()) {
        return gfxfb::ERR_DEVICE;
    }

    return head != nullptr ? gfxfb::OK : gfxfb::ERR_DARK;
}

int32_t display_engine::check_picture(const engine_head& head, uint32_t output, uint32_t generation) const {
    if (m_is_released || m_is_display_failed.load_acquire()) {
        return gfxfb::ERR_DEVICE;
    }

    bool is_lit_since_call = head.is_lit && head.output == output && head.light_generation == generation;
    return is_lit_since_call ? gfxfb::OK : gfxfb::ERR_DARK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t display_engine::write_cursor_position(engine_head& head, uint32_t position) {
    // The free register reads 0 while the front end takes a method, so a try after a refused `UPDATE` resumes there
    bool is_update_owed = head.is_cursor_update_owed && head.owed_cursor_position == position;
    int32_t rc = OK;
    if (!is_update_owed) {
        rc = head.cursor.write(cursor_channel::HOT_SPOT_POINT, position);
    }

    if (rc == OK) {
        head.is_cursor_update_owed = true;
        head.owed_cursor_position = position;
        rc = head.cursor.write(cursor_channel::UPDATE, 0);
    }

    if (rc == OK) {
        head.is_cursor_update_owed = false;
        head.has_logged_cursor_refusal = false;
    } else if (rc == ERR_BAD_STATE) {
        m_is_display_failed.store_release(true);
        fail_queued_presents();
    }

    return rc;
}

bool display_engine::retries_cursor_write(uint64_t tried_ns, uint64_t& deadline_ns) {
    if (deadline_ns == 0) {
        deadline_ns = tried_ns + m_cursor_wait_ns;
    } else if (tried_ns > deadline_ns) {
        return false;
    }

    sched::yield();
    if (m_cursor_step_seam != nullptr) {
        m_cursor_step_seam(*this);
    }

    return true;
}

int32_t display_engine::end_cursor_call(
    uint32_t output,
    engine_head* head,
    const char* call,
    int32_t rc,
    int32_t write_rc
) {
    if (rc != gfxfb::OK) {
        return rc;
    }

    if (write_rc == ERR_BAD_STATE) {
        log::error(
            "nvidia: output %u's cursor %s found its cursor channel's free register reading all ones, so the display "
            "failed",
            output,
            call
        );
        return gfxfb::ERR_DEVICE;
    }

    if (write_rc != ERR_QUEUE_FULL) {
        return gfxfb::OK;
    }

    bool is_first_refusal = false;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(m_output_lock);
        is_first_refusal = !head->has_logged_cursor_refusal;
        head->has_logged_cursor_refusal = true;
        sync::spin_unlock_irqrestore(m_output_lock, irq);
    });

    if (is_first_refusal) {
        log::error(
            "nvidia: output %u's cursor %s is refused, as its cursor channel found no room in time",
            output,
            call
        );
    }

    return gfxfb::ERR_REFUSED;
}

} // namespace drivers::nvidia
