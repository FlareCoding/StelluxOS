// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_ids.h"
#include "drivers/graphics/nvidia/display_info.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

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

// Logs the bytes of the DPCD capabilities an output's monitor reports, with its PHY repeaters'
static void log_dpcd(uint8_t output_index, const dpcd_capabilities& dpcd) {
    char receiver[DPCD_RECEIVER_CAPABILITY_BYTES * 3];
    char phy_repeaters[DPCD_PHY_REPEATER_CAPABILITY_BYTES * 3];
    format_bytes(dpcd.receiver, DPCD_RECEIVER_CAPABILITY_BYTES, receiver);
    format_bytes(dpcd.phy_repeaters, DPCD_PHY_REPEATER_CAPABILITY_BYTES, phy_repeaters);
    uint32_t receiver_address = dpcd.is_extended ? DPCD_EXTENDED_RECEIVER_CAPABILITIES : DPCD_RECEIVER_CAPABILITIES;

    log::info(
        "nvidia: output %u's monitor reports the DPCD capabilities %s from 0x%05X",
        output_index,
        receiver,
        receiver_address
    );
    log::info(
        "nvidia: output %u's PHY repeater capabilities from 0x%05X are %s",
        output_index,
        DPCD_PHY_REPEATER_CAPABILITIES,
        phy_repeaters
    );
}

// Reads the EDID and the DPCD capabilities of each connected output's monitor, the EDID into one `edid::monitor` on the
// unprivileged heap, as it is too large for the stack, and logs each EDID that parses and the capabilities read
static int32_t read_connected_monitors(firmware& fw, rm::handle display_common, const display_outputs& outputs) {
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
        rc = read_output_dpcd(fw, display_common, output.display_id, dpcd, is_read);
        if (rc != OK) {
            break;
        }

        if (is_read) {
            log_dpcd(index, dpcd);
        }
    }

    heap::ufree(monitor);

    return rc;
}

static int32_t read_display_facts(firmware& fw, const display_objects& objects, const chip_display& display) {
    bool has_crossbar = false;
    int32_t rc = read_display_capabilities(fw, objects, has_crossbar);
    if (rc != OK) {
        return rc;
    }

    log::info(
        "nvidia: the display engine can reach push buffers, semaphores and notifiers in system memory, and display "
        "common reports %s",
        has_crossbar ? "a crossbar" : "no crossbar"
    );

    display_heads heads = {};
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

    display_outputs outputs = {};
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
    }

    return read_connected_monitors(fw, objects.display_common, outputs);
}

display_engine::display_engine(firmware& fw, video_memory& memory)
    : m_firmware(fw)
    , m_instance_memory(memory, fw) {}

int32_t display_engine::set_up(
    const registers& regs,
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

    rc = read_display_facts(m_firmware, m_objects, display);
    if (rc != OK) {
        return rc;
    }

    size_t push_buffer_pages = pmm::page_align_up(PUSH_BUFFER_BYTES) / pmm::PAGE_SIZE;
    RUN_ELEVATED(
        rc = dma::alloc_pages(
            card,
            push_buffer_pages,
            dma::caching::cached_when_coherent,
            paging::PAGE_USER,
            m_core_push_buffer
        )
    );
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

    return OK;
}

int32_t display_engine::release() {
    int32_t core_channel_rc = OK;
    if (m_core_channel != 0) {
        core_channel_rc = release_core_channel(m_firmware, m_objects.display, m_core_channel);
        m_core_channel = 0;
    }

    int32_t objects_rc = OK;
    if (m_objects.display_common != 0) {
        objects_rc = release_display_objects(m_firmware, m_objects);
        m_objects = {};
    }

    if (core_channel_rc != OK) {
        return core_channel_rc;
    }

    return objects_rc;
}

} // namespace drivers::nvidia
