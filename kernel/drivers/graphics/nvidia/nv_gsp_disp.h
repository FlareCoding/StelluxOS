#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_DISP_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_DISP_H

#include "common/types.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_rpc.h"
#include "drivers/graphics/nvidia/nv_gsp_boot.h"

namespace nv {

class nv_gpu;

// ============================================================================
// Display RM Control Commands
// ============================================================================

constexpr uint32_t NV0073_CMD_SYSTEM_GET_SUPPORTED     = 0x00730120;
constexpr uint32_t NV0073_CMD_SYSTEM_GET_NUM_HEADS     = 0x00730102;
constexpr uint32_t NV0073_CMD_SYSTEM_GET_CONNECT_STATE = 0x00730122;
constexpr uint32_t NV0073_CMD_SPECIFIC_GET_EDID_V2     = 0x00730245;
constexpr uint32_t NV0073_CMD_SPECIFIC_GET_CONNECTOR_DATA = 0x00730250;
constexpr uint32_t NV0073_CMD_SPECIFIC_OR_GET_INFO     = 0x0073028B;
constexpr uint32_t NV0073_CMD_DP_AUXCH_CTRL            = 0x00731341;
constexpr uint32_t NV0073_CMD_DP_GET_CAPS              = 0x00731369;

// ============================================================================
// Display RM Control Parameter Structs
// ============================================================================

// GET_SUPPORTED — query bitmask of available displays
struct __attribute__((packed)) disp_get_supported_params {
    uint32_t sub_device_instance; // IN: 0
    uint32_t display_mask;        // OUT: bitmask of supported displays
    uint32_t display_mask_ddc;    // OUT: displays with DDC capability
};
static_assert(sizeof(disp_get_supported_params) == 12);

// GET_NUM_HEADS
struct __attribute__((packed)) disp_get_num_heads_params {
    uint32_t sub_device_instance; // IN: 0
    uint32_t flags;               // IN: 0
    uint32_t num_heads;           // OUT
};
static_assert(sizeof(disp_get_num_heads_params) == 12);

// GET_CONNECT_STATE
struct __attribute__((packed)) disp_get_connect_state_params {
    uint32_t sub_device_instance; // IN: 0
    uint32_t flags;               // IN: 0
    uint32_t display_mask;        // IN/OUT: query mask → connected mask
    uint32_t retry_time_ms;       // IN: 0
};
static_assert(sizeof(disp_get_connect_state_params) == 16);

// GET_EDID_V2 — read EDID from a display (large struct — heap allocate!)
struct __attribute__((packed)) disp_get_edid_v2_params {
    uint32_t sub_device_instance; // IN: 0
    uint32_t display_id;          // IN: single-bit display ID
    uint32_t buffer_size;         // IN/OUT: max 2048, GSP fills actual
    uint32_t flags;               // IN: 0
    uint8_t  edid_buffer[2048];   // OUT: raw EDID data
};
static_assert(sizeof(disp_get_edid_v2_params) == 2064);

// DP_AUXCH_CTRL — DisplayPort AUX channel transaction
struct __attribute__((packed)) dp_auxch_ctrl_params {
    uint32_t sub_device_instance; // IN: 0
    uint32_t display_id;          // IN: display ID
    uint8_t  b_addr_only;         // IN: address-only transaction
    uint8_t  pad[3];
    uint32_t cmd;                 // IN: AUX command (see bit defs)
    uint32_t addr;                // IN: DPCD address
    uint8_t  data[16];            // IN/OUT: data buffer (max 16 bytes)
    uint32_t size;                // IN/OUT: transfer size
    uint32_t reply_type;          // OUT: AUX reply
    uint32_t retry_time_ms;       // IN: timeout
};
static_assert(sizeof(dp_auxch_ctrl_params) == 48);

// AUX command bits
constexpr uint32_t DP_AUX_CMD_TYPE_AUX  = (1 << 3); // Native AUX
constexpr uint32_t DP_AUX_CMD_I2C_MOT   = (1 << 2); // I2C middle-of-transaction
constexpr uint32_t DP_AUX_CMD_REQ_WRITE = 0;
constexpr uint32_t DP_AUX_CMD_REQ_READ  = 1;

// OR_GET_INFO — get output resource info for a display
struct __attribute__((packed)) or_get_info_params {
    uint32_t sub_device_instance;
    uint32_t display_id;
    uint32_t index;        // OUT: SOR/DAC/PIOR index
    uint32_t type;         // OUT: 0=NONE, 1=DAC, 2=SOR, 3=PIOR
    uint32_t protocol;     // OUT: 1=TMDS, 8=DP_A, etc.
    uint32_t dither_type;
    uint32_t dither_algo;
    uint32_t location;
    uint32_t root_port_id;
    uint32_t dcb_index;
    uint64_t vbios_address;
    uint8_t  b_is_lit_by_vbios;
    uint8_t  b_is_disp_dynamic;
    uint8_t  pad[6];       // Alignment padding
};

// OR protocol values
constexpr uint32_t OR_PROTOCOL_SOR_DP_A       = 0x08;
constexpr uint32_t OR_PROTOCOL_SOR_TMDS_A     = 0x01;
constexpr uint32_t OR_PROTOCOL_SOR_DUAL_TMDS  = 0x05;

// ============================================================================
// Display Discovery Results
// ============================================================================

struct display_info {
    uint32_t display_id;      // Single-bit display ID
    uint32_t or_index;        // Output resource index
    uint32_t or_type;         // 1=DAC, 2=SOR, 3=PIOR
    uint32_t or_protocol;     // 1=TMDS, 8=DP
    uint32_t dcb_index;       // DCB entry index
    bool     connected;       // Monitor detected
    bool     has_edid;        // EDID successfully read
    edid_info edid;           // Parsed EDID data
};

constexpr uint32_t MAX_DISPLAYS = 8;

struct display_state {
    uint32_t    supported_mask;   // Bitmask of supported display IDs
    uint32_t    connected_mask;   // Bitmask of connected displays
    uint32_t    ddc_mask;         // Displays with DDC capability
    uint32_t    num_heads;        // Number of display heads
    display_info displays[MAX_DISPLAYS];
    uint32_t    display_count;    // Number of valid entries in displays[]
    bool        initialized;
};

// ============================================================================
// Display API
// ============================================================================

/**
 * Discover and probe all connected displays via GSP-RM.
 * 1. Query supported displays (GET_SUPPORTED)
 * 2. Query connected state
 * 3. Get OR info for each connected display
 * 4. Read EDID for each connected display
 * 5. Parse EDID and populate display_state
 */
int32_t gsp_display_probe(nv_gpu* gpu, gsp_boot_state& boot,
                           rm_state& rm, display_state& disp);

/**
 * Read EDID from a specific display via GSP-RM.
 */
int32_t gsp_display_read_edid(nv_gpu* gpu, gsp_boot_state& boot,
                               rm_state& rm, uint32_t display_id,
                               uint8_t* edid_out, uint32_t* edid_size);

/**
 * Perform a DP AUX transaction via GSP-RM.
 */
int32_t gsp_display_dp_aux(nv_gpu* gpu, gsp_boot_state& boot,
                            rm_state& rm, uint32_t display_id,
                            uint32_t cmd, uint32_t addr,
                            uint8_t* data, uint32_t* size, uint32_t* reply);

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_DISP_H
