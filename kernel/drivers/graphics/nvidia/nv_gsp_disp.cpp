#include "drivers/graphics/nvidia/nv_gsp_disp.h"
#include "drivers/graphics/nvidia/nv_gpu.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "common/string.h"

namespace nv {

// ============================================================================
// EDID Parsing (reuse from Phase 3 — forward declaration)
// ============================================================================

// parse_edid is defined in nv_gpu.cpp — we need it here too.
// For now, inline a minimal version. The full parser is in nv_gpu.
static int32_t parse_edid_data(const uint8_t* raw, edid_info* out) {
    out->valid = false;

    // Validate EDID header
    static const uint8_t hdr[] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
    for (int i = 0; i < 8; i++) {
        if (raw[i] != hdr[i]) return ERR_INVALID;
    }

    // Checksum
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++) sum += raw[i];
    if (sum != 0) return ERR_CHECKSUM;

    // Manufacturer
    uint16_t mfg = (static_cast<uint16_t>(raw[8]) << 8) | raw[9];
    out->manufacturer[0] = static_cast<char>(((mfg >> 10) & 0x1F) + 'A' - 1);
    out->manufacturer[1] = static_cast<char>(((mfg >> 5) & 0x1F) + 'A' - 1);
    out->manufacturer[2] = static_cast<char>((mfg & 0x1F) + 'A' - 1);
    out->manufacturer[3] = '\0';

    out->product_code = static_cast<uint16_t>(raw[10]) | (static_cast<uint16_t>(raw[11]) << 8);
    out->serial_number = static_cast<uint32_t>(raw[12]) | (static_cast<uint32_t>(raw[13]) << 8) |
                         (static_cast<uint32_t>(raw[14]) << 16) | (static_cast<uint32_t>(raw[15]) << 24);
    out->mfg_week = raw[16];
    out->mfg_year = raw[17];

    out->display_name[0] = '\0';
    bool got_timing = false;

    for (int block = 0; block < 4; block++) {
        uint32_t base = 54 + block * 18;
        uint16_t pixel_clock = static_cast<uint16_t>(raw[base]) | (static_cast<uint16_t>(raw[base+1]) << 8);

        if (pixel_clock != 0 && !got_timing) {
            out->pixel_clock_khz = pixel_clock * 10;
            out->h_active = raw[base+2] | ((raw[base+4] & 0xF0) << 4);
            out->h_blanking = raw[base+3] | ((raw[base+4] & 0x0F) << 8);
            out->v_active = raw[base+5] | ((raw[base+7] & 0xF0) << 4);
            out->v_blanking = raw[base+6] | ((raw[base+7] & 0x0F) << 8);
            out->h_sync_offset = raw[base+8] | ((raw[base+11] & 0xC0) << 2);
            out->h_sync_width = raw[base+9] | ((raw[base+11] & 0x30) << 4);
            out->v_sync_offset = ((raw[base+10] >> 4) & 0x0F) | ((raw[base+11] & 0x0C) << 2);
            out->v_sync_width = (raw[base+10] & 0x0F) | ((raw[base+11] & 0x03) << 4);
            out->h_sync_positive = (raw[base+17] & 0x02) != 0;
            out->v_sync_positive = (raw[base+17] & 0x04) != 0;

            uint32_t htotal = out->h_active + out->h_blanking;
            uint32_t vtotal = out->v_active + out->v_blanking;
            if (htotal > 0 && vtotal > 0)
                out->refresh_hz = (out->pixel_clock_khz * 1000) / (htotal * vtotal);
            got_timing = true;
        } else if (pixel_clock == 0 && raw[base+3] == 0xFC) {
            for (int j = 0; j < 13; j++) {
                char c = static_cast<char>(raw[base+5+j]);
                if (c == '\n' || c == '\0') { out->display_name[j] = '\0'; break; }
                out->display_name[j] = c;
                out->display_name[j+1] = '\0';
            }
        }
    }

    if (!got_timing) return ERR_INVALID;
    out->valid = true;
    return OK;
}

// ============================================================================
// Display Probe
// ============================================================================

int32_t gsp_display_probe(nv_gpu* gpu, gsp_boot_state& boot,
                           rm_state& rm, display_state& disp) {
    log::info("nvidia: ========================================");
    log::info("nvidia: Phase E: Display discovery via GSP-RM");
    log::info("nvidia: ========================================");

    string::memset(&disp, 0, sizeof(disp));
    disp.initialized = false;

    int32_t rc;

    // Step 1: Get number of heads
    disp_get_num_heads_params heads_p;
    string::memset(&heads_p, 0, sizeof(heads_p));
    rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                    NV0073_CMD_SYSTEM_GET_NUM_HEADS, &heads_p, sizeof(heads_p));
    if (rc == OK) {
        disp.num_heads = heads_p.num_heads;
        log::info("nvidia: disp: num_heads = %u", disp.num_heads);
    } else {
        log::warn("nvidia: disp: GET_NUM_HEADS failed (%d), assuming 4", rc);
        disp.num_heads = 4;
    }

    // Step 2: Get supported display mask
    disp_get_supported_params sup_p;
    string::memset(&sup_p, 0, sizeof(sup_p));
    rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                    NV0073_CMD_SYSTEM_GET_SUPPORTED, &sup_p, sizeof(sup_p));
    if (rc != OK) {
        log::error("nvidia: disp: GET_SUPPORTED failed: %d", rc);
        return rc;
    }

    disp.supported_mask = sup_p.display_mask;
    disp.ddc_mask = sup_p.display_mask_ddc;
    log::info("nvidia: disp: supported=0x%08x DDC=0x%08x",
              disp.supported_mask, disp.ddc_mask);

    // Step 3: Get connected state
    disp_get_connect_state_params conn_p;
    string::memset(&conn_p, 0, sizeof(conn_p));
    conn_p.display_mask = disp.supported_mask;
    rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                    NV0073_CMD_SYSTEM_GET_CONNECT_STATE, &conn_p, sizeof(conn_p));
    if (rc == OK) {
        disp.connected_mask = conn_p.display_mask;
        log::info("nvidia: disp: connected=0x%08x", disp.connected_mask);
    } else {
        log::warn("nvidia: disp: GET_CONNECT_STATE failed (%d), using supported mask", rc);
        disp.connected_mask = disp.supported_mask;
    }

    // Step 4: Tell GSP we manage DP link training manually
    if (disp.supported_mask != 0) {
        dp_set_manual_dp_params manual_p;
        string::memset(&manual_p, 0, sizeof(manual_p));
        manual_p.display_id = disp.supported_mask; // All supported displays
        rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                        NV0073_CMD_DP_SET_MANUAL_DISPLAYPORT, &manual_p, sizeof(manual_p));
        if (rc == OK) {
            log::info("nvidia: disp: DP_SET_MANUAL_DISPLAYPORT set for mask 0x%08x",
                      disp.supported_mask);
        } else {
            log::warn("nvidia: disp: DP_SET_MANUAL_DISPLAYPORT failed (%d, non-fatal)", rc);
        }
    }

    // Step 5: For each connected display, get OR info and read EDID
    disp.display_count = 0;

    for (uint32_t bit = 0; bit < 32 && disp.display_count < MAX_DISPLAYS; bit++) {
        uint32_t display_id = 1u << bit;
        if (!(disp.connected_mask & display_id)) continue;

        display_info& d = disp.displays[disp.display_count];
        string::memset(&d, 0, sizeof(d));
        d.display_id = display_id;
        d.connected = true;

        log::info("nvidia: disp: probing display 0x%08x...", display_id);

        // Get OR info
        or_get_info_params or_p;
        string::memset(&or_p, 0, sizeof(or_p));
        or_p.display_id = display_id;
        rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                        NV0073_CMD_SPECIFIC_OR_GET_INFO, &or_p, sizeof(or_p));
        if (rc == OK) {
            d.or_index = or_p.index;
            d.or_type = or_p.type;
            d.or_protocol = or_p.protocol;
            d.dcb_index = or_p.dcb_index;
            log::info("nvidia: disp:   OR: index=%u type=%u protocol=0x%x dcb=%u%s",
                      d.or_index, d.or_type, d.or_protocol, d.dcb_index,
                      or_p.b_is_lit_by_vbios ? " [lit by VBIOS]" : "");
        } else {
            log::warn("nvidia: disp:   OR_GET_INFO failed: %d", rc);
        }

        // Read EDID
        uint8_t edid_raw[256]; // Stack OK — 256 bytes < 512 limit
        uint32_t edid_sz = 0;
        rc = gsp_display_read_edid(gpu, boot, rm, display_id, edid_raw, &edid_sz);
        if (rc == OK && edid_sz >= 128) {
            d.has_edid = true;
            parse_edid_data(edid_raw, &d.edid);

            if (d.edid.valid) {
                log::info("nvidia: disp:   Monitor: %s",
                          d.edid.display_name[0] ? d.edid.display_name : "(unnamed)");
                log::info("nvidia: disp:   Manufacturer: %s Product: 0x%04x",
                          d.edid.manufacturer, d.edid.product_code);
                log::info("nvidia: disp:   Native mode: %ux%u @%uHz (pixel clock %u kHz)",
                          d.edid.h_active, d.edid.v_active,
                          d.edid.refresh_hz, d.edid.pixel_clock_khz);
            } else {
                log::warn("nvidia: disp:   EDID parse failed");
            }
        } else {
            log::info("nvidia: disp:   no EDID (rc=%d size=%u)", rc, edid_sz);
        }

        disp.display_count++;
    }

    disp.initialized = true;

    log::info("nvidia: ========================================");
    log::info("nvidia: Display discovery complete:");
    log::info("nvidia:   %u display(s) found, %u connected",
              disp.display_count, disp.display_count);
    for (uint32_t i = 0; i < disp.display_count; i++) {
        const display_info& d = disp.displays[i];
        const char* proto = (d.or_protocol == OR_PROTOCOL_SOR_DP_A) ? "DP" :
                            (d.or_protocol == OR_PROTOCOL_SOR_TMDS_A) ? "TMDS" :
                            (d.or_protocol == OR_PROTOCOL_SOR_DUAL_TMDS) ? "TMDS-DL" : "?";
        log::info("nvidia:   [%u] id=0x%08x %s SOR%u %s%s",
                  i, d.display_id, proto, d.or_index,
                  d.has_edid ? d.edid.display_name : "(no EDID)",
                  d.has_edid && d.edid.valid ?
                      "" : " (EDID invalid)");
        if (d.has_edid && d.edid.valid) {
            log::info("nvidia:       %ux%u @%uHz",
                      d.edid.h_active, d.edid.v_active, d.edid.refresh_hz);
        }
    }
    log::info("nvidia: ========================================");

    return OK;
}

// ============================================================================
// EDID Read via GSP-RM
// ============================================================================

int32_t gsp_display_read_edid(nv_gpu* gpu, gsp_boot_state& boot,
                               rm_state& rm, uint32_t display_id,
                               uint8_t* edid_out, uint32_t* edid_size) {
    // disp_get_edid_v2_params is 2064 bytes — must heap allocate!
    disp_get_edid_v2_params* params = static_cast<disp_get_edid_v2_params*>(
        heap::uzalloc(sizeof(disp_get_edid_v2_params)));
    if (!params) {
        log::error("nvidia: disp: failed to allocate EDID params");
        return ERR_NOT_FOUND;
    }

    params->sub_device_instance = 0;
    params->display_id = display_id;
    params->buffer_size = 2048;
    params->flags = 0;

    int32_t rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                            NV0073_CMD_SPECIFIC_GET_EDID_V2,
                            params, sizeof(disp_get_edid_v2_params));
    if (rc != OK) {
        heap::ufree(params);
        return rc;
    }

    uint32_t actual_size = params->buffer_size;
    if (actual_size > 2048) actual_size = 2048;
    if (actual_size > 0 && edid_out) {
        uint32_t copy = (actual_size < 256) ? actual_size : 256; // Only first block for now
        string::memcpy(edid_out, params->edid_buffer, copy);
    }
    if (edid_size) *edid_size = actual_size;

    log::info("nvidia: disp: EDID read: display=0x%08x size=%u bytes", display_id, actual_size);

    heap::ufree(params);
    return OK;
}

// ============================================================================
// DP AUX via GSP-RM
// ============================================================================

int32_t gsp_display_dp_aux(nv_gpu* gpu, gsp_boot_state& boot,
                            rm_state& rm, uint32_t display_id,
                            uint32_t cmd, uint32_t addr,
                            uint8_t* data, uint32_t* size, uint32_t* reply) {
    dp_auxch_ctrl_params params;
    string::memset(&params, 0, sizeof(params));

    params.sub_device_instance = 0;
    params.display_id = display_id;
    params.cmd = cmd;
    params.addr = addr;
    params.size = *size;
    params.retry_time_ms = 100;

    // Copy input data for write operations
    if (data && *size > 0 && *size <= 16) {
        string::memcpy(params.data, data, *size);
    }

    int32_t rc = rm_control(gpu, boot, rm.h_client, rm.h_display,
                            NV0073_CMD_DP_AUXCH_CTRL, &params, sizeof(params));
    if (rc != OK) return rc;

    // Copy output
    if (data && params.size > 0 && params.size <= 16) {
        string::memcpy(data, params.data, params.size);
    }
    *size = params.size;
    if (reply) *reply = params.reply_type;

    return OK;
}

} // namespace nv
