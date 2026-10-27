// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/display_edid.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/rm.h"
#include "common/logging.h"
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

// Parses the EDID a reply carries into `out` and sets `is_parsed`, or logs why the EDID cannot be used
static void parse_reply(uint32_t display_id, const rm::edid_params& reply, edid::monitor& out, bool& is_parsed) {
    if (reply.edid_bytes > rm::EDID_BUFFER_BYTES) {
        log::error(
            "nvidia: display common reports display id 0x%X's EDID as %u bytes, more than the %u its reply holds",
            display_id,
            reply.edid_bytes,
            rm::EDID_BUFFER_BYTES
        );
        return;
    }

    int32_t rc = edid::parse(reply.edid, reply.edid_bytes, out);
    if (rc != edid::OK) {
        log::error("nvidia: display id 0x%X's EDID of %u bytes %s", display_id, reply.edid_bytes, parse_failure(rc));
        return;
    }

    is_parsed = true;
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

    params->display_id = display_id;
    int32_t rc = fw.control(display_common, rm::CONTROL_GET_EDID, params, params_bytes);
    if (rc == OK) {
        parse_reply(display_id, *params, out, is_parsed);
    } else if (rc == ERR_FIRMWARE) {
        log::error("nvidia: display common refused display id 0x%X's EDID", display_id);
        rc = OK;
    }

    heap::ufree(params);

    return rc;
}

} // namespace drivers::nvidia
