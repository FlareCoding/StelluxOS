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
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/displayport.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "hw/mmio.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

// NVIDIA's NV_EVO_SURFACE_ALIGNMENT, the alignment of the surfaces NVIDIA's modesetting module scans out and the least
// alignment of its look-up table surfaces, which it aligns to 4 KiB in any case
constexpr uint64_t SURFACE_ALIGNMENT = 0x1000;

// Logs the maker, name and timings an output's EDID gives
static void log_monitor(uint8_t output_index, const edid::monitor& monitor) {
    log::info(
        "nvidia: output %u's monitor is %s \"%s\", its EDID giving %u timings, %s, with %u more that did not fit and "
        "%u extension blocks skipped",
        output_index,
        monitor.maker,
        monitor.name,
        monitor.timing_count,
        monitor.first_timing_is_preferred ? "the first preferred" : "none preferred",
        monitor.dropped_timings,
        monitor.skipped_extensions
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
        "nvidia: a link on output %u's connector takes up to %u lanes at DisplayPort rate 0x%02X",
        output_index,
        limits.max_lanes,
        static_cast<uint32_t>(limits.max_rate)
    );
}

// Logs the most lanes and the highest rate a link to an output's monitor takes, the rate as DisplayPort codes it, and
// whether the monitor takes enhanced framing
static void log_monitor_link_limits(uint8_t output_index, const dpcd_capabilities& dpcd) {
    displayport::link_limits limits = monitor_link_limits(dpcd);
    log::info(
        "nvidia: output %u's link to its monitor takes up to %u lanes at DisplayPort rate 0x%02X %s enhanced framing",
        output_index,
        limits.max_lanes,
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
            "nvidia: output %u has no mode, as none of its EDID's %u timings both refreshes below 60.600 Hz and is "
            "carried by its link",
            output_index,
            monitor.timing_count
        );
        return;
    }

    if (candidates.count == 0) {
        log::info(
            "nvidia: output %u has no mode, as its link carries none of its EDID's %u timings",
            output_index,
            monitor.timing_count
        );
        return;
    }

    modes.count = candidates.count;
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
        "0x%02X, ranked first of the %u timings its link carries%s",
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
        counted
    );
}

// Reads each connected output's EDID and DPCD capabilities, logs them and its mode, and keeps its modes in `modes`, the
// EDID parsed into one `edid::monitor`, both on the unprivileged heap, as the stack cannot hold them
static int32_t read_connected_monitors(
    firmware& fw,
    rm::handle display_common,
    const chip_displayport& gpu,
    const display_outputs& outputs,
    output_modes*& modes
) {
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

    log::info("nvidia: the static digital display ids give DisplayPort outputs, %u of them", outputs.count);
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

// Chooses the output to light first and its mode with `choose_first_light`, on the head and window `find_light_target`
// gives, and logs the choice or why none is made
static int32_t choose_and_log_first_light(
    firmware& fw,
    rm::handle bandwidth_check,
    const display_heads& heads,
    const active_heads& active,
    const display_outputs& outputs,
    const output_modes* modes,
    first_light& light,
    bool& is_chosen
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
    int32_t rc = choose_first_light(fw, bandwidth_check, modes, outputs.count, target, light, is_chosen);

    // Lighting nothing leaves the driver attached, so a request that cannot be allocated ends only the choice
    if (rc == ERR_NO_MEMORY) {
        log::error("nvidia: no output is chosen to light, as the bandwidth check's request cannot be allocated");
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (!is_chosen) {
        log::info(
            "nvidia: no output is chosen to light, as every mode of the %u %s with modes is refused",
            outputs_with_modes,
            outputs_with_modes == 1 ? "output" : "outputs"
        );
        return OK;
    }

    uint64_t millihertz = light.timing.refresh_millihertz();
    log::info(
        "nvidia: output %u is chosen to light first, at its EDID's timing %u, %u by %u at %lu.%03lu Hz, on head %u "
        "with window %u, which the bandwidth check accepts",
        light.output,
        light.timing_index,
        light.timing.width,
        light.timing.height,
        millihertz / 1000,
        millihertz % 1000,
        light.head,
        light.window
    );

    return OK;
}

// Allocates a channel's push buffer in DMA pages the card reaches, cached only when the card's DMA is coherent
static int32_t allocate_push_buffer(const dma::device_profile& card, dma::buffer& out) {
    size_t pages = pmm::page_align_up(PUSH_BUFFER_BYTES) / pmm::PAGE_SIZE;
    int32_t rc = dma::OK;
    RUN_ELEVATED(rc = dma::alloc_pages(card, pages, dma::caching::cached_when_coherent, paging::PAGE_USER, out));

    return rc;
}

// Allocates the chosen window's channel with its push buffer and logs it, or returns `OK` with why the window has no
// channel logged, for each reason `display_engine::set_up` lists, else the error of the request that failed
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

// Allocates the chosen output's base buffer at its mode's size and clears it through a mapping for the CPU, logging it,
// or returns `OK` with why the output has no base buffer logged, else the error of the call that failed, logged
static int32_t set_up_base_buffer(video_memory& memory, const first_light& light, vram_range& out) {
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
        "nvidia: output %u's base buffer lies at 0x%lX in video memory, its %u rows %u bytes apart, and is cleared to "
        "black",
        light.output,
        allocated.offset,
        light.timing.height,
        pitch
    );

    return OK;
}

// Binds `surface`, video memory for `window` to read, to the window's channel under a new handle given in
// `context_dma`, and when the binding fails releases the memory and empties `surface`, since nothing names it then
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
    }

    return rc;
}

// Binds the chosen output's base buffer to its window's channel, or releases and empties it and returns `OK` when the
// binding finds no room, else releases and empties it and returns the binding's error, each outcome logged
static int32_t bind_base_buffer(
    firmware& fw,
    instance_memory& instances,
    video_memory& memory,
    const first_light& light,
    vram_range& base_buffer
) {
    rm::handle context_dma = 0;
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
        "nvidia: the look-up table surface lies at 0x%lX in video memory, its input table passing each color through "
        "unchanged",
        allocated.offset
    );

    return OK;
}

// Binds the look-up table surface to `window`'s channel, or releases and empties it and returns `OK` when the binding
// finds no room, else releases and empties it and returns the binding's error, each outcome logged
static int32_t bind_look_up_table(
    firmware& fw,
    instance_memory& instances,
    video_memory& memory,
    uint8_t window,
    vram_range& table
) {
    rm::handle context_dma = 0;
    int32_t rc = bind_to_window_channel(fw, instances, memory, window, table, context_dma);
    if (rc == ERR_NO_MEMORY) {
        log::error(
            "nvidia: the display engine has no look-up table surface, as binding it to window %u's channel found no "
            "room, so its video memory is released",
            window
        );
        return OK;
    }

    if (rc != OK) {
        log::error(
            "nvidia: binding the look-up table surface to window %u's channel failed: %d, so its video memory is "
            "released",
            window,
            rc
        );
        return rc;
    }

    log::info(
        "nvidia: the look-up table surface is bound to window %u's channel as context DMA 0x%X",
        window,
        context_dma
    );

    return OK;
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

// Writes `method` into `queue`, and when `write` returns `ERR_QUEUE_FULL`, kicks, waits for GET to reach PUT and writes
// it again, as `display_channel::write` asks of a writer that may wait. One wait makes room for a method of one value
static int32_t write_waiting(display_channel& queue, const display_method& method) {
    int32_t rc = queue.write(method.method, &method.value, 1);
    if (rc != ERR_QUEUE_FULL) {
        return rc;
    }

    queue.kick();
    rc = queue.wait_idle(clock::now_ns() + IDLE_TIMEOUT_NS);
    if (rc != OK) {
        return rc;
    }

    return queue.write(method.method, &method.value, 1);
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

// Writes and kicks the update applying the methods before it with no channel or cursor interlocked, naming the
// completion notifier's context DMA `notifier_context_dma` with write-awaken, or no notifier for 0
static int32_t write_update(display_channel& queue, rm::handle notifier_context_dma) {
    uint32_t notifier_control = 0;
    if (notifier_context_dma != 0) {
        notifier_control = rm::NOTIFIER_CONTROL_MODE_WRITE_AWAKEN | rm::NOTIFIER_CONTROL_NOTIFY;
    }

    const display_method methods[] = {
        {rm::CORE_SET_NOTIFIER_CONTEXT_DMA, notifier_context_dma},
        {rm::CORE_SET_NOTIFIER_CONTROL, notifier_control},
        {rm::CORE_SET_INTERLOCK_FLAGS, 0},
        {rm::CORE_SET_WINDOW_INTERLOCK_FLAGS, 0},
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

// Logs each active head's display id and its link's control as the core channel's armed state reads them, both 0 once
// the shutdown update took effect
static void log_heads_after_shutdown(const registers& regs, const active_heads& active) {
    static constexpr uint32_t ARMED_STATE = CORE_CHANNEL_AREA + CORE_CHANNEL_ARMED_STATE;

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
            regs.read(ARMED_STATE + display_id_method),
            state.link_index,
            regs.read(ARMED_STATE + link_method)
        );
    }
}

display_engine::display_engine(firmware& fw, video_memory& memory)
    : m_firmware(fw)
    , m_memory(memory)
    , m_instance_memory(memory, fw) {}

int32_t display_engine::set_up(
    registers& regs,
    const dma::device_profile& card,
    const chip_info& chip,
    rm::handle device
) {
    const chip_display& display = chip.display;

    uint32_t present_windows = 0;
    int32_t rc = read_present_windows(m_firmware, display, present_windows);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the GSP's firmware reports windows 0x%X present, of the chip's %u windows",
        present_windows,
        display.window_count
    );

    rc = m_instance_memory.set_up();
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the display's instance memory lies at 0x%lX in video memory, and the GSP's firmware accepted its "
        "address",
        m_instance_memory.range().offset
    );

    rc = allocate_display_objects(m_firmware, device, display, m_objects);
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

    bool has_crossbar = false;
    display_heads heads = {};
    display_outputs outputs = {};
    rc = read_display_facts(m_firmware, m_objects, display, has_crossbar, heads, outputs);
    if (rc != OK) {
        return rc;
    }

    rc = read_connected_monitors(m_firmware, m_objects.display_common, display.displayport, outputs, m_output_modes);
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
    first_light light = {};
    bool is_chosen = false;
    rc = choose_and_log_first_light(
        m_firmware,
        m_objects.bandwidth_check,
        heads,
        active,
        outputs,
        m_output_modes,
        light,
        is_chosen
    );

    free_output_modes(m_output_modes);
    if (rc != OK || !is_chosen) {
        return rc;
    }

    rc = set_up_window_channel(
        m_firmware,
        regs,
        card,
        m_objects.display,
        display,
        present_windows,
        light.window,
        m_window_push_buffer,
        m_window_channel
    );
    if (rc != OK || m_window_channel == 0) {
        return rc;
    }

    rc = set_up_base_buffer(m_memory, light, m_base_buffer);
    if (rc != OK || m_base_buffer.size == 0) {
        return rc;
    }

    rc = bind_base_buffer(m_firmware, m_instance_memory, m_memory, light, m_base_buffer);
    if (rc != OK || m_base_buffer.size == 0) {
        return rc;
    }

    rc = set_up_look_up_table(m_memory, m_look_up_table);
    if (rc != OK || m_look_up_table.size == 0) {
        return rc;
    }

    rc = bind_look_up_table(m_firmware, m_instance_memory, m_memory, light.window, m_look_up_table);
    if (rc != OK || m_look_up_table.size == 0) {
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

    return bring_up(regs, heads, present_windows, active, has_crossbar, outputs.outputs[light.output]);
}

int32_t display_engine::bring_up(
    const registers& regs,
    const display_heads& heads,
    uint32_t present_windows,
    const active_heads& active,
    bool has_crossbar,
    const display_output& output
) {
    rm::handle display_common = m_objects.display_common;
    uint32_t display_id = output.display_id;
    int32_t rc = send_display_change(m_firmware, display_common, display_id, rm::DISPLAY_CHANGE_START);
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display common refused the start of the mode set naming display id 0x%X", display_id);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    log::info("nvidia: display common took the start of the mode set naming display id 0x%X", display_id);
    int32_t link_rc = OK;
    bool is_link_set_up = false;
    rc = count_active_heads(active) > 0 ? shut_down_heads(regs, heads, present_windows, active) : OK;
    if (rc != OK) {
        log::error("nvidia: the shutdown update failed: %d, so bring-up takes the display engine down", rc);
    } else {
        link_rc = set_up_output_link(m_firmware, display_common, has_crossbar, output, is_link_set_up);
        if (!is_link_set_up) {
            log::error(
                "nvidia: display id 0x%X's link is not set up, so bring-up takes the display engine down",
                display_id
            );
        }
    }

    if (!is_link_set_up) {
        rc = take_down(heads, present_windows, active);
        if (rc != OK) {
            log::error("nvidia: taking the display engine down failed: %d, so its heads may still scan out", rc);
        }
    }

    rc = send_display_change(m_firmware, display_common, display_id, rm::DISPLAY_CHANGE_END);
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display common refused the end of the mode set naming display id 0x%X", display_id);
        return link_rc;
    }

    if (rc != OK) {
        return link_rc != OK ? link_rc : rc;
    }

    log::info("nvidia: display common took the end of the mode set naming display id 0x%X", display_id);
    return link_rc;
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

    m_core_notifier.clear();
    rc = write_update(*m_core_queue, m_notifier_context_dma);
    if (rc != OK) {
        return rc;
    }

    m_has_sent_update = true;
    rc = m_core_notifier.wait_finished(*m_core_queue, clock::now_ns() + m_notifier_wait_ns);
    if (rc != OK) {
        return rc;
    }

    uint32_t status_word = m_core_notifier.status_word();
    rc = stop_notifier_writes(*m_core_queue);
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

int32_t display_engine::take_down(const display_heads& heads, uint32_t present_windows, const active_heads& active) {
    shutdown_methods methods = {};
    build_shutdown_methods(heads, present_windows, active, !m_has_sent_update, methods);

    int32_t rc = write_methods(*m_core_queue, methods.methods, methods.count);
    if (rc != OK) {
        return rc;
    }

    rc = write_update(*m_core_queue, 0);
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
    m_core_queue = nullptr;

    free_output_modes(m_output_modes);

    bool keeps_hardware = !m_has_sent_update;
    int32_t window_channel_rc = OK;
    if (m_window_channel != 0) {
        window_channel_rc = release_display_channel(m_firmware, m_objects.display, m_window_channel, keeps_hardware);
        m_window_channel = 0;
    }

    int32_t core_channel_rc = OK;
    if (m_core_channel != 0) {
        core_channel_rc = release_display_channel(m_firmware, m_objects.display, m_core_channel, keeps_hardware);
        m_core_channel = 0;
    }

    int32_t objects_rc = OK;
    if (m_objects.display_common != 0) {
        objects_rc = release_display_objects(m_firmware, m_objects);
        m_objects = {};
    }

    if (window_channel_rc != OK) {
        return window_channel_rc;
    }

    if (core_channel_rc != OK) {
        return core_channel_rc;
    }

    return objects_rc;
}

} // namespace drivers::nvidia
