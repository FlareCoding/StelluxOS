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

namespace {

// What one read of an output's EDID gives
enum class read_result {
    parsed,
    bad_checksum,
    failed,
};

} // namespace

// What the log says of an EDID `edid::parse` refuses with `rc`
static const char* parse_failure(int32_t rc) {
    switch (rc) {
        case edid::ERR_TOO_SHORT:
            return "is shorter than one block";
        case edid::ERR_BAD_HEADER:
            return "does not start with the EDID header";
        case edid::ERR_BAD_VERSION:
            return "is not of version 1";
    }

    return "fails to parse";
}

static void wait_before_reread() {
    uint64_t end = clock::now_ns() + EDID_REREAD_DELAY_US * NS_PER_US;
    while (clock::now_ns() < end) {
        cpu::relax();
    }
}

// Asks once for the EDID, the parameters zeroed but for the display id, and parses the reply's EDID into `out`, logging
// why the EDID cannot be used unless it parses or fails only its checksum
static int32_t read_once(
    firmware& fw,
    rm::handle display_common,
    uint32_t display_id,
    rm::edid_params& params,
    edid::monitor& out,
    read_result& result
) {
    result = read_result::failed;
    string::memset(&params, 0, sizeof(params));
    params.display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_EDID, &params, sizeof(params));
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

    int32_t parse_rc = edid::parse(params.edid, params.edid_bytes, out);
    if (parse_rc == edid::OK) {
        result = read_result::parsed;
    } else if (parse_rc == edid::ERR_BAD_CHECKSUM) {
        result = read_result::bad_checksum;
    } else {
        log::error(
            "nvidia: display id 0x%X's EDID of %u bytes %s",
            display_id,
            params.edid_bytes,
            parse_failure(parse_rc)
        );
    }

    return OK;
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
    read_result result = read_result::failed;
    uint32_t reads = 0;
    while (true) {
        reads++;
        rc = read_once(fw, display_common, display_id, *params, out, result);
        bool failed_only_checksum = rc == OK && result == read_result::bad_checksum;
        bool has_reread_left = reads <= EDID_REREADS_MAX;
        if (!failed_only_checksum || !has_reread_left) {
            break;
        }

        wait_before_reread();
    }

    heap::ufree(params);

    if (rc != OK) {
        return rc;
    }

    switch (result) {
        case read_result::parsed:
            if (reads > 1) {
                log::info("nvidia: display id 0x%X's EDID passed its checksum on read %u", display_id, reads);
            }

            break;
        case read_result::bad_checksum:
            log::error("nvidia: display id 0x%X's EDID fails its checksum on each of its %u reads", display_id, reads);
            break;
        case read_result::failed:
            break;
    }

    is_parsed = result == read_result::parsed;

    return OK;
}

} // namespace drivers::nvidia
