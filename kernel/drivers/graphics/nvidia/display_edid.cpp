// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/cpu.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// What the log says of an EDID `edid::parse` refuses with `rc`
static const char* parse_failure(int32_t rc) {
    switch (rc) {
        case edid::ERR_TOO_SHORT:
            return "is shorter than one block";
        case edid::ERR_BAD_HEADER:
            return "does not start with the EDID header";
        case edid::ERR_BAD_CHECKSUM:
            return "fails its checksum";
        case edid::ERR_BAD_VERSION:
            return "is not of version 1";
    }

    return "fails to parse";
}

// A base block cut short or corrupted, or a CTA-861 or DisplayID block failing its checksum, may be the cable's doing
static bool is_worth_rereading(int32_t parse_rc, const edid::monitor& monitor) {
    switch (parse_rc) {
        case edid::OK:
            return monitor.bad_checksum_extensions > 0;
        case edid::ERR_TOO_SHORT:
        case edid::ERR_BAD_HEADER:
        case edid::ERR_BAD_CHECKSUM:
            return true;
    }

    return false;
}

static void wait_before_reread() {
    uint64_t end = clock::now_ns() + EDID_REREAD_DELAY_US * NS_PER_US;
    while (clock::now_ns() < end) {
        cpu::relax();
    }
}

// Sets `parse_rc` only along with `has_edid`, for a reply holding all the EDID bytes it reports
static int32_t read_once(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    rm::edid_params& params,
    edid::monitor& out,
    bool& has_edid,
    int32_t& parse_rc
) {
    has_edid = false;
    string::memset(&params, 0, sizeof(params));
    params.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_EDID, &params, sizeof(params));

    // A refusal ends the read, where NVIDIA's modesetting module asks again for a DisplayPort monitor, since the
    // rereads are for bytes the cable may spoil and a refusal brings none
    if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display common refused display id 0x%X's EDID", display_id);
        return OK;
    }

    if (rc != OK) {
        return rc;
    }

    if (params.edid_bytes > rm::EDID_BUFFER_BYTES) {
        log::error(
            "nvidia: display common reports display id 0x%X's EDID as %u bytes, more than the %u its reply holds",
            display_id,
            params.edid_bytes,
            rm::EDID_BUFFER_BYTES
        );
        return OK;
    }

    has_edid = true;
    parse_rc = edid::parse(params.edid, params.edid_bytes, out);

    return OK;
}

// Only the last read is logged, since each read's cause may differ and the last one decides
static void log_last_read(
    uint32_t display_id,
    uint32_t edid_bytes,
    int32_t parse_rc,
    const edid::monitor& monitor,
    uint32_t reads
) {
    if (parse_rc != edid::OK && reads == 1) {
        log::error("nvidia: display id 0x%X's %u-byte EDID %s", display_id, edid_bytes, parse_failure(parse_rc));
        return;
    }

    if (parse_rc != edid::OK) {
        log::error(
            "nvidia: display id 0x%X's %u-byte EDID %s on read %u, the last",
            display_id,
            edid_bytes,
            parse_failure(parse_rc),
            reads
        );
        return;
    }

    if (monitor.bad_checksum_extensions > 0) {
        log::warn(
            "nvidia: display id 0x%X's EDID parsed with %u %s skipped for a bad checksum on read %u, the last",
            display_id,
            monitor.bad_checksum_extensions,
            monitor.bad_checksum_extensions == 1 ? "extension block" : "extension blocks",
            reads
        );
        return;
    }

    if (reads > 1) {
        log::info(
            "nvidia: display id 0x%X's EDID parsed with no block skipped for a bad checksum on read %u",
            display_id,
            reads
        );
    }
}

int32_t read_output_edid(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    edid::monitor& out,
    bool& is_parsed
) {
    is_parsed = false;

    // The parameters' 2 KiB of EDID would take too much of a kernel stack
    uint32_t params_bytes = sizeof(rm::edid_params);
    auto* params = static_cast<rm::edid_params*>(heap::uzalloc(params_bytes));
    if (params == nullptr) {
        log::error("nvidia: allocating %u bytes for display id 0x%X's EDID request failed", params_bytes, display_id);
        return ERR_NO_MEMORY;
    }

    int32_t rc = OK;
    bool has_edid = false;
    int32_t parse_rc = edid::OK;
    uint32_t reads = 0;
    while (true) {
        reads++;
        rc = read_once(fw, display_common, display_id, *params, out, has_edid, parse_rc);
        bool should_reread = rc == OK && has_edid && is_worth_rereading(parse_rc, out);
        bool has_reread_left = reads <= EDID_REREADS_MAX;
        if (!should_reread || !has_reread_left) {
            break;
        }

        wait_before_reread();
    }

    uint32_t edid_bytes = params->edid_bytes;
    heap::ufree(params);

    if (rc != OK || !has_edid) {
        return rc;
    }

    log_last_read(display_id, edid_bytes, parse_rc, out, reads);

    // A base block failing its checksum on the last read leaves `is_parsed` cleared, since a corrupted base block could
    // drive the monitor out of range
    is_parsed = parse_rc == edid::OK;

    return OK;
}

} // namespace drivers::nvidia
