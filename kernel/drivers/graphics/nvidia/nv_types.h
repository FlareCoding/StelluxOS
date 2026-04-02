#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_TYPES_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_TYPES_H

#include "common/types.h"

namespace nv {

// PCI identification
constexpr uint16_t PCI_VENDOR_NVIDIA = 0x10DE;
constexpr uint8_t  PCI_CLASS_DISPLAY = 0x03;
constexpr uint8_t  PCI_SUBCLASS_VGA  = 0x00;
constexpr uint8_t  PCI_SUBCLASS_3D   = 0x02;

// GPU architecture families (from PMC_BOOT_0 chipset & 0x1f0)
enum class gpu_family : uint8_t {
    UNKNOWN  = 0,
    TESLA    = 1,  // NV50  (0x050-0x0af)
    FERMI    = 2,  // GF100 (0x0c0-0x0ff)
    KEPLER   = 3,  // GK100 (0x0e0-0x0ff, 0x100-0x12f)
    MAXWELL  = 4,  // GM100 (0x110-0x13f)
    PASCAL   = 5,  // GP100 (0x130-0x13f)
    VOLTA    = 6,  // GV100 (0x140-0x14f)
    TURING   = 7,  // TU100 (0x160-0x16f)
    AMPERE   = 8,  // GA100 (0x170-0x17f)
    ADA      = 9,  // AD100 (0x190-0x19f)
};

// Display output types (from DCB device entry bits [3:0])
enum class dcb_output_type : uint8_t {
    ANALOG  = 0x0, // CRT / VGA
    TV      = 0x1, // TV encoder
    TMDS    = 0x2, // DVI / HDMI
    LVDS    = 0x3, // Laptop panel
    DP      = 0x6, // DisplayPort
    WFD     = 0x8, // WiFi Display
    EOL     = 0xE, // End of list marker
    UNUSED  = 0xF, // Skip entry
};

// DCB connector types (from connector table entry bits [7:0])
enum class dcb_connector_type : uint8_t {
    VGA          = 0x00,
    DVI_A        = 0x01,
    TV_COMPOSITE = 0x10,
    TV_SVIDEO    = 0x11,
    TV_HDTV      = 0x13,
    DVI_I        = 0x30,
    DVI_D        = 0x31,
    LVDS_SPWG    = 0x40,
    LVDS_OEM     = 0x41,
    DP_EXT       = 0x46,
    DP_INT       = 0x47,
    MDP_EXT      = 0x48,
    HDMI_A       = 0x61,
    HDMI_C       = 0x63,
    SKIP         = 0xFF,
};

// DCB I2C access method types (from CCB entry upper byte)
enum class dcb_i2c_type : uint8_t {
    NV04_BIT  = 0x00, // Legacy dual-register
    NV4E_BIT  = 0x04, // NV4E bit-bang
    NVIO_BIT  = 0x05, // NVIO I2C bit-bang (GF100+)
    NVIO_AUX  = 0x06, // NVIO DP AUX channel
    PMGR      = 0x80, // PMGR-controlled (GM20x+)
    UNUSED    = 0xFF, // Unused entry
};

// Error codes
constexpr int32_t OK              =  0;
constexpr int32_t ERR_NOT_FOUND   = -1;
constexpr int32_t ERR_TIMEOUT     = -2;
constexpr int32_t ERR_MAP_FAILED  = -3;
constexpr int32_t ERR_INVALID     = -4;
constexpr int32_t ERR_UNSUPPORTED = -5;
constexpr int32_t ERR_IO          = -6;
constexpr int32_t ERR_NACK        = -7;
constexpr int32_t ERR_CHECKSUM    = -8;
constexpr int32_t ERR_NO_DISPLAY  = -9;

// Maximum counts
constexpr uint32_t MAX_DCB_ENTRIES    = 16;
constexpr uint32_t MAX_I2C_PORTS      = 16;
constexpr uint32_t MAX_CONNECTORS     = 8;
constexpr uint32_t MAX_HEADS          = 4;
constexpr uint32_t MAX_SORS           = 8;
constexpr uint32_t VBIOS_MAX_SIZE     = 1024 * 1024; // 1MB max VBIOS (PROM window is 1MB)
constexpr uint32_t EDID_BLOCK_SIZE    = 128;

// DCB parsed entry
struct dcb_entry {
    dcb_output_type type;
    uint8_t i2c_index;    // I2C/CCB port index for DDC
    uint8_t head_mask;    // Which heads can drive this output
    uint8_t connector;    // Physical connector table index
    uint8_t bus;          // Logical bus for mutual exclusion
    uint8_t location;     // 0=on-chip, 1=on-board
    uint8_t or_mask;      // Output resource (DAC/SOR/PIOR) assignment
    // DFP-specific (TMDS/DP)
    uint8_t link;         // Sub-link assignment
    bool    hdmi_enable;  // HDMI mode enabled
    uint8_t dp_max_rate;  // DP max link rate code
    uint8_t dp_max_lanes; // DP max lane mask
    uint32_t raw[2];      // Raw DWORD0/DWORD1 for debugging
};

// I2C port entry from CCB
struct i2c_port {
    dcb_i2c_type type;
    uint8_t port;         // Physical port number
    uint8_t aux_port;     // DP AUX port (for shared pads)
    bool    hybrid;       // Shared I2C/AUX pad
    bool    valid;        // Entry is usable
};

// Connector table entry
struct connector_entry {
    dcb_connector_type type;
    uint8_t location;
    uint16_t flags;       // Hotplug, DP2DVI, etc.
};

// EDID parsed information
struct edid_info {
    bool    valid;
    char    manufacturer[4];  // 3-letter code + NUL
    uint16_t product_code;
    uint32_t serial_number;
    uint8_t  mfg_week;
    uint8_t  mfg_year;        // Year since 1990

    // Preferred/native timing (from first Detailed Timing Descriptor)
    uint32_t pixel_clock_khz; // Pixel clock in kHz (10kHz units * 10)
    uint16_t h_active;
    uint16_t h_blanking;
    uint16_t h_sync_offset;
    uint16_t h_sync_width;
    uint16_t v_active;
    uint16_t v_blanking;
    uint16_t v_sync_offset;
    uint16_t v_sync_width;
    bool     h_sync_positive;
    bool     v_sync_positive;

    // Display name (from descriptor block type 0xFC)
    char display_name[14];

    // Computed
    uint32_t refresh_hz;      // Approximate refresh rate in Hz
};

// VBIOS DCB summary
struct dcb_table {
    dcb_entry entries[MAX_DCB_ENTRIES];
    uint8_t   count;
    // Pointers into VBIOS (absolute offsets)
    uint16_t  i2c_table_ptr;
    uint16_t  gpio_table_ptr;
    uint16_t  connector_table_ptr;
};

// I2C port table
struct i2c_table {
    i2c_port ports[MAX_I2C_PORTS];
    uint8_t  count;
};

// Connector table
struct connector_table {
    connector_entry entries[MAX_CONNECTORS];
    uint8_t count;
};

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_TYPES_H
