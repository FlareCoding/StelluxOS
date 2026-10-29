// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H

#include "common/types.h"

// The vocabulary of the resource manager inside the GSP's firmware (the part of NVIDIA's firmware that keeps the GPU's
// objects and carries out the commands to them, called controls), only what this driver uses
namespace drivers::nvidia::rm {

// NVIDIA's handle, the number naming an object the firmware keeps, such as a client (the owner of a tree of objects)
using handle = uint32_t;

// NVIDIA's classes of the objects standing for the driver and its GPU: a client (NV01_ROOT), a device (NV01_DEVICE_0, a
// group of GPUs working as one) and a subdevice (NV20_SUBDEVICE_0, one GPU of the group)
constexpr uint32_t CLASS_CLIENT    = 0x0;
constexpr uint32_t CLASS_DEVICE    = 0x80;
constexpr uint32_t CLASS_SUBDEVICE = 0x2080;

// NVIDIA's KERNEL_PID, the process number NVIDIA's driver names in the parameters of a client of the kernel
constexpr uint32_t KERNEL_PROCESS_ID = 0xFFFFFFFF;

// NVIDIA's NV_PROC_NAME_MAX_LENGTH, the room for a process's name in a client's allocation parameters
constexpr uint32_t PROCESS_NAME_MAX_BYTES = 100;

// NVIDIA's NV0000_ALLOC_PARAMETERS, the parameters of a client's allocation
struct client_params {
    handle   client;
    uint32_t process_id;
    char     process_name[PROCESS_NAME_MAX_BYTES];
};

// NVIDIA's NV0080_ALLOC_PARAMETERS, the parameters of a device's allocation. A device whose shared client is its own
// client gets a GPU virtual address space of its own
struct device_params {
    uint32_t device_index;
    handle   shared_client;
    handle   target_client;
    handle   target_device;
    uint32_t flags;
    uint64_t va_space_bytes;
    uint64_t va_start_internal;
    uint64_t va_limit_internal;
    uint32_t va_mode;
};

// NVIDIA's NV2080_ALLOC_PARAMETERS, the parameters of a subdevice's allocation
struct subdevice_params {
    uint32_t subdevice_index;
};

static_assert(sizeof(client_params) == 108, "NVIDIA's layout of a client's allocation parameters");
static_assert(sizeof(device_params) == 56, "NVIDIA's layout of a device's allocation parameters");
static_assert(sizeof(subdevice_params) == 4, "NVIDIA's layout of a subdevice's allocation parameters");

// NVIDIA's NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE, the control that gives the kernel interrupt table (for each
// GPU unit whose interrupts NVIDIA's driver services, the vectors on which they arrive)
constexpr uint32_t CONTROL_GET_KERNEL_INTERRUPT_TABLE = 0x20800A5C;

// The most rows the kernel interrupt table holds, NVIDIA's NV2080_CTRL_INTERNAL_INTR_MAX_TABLE_SIZE
constexpr uint32_t KERNEL_INTERRUPT_ROWS_MAX = 128;

// NVIDIA's MC_ENGINE_IDX_GSP, the GSP's number among the engines (the GPU's units that NVIDIA's interrupt code serves),
// which the GSP's row in the kernel interrupt table gives
constexpr uint16_t ENGINE_GSP = 49;

// A row of the kernel interrupt table: an engine, its PMC mask (NVIDIA's pmcIntrMask) and two GPU interrupt vectors
// (NVIDIA's numbers for the GPU's interrupts)
struct kernel_interrupt_row {
    uint16_t engine;
    uint32_t pmc_interrupt_mask; // the engine's bits in the GPU's PMC interrupt register, 0 for none
    uint32_t stall_vector;       // for the interrupts NVIDIA's driver services with the engine's handler
    uint32_t nonstall_vector;    // for the engine's notifications
};

// NVIDIA's NV2080_INTR_CATEGORY_SUBTREE_MAP, the first and last of the top-level bits a category of interrupts takes in
// the CPU interrupt tree (the GPU's registers reporting its interrupts to the CPU)
struct kernel_interrupt_subtree_range {
    uint8_t first;
    uint8_t last;
};

// NVIDIA's NV2080_INTR_CATEGORY_ENUM_COUNT, the categories of interrupts in the subtree map (for each category, a range
// of those top-level bits)
constexpr uint32_t KERNEL_INTERRUPT_CATEGORY_COUNT = 7;

// The parameters of `CONTROL_GET_KERNEL_INTERRUPT_TABLE`, which the reply fills. The subtree map is not read
struct kernel_interrupt_table_params {
    uint32_t                       row_count;
    kernel_interrupt_row           rows[KERNEL_INTERRUPT_ROWS_MAX];
    kernel_interrupt_subtree_range subtree_map[KERNEL_INTERRUPT_CATEGORY_COUNT];
};

static_assert(sizeof(kernel_interrupt_row) == 16, "NVIDIA's layout of a row of the kernel interrupt table");
static_assert(sizeof(kernel_interrupt_subtree_range) == 2, "NVIDIA's layout of a range of the subtree map");
static_assert(sizeof(kernel_interrupt_table_params) == 2068, "NVIDIA's layout of the kernel interrupt table");

// NVIDIA's NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO, the control that gives the display's static information
// (fixed facts about the display engine, such as its windows)
constexpr uint32_t CONTROL_GET_DISPLAY_STATIC_INFO = 0x20800A01;

// The parameters of `CONTROL_GET_DISPLAY_STATIC_INFO`, which the reply fills. Only the present windows are read
struct display_static_info_params {
    uint32_t system_capabilities; // NVIDIA's feHwSysCap, a copy of the display engine's capability register
    uint32_t present_windows;     // bit i set when this GPU has window i
    uint8_t  has_remapper;        // NVIDIA's bFbRemapperEnabled
    uint32_t head_count;
    uint8_t  is_primary_vga;
    uint32_t i2c_port;
    uint32_t internal_active_displays;
};

static_assert(sizeof(display_static_info_params) == 28, "NVIDIA's layout of the display's static information");

// NVIDIA's NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM, the control that gives the firmware where the display's
// instance memory lies, for the firmware to write into the display engine's registers
constexpr uint32_t CONTROL_WRITE_DISPLAY_INSTANCE_MEMORY = 0x20800A49;

// NVIDIA's ADDR_FBMEM, the address space of video memory, where an address is an offset from its start
constexpr uint32_t ADDRESS_SPACE_VIDEO_MEMORY = 2;

// NVIDIA's NV_MEMORY_WRITECOMBINED, the caching of memory the CPU maps write-combined
constexpr uint32_t CPU_CACHING_WRITE_COMBINED = 2;

// The parameters of `CONTROL_WRITE_DISPLAY_INSTANCE_MEMORY`
struct display_instance_memory_params {
    uint64_t address;       // NVIDIA's instMemPhysAddr, for video memory an offset from its start
    uint64_t bytes;         // NVIDIA's instMemSize
    uint32_t address_space; // NVIDIA's instMemAddrSpace
    uint32_t cpu_caching;   // NVIDIA's instMemCpuCacheAttr
};

static_assert(sizeof(display_instance_memory_params) == 24, "NVIDIA's layout of WRITE_INST_MEM's parameters");

// NVIDIA's classes of two of the display's objects under a device: display common (NV04_DISPLAY_COMMON, which the
// controls about the display's connectors go to) and the bandwidth check's object (NVC372_DISPLAY_SW)
constexpr uint32_t CLASS_DISPLAY_COMMON          = 0x73;
constexpr uint32_t CLASS_DISPLAY_BANDWIDTH_CHECK = 0xC372;

// NVIDIA's NV0073_CTRL_CMD_SYSTEM_GET_CAPS_V2, the control to display common that gives its capabilities, a table of
// bits naming what the display engine supports and the workarounds it needs
constexpr uint32_t CONTROL_GET_DISPLAY_COMMON_CAPABILITIES = 0x730138;

// NVIDIA's NV0073_CTRL_SYSTEM_CAPS_TBL_SIZE, the bytes of display common's table
constexpr uint32_t DISPLAY_COMMON_CAPABILITY_TABLE_BYTES = 2;

// The parameters of `CONTROL_GET_DISPLAY_COMMON_CAPABILITIES`, which the reply fills
struct display_common_capabilities_params {
    uint8_t table[DISPLAY_COMMON_CAPABILITY_TABLE_BYTES];
};

// NVIDIA's NV0073_CTRL_SYSTEM_CAPS_CROSS_BAR_SUPPORTED, bit 0x08 of the table's byte 1: the display engine's links (its
// units that each drive one connector) can each drive any connector through a crossbar
constexpr uint32_t DISPLAY_COMMON_CROSSBAR_BYTE = 1;
constexpr uint8_t  DISPLAY_COMMON_CROSSBAR_BIT  = 0x08;

// NVIDIA's NV5070_CTRL_CMD_SYSTEM_GET_CAPS_V2, the control to the display that gives its capabilities, a table of
// bits as display common's is
constexpr uint32_t CONTROL_GET_DISPLAY_CAPABILITIES = 0x50700709;

// NVIDIA's NV5070_CTRL_SYSTEM_CAPS_TBL_SIZE, the bytes of the display's table
constexpr uint32_t DISPLAY_CAPABILITY_TABLE_BYTES = 1;

// The parameters of `CONTROL_GET_DISPLAY_CAPABILITIES`, which the reply fills
struct display_capabilities_params {
    uint32_t subdevice_index; // NVIDIA's base.subdeviceIndex, the GPU of the device asked about, 0 for the first
    uint8_t  table[DISPLAY_CAPABILITY_TABLE_BYTES];
};

// NVIDIA's NV5070_CTRL_SYSTEM_CAPS_BUG_644815_DNISO_VIDMEM_ONLY, bit 0x40 of the table's byte 0: the display engine
// reaches push buffers, semaphores and notifiers (memory its queues of commands use) only in video memory
constexpr uint32_t DISPLAY_VIDEO_MEMORY_ONLY_BYTE = 0;
constexpr uint8_t  DISPLAY_VIDEO_MEMORY_ONLY_BIT  = 0x40;

static_assert(sizeof(display_common_capabilities_params) == 2, "NVIDIA's layout of display common's capabilities");
static_assert(sizeof(display_capabilities_params) == 8, "NVIDIA's layout of the display's capabilities");

// NVIDIA's NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, the control to display common that gives how many heads (the units
// that each generate one monitor's timing) the display engine can use
constexpr uint32_t CONTROL_GET_HEAD_COUNT = 0x730102;

// The parameters of `CONTROL_GET_HEAD_COUNT`, which the reply fills. Flags of 0 (NVIDIA's
// NV0073_CTRL_SYSTEM_GET_NUM_HEADS_FLAGS_CLIENT_DISABLE) count every usable head, not only those a client has in use
struct head_count_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, the GPU of the device asked about, 0 for the first
    uint32_t flags;
    uint32_t head_count;      // NVIDIA's numHeads
};

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK, the control to display common that gives the mask of the heads
// the display engine can use
constexpr uint32_t CONTROL_GET_HEAD_MASK = 0x730287;

// The parameters of `CONTROL_GET_HEAD_MASK`, which the reply fills
struct head_mask_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t head_mask;       // bit i set when head i can be used
};

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_GET_VALID_HEAD_WINDOW_ASSIGNMENT, the control to display common that gives, for
// each window (the units that each scan one image out onto a head), the heads it may serve
constexpr uint32_t CONTROL_GET_WINDOW_HEADS = 0x7302AD;

// NVIDIA's NV0073_CTRL_SPECIFIC_MAX_WINDOWS, the windows the reply of `CONTROL_GET_WINDOW_HEADS` gives a mask for
constexpr uint32_t WINDOW_HEAD_MASK_COUNT = 32;

// NVIDIA's NV0073_CTRL_SPECIFIC_FLEXIBLE_HEAD_WINDOW_ASSIGNMENT, the mask of window 0 in a reply whose windows may each
// serve any head
constexpr uint8_t FLEXIBLE_WINDOW_HEADS = 0xFF;

// The parameters of `CONTROL_GET_WINDOW_HEADS`, which the reply fills
struct window_heads_params {
    uint32_t subdevice_index;                     // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint8_t  window_heads[WINDOW_HEAD_MASK_COUNT]; // bit h of entry i set when window i may serve head h
};

static_assert(sizeof(head_count_params) == 12, "NVIDIA's layout of GET_NUM_HEADS's parameters");
static_assert(sizeof(head_mask_params) == 8, "NVIDIA's layout of GET_ALL_HEAD_MASK's parameters");
static_assert(sizeof(window_heads_params) == 36, "NVIDIA's layout of GET_VALID_HEAD_WINDOW_ASSIGNMENT's parameters");

// NVIDIA's NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, the control to display common that reports the supported display ids
// (single bits that each name one way the firmware can drive a monitor through one of the card's connectors)
constexpr uint32_t CONTROL_GET_SUPPORTED_DISPLAYS = 0x730120;

// The parameters of `CONTROL_GET_SUPPORTED_DISPLAYS`, which the reply fills
struct supported_displays_params {
    uint32_t subdevice_index;  // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_mask;     // bit i set when display id 1 << i is supported
    uint32_t ddc_display_mask; // NVIDIA's displayMaskDDC, the supported display ids with DDC (a connector's I2C bus)
};

// NVIDIA's NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, the control to display common that gives which display ids of a
// mask have a monitor connected
constexpr uint32_t CONTROL_GET_CONNECTED_DISPLAYS = 0x730122;

// The parameters of `CONTROL_GET_CONNECTED_DISPLAYS`. Flags of 0 (NVIDIA's METHOD_DEFAULT, DDC_DEFAULT, LOAD_DEFAULT
// and VBLANK_DEFAULT) let the firmware pick how it detects a monitor on each display id
struct connected_displays_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t flags;
    uint32_t display_mask;    // the display ids to check, which the reply narrows to those connected
    uint32_t retry_time_ms;   // NVIDIA's retryTimeMs, set with NVIDIA's status asking for the control again later
};

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_GET_TYPE, the control to display common that gives a display id's kind
constexpr uint32_t CONTROL_GET_DISPLAY_KIND = 0x730240;

// The parameters of `CONTROL_GET_DISPLAY_KIND`, whose reply fills `display_kind`
struct display_kind_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_id;      // a single bit
    uint32_t display_kind;    // NVIDIA's displayType: 0 unknown, 1 analog (CRT), 2 digital (DFP) or 3 television
};

// NVIDIA's NV0073_CTRL_SPECIFIC_DISPLAY_TYPE_DFP, the kind of a digital display id
constexpr uint32_t DISPLAY_KIND_DIGITAL = 2;

static_assert(sizeof(supported_displays_params) == 12, "NVIDIA's layout of GET_SUPPORTED's parameters");
static_assert(sizeof(connected_displays_params) == 16, "NVIDIA's layout of GET_CONNECT_STATE's parameters");
static_assert(sizeof(display_kind_params) == 12, "NVIDIA's layout of GET_TYPE's parameters");

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO, the control to display common that gives facts about a display id's
// output resource (the display engine's unit driving it) and whether the firmware allocates the display id dynamically
constexpr uint32_t CONTROL_GET_OUTPUT_RESOURCE = 0x73028B;

// The parameters of `CONTROL_GET_OUTPUT_RESOURCE`, whose reply fills every field after `display_id`
struct output_resource_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_id;      // a single bit
    uint32_t index;           // the output resource's number among those of its type, or `NO_OUTPUT_RESOURCE`
    uint32_t type;            // NVIDIA's output resource type, such as 2 for a serial one (NVIDIA's SOR)
    uint32_t protocol;
    uint32_t dither_type;
    uint32_t dither_algorithm;
    uint32_t location;
    uint32_t root_port_id;
    uint32_t dcb_index;
    uint64_t vbios_address;
    uint8_t  is_lit_by_vbios;
    uint8_t  is_dynamic;      // NVIDIA's bIsDispDynamic, nonzero for a display id the firmware allocates dynamically
};

// NVIDIA's NV_INVALID_OR, the `output_resource_params::index` of a display id no output resource drives
constexpr uint32_t NO_OUTPUT_RESOURCE = 0xFFFFFFFF;

// NVIDIA's NV0073_CTRL_SPECIFIC_OR_TYPE_SOR, the `output_resource_params::type` of a serial output resource
constexpr uint32_t OUTPUT_RESOURCE_TYPE_SERIAL = 2;

static_assert(sizeof(output_resource_params) == 56, "NVIDIA's layout of OR_GET_INFO's parameters");

// NVIDIA's NV0073_CTRL_CMD_DFP_GET_INFO, the control to display common that gives a digital display id's facts, which
// the firmware refuses for a display id of another kind
constexpr uint32_t CONTROL_GET_DIGITAL_DISPLAY_FACTS = 0x731140;

// The parameters of `CONTROL_GET_DIGITAL_DISPLAY_FACTS`, whose reply fills `flags` and `flags2`
struct digital_display_facts_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_id;      // a single bit
    uint32_t flags;           // NVIDIA's DFP flags, holding the fields below
    uint32_t flags2;
};

// The fields of `digital_display_facts_params::flags` the driver reads, NVIDIA's NV0073_CTRL_DFP_FLAGS_*. SIGNAL, bits
// 2 to 0, is 3 for DisplayPort, and 0 for TMDS, 1 LVDS, 2 SDI, 4 DSI or 5 writeback
constexpr uint32_t DIGITAL_SIGNAL_SHIFT       = 0;
constexpr uint32_t DIGITAL_SIGNAL_MASK        = 0x7;
constexpr uint32_t DIGITAL_SIGNAL_DISPLAYPORT = 3;

// LANE, bits 5 to 3, the lanes the board supports for a DisplayPort signal: 0 none, 1 one, 2 two, 3 four or 4 eight
constexpr uint32_t DIGITAL_LANES_SHIFT = 3;
constexpr uint32_t DIGITAL_LANES_MASK  = 0x7;

// LIMIT, bit 6, set when the display id may only show modes of a 60 Hz refresh rate
constexpr uint32_t DIGITAL_LIMITED_TO_60_HZ = 1u << 6;

// DP_LINK_CONSTRAINT, bit 16, set when the display id's DisplayPort link must be trained at RBR (the reduced bit rate,
// 1.62 Gbps) whenever possible
constexpr uint32_t DIGITAL_PREFERS_RBR = 1u << 16;

// DP_LINK_BW, bits 19 to 17, the highest link rate of a DisplayPort display id: 1 for 1.62 Gbps, 2 for 2.70, 3 for
// 5.40 or 4 for 8.10
constexpr uint32_t DIGITAL_LINK_RATE_SHIFT = 17;
constexpr uint32_t DIGITAL_LINK_RATE_MASK  = 0x7;

static_assert(sizeof(digital_display_facts_params) == 16, "NVIDIA's layout of DFP_GET_INFO's parameters");

// NVIDIA's NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, the control to display common that reads the EDID of the monitor on a
// connected display id
constexpr uint32_t CONTROL_GET_EDID = 0x730245;

// NVIDIA's NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES, the most EDID bytes a reply carries, as the firmware refuses a
// request for a longer EDID
constexpr uint32_t EDID_BUFFER_BYTES = 2048;

// The parameters of `CONTROL_GET_EDID`, whose reply fills `edid_bytes` and `edid`. Flags of 0 (COPY_CACHE_NO,
// READ_MODE_COOKED and DISPMUX_READ_MODE_DEFAULT) have the firmware read the EDID from the monitor, not its last copy
struct edid_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_id;      // a single bit
    uint32_t edid_bytes;      // NVIDIA's bufferSize, how many bytes of `edid` the reply fills
    uint32_t flags;
    uint8_t  edid[EDID_BUFFER_BYTES];
};

static_assert(sizeof(edid_params) == 2064, "NVIDIA's layout of GET_EDID_V2's parameters");

// NVIDIA's NV0073_CTRL_CMD_DP_AUXCH_CTRL, the control to display common that sends one transaction over a DisplayPort
// display id's AUX channel, the side channel to the monitor's DPCD registers
constexpr uint32_t CONTROL_DP_AUX = 0x731341;

// NVIDIA's NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE, the most bytes one AUX transaction carries
constexpr uint32_t AUX_DATA_MAX_BYTES = 16;

// The parameters of `CONTROL_DP_AUX`, whose reply fills `data`, `size` and `reply`
struct aux_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t display_id;      // a single bit
    uint8_t  is_address_only; // NVIDIA's bAddrOnly, for I2C transactions only
    uint32_t command;
    uint32_t address;         // a DPCD address, 20 bits
    uint8_t  data[AUX_DATA_MAX_BYTES];
    uint32_t size;            // sent as the bytes asked for less 1, and given back as the bytes the transaction moved
    uint32_t reply;           // NVIDIA's replyType, how the transaction ended
    uint32_t retry_time_ms;   // NVIDIA's retryTimeMs
};

// `aux_params::command` for a DPCD read, NVIDIA's AUXCH_CMD_TYPE_AUX (bit 3) with AUXCH_CMD_REQ_TYPE_READ (bits 1 to 0)
constexpr uint32_t AUX_COMMAND_DPCD_READ = 0x9;

// `aux_params::reply`'s values, NVIDIA's NV0073_CTRL_DP_AUXCH_REPLYTYPE_*: the monitor acknowledged the transaction,
// refused it, asked for it again later, did not answer in time, refused an I2C transaction or asked for one again later
constexpr uint32_t AUX_REPLY_ACK       = 0;
constexpr uint32_t AUX_REPLY_NACK      = 1;
constexpr uint32_t AUX_REPLY_DEFER     = 2;
constexpr uint32_t AUX_REPLY_TIMEOUT   = 3;
constexpr uint32_t AUX_REPLY_I2C_NACK  = 4;
constexpr uint32_t AUX_REPLY_I2C_DEFER = 8;

static_assert(sizeof(aux_params) == 48, "NVIDIA's layout of DP_AUXCH_CTRL's parameters");

// NVIDIA's NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT, the control to display common that leaves the driver the
// DisplayPort hardware's watermark programming, DP IRQ (CP IRQ) handling and retries of a deferred AUX reply
constexpr uint32_t CONTROL_SET_MANUAL_DISPLAYPORT = 0x731365;

// The parameters of `CONTROL_SET_MANUAL_DISPLAYPORT`
struct manual_displayport_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
};

static_assert(sizeof(manual_displayport_params) == 4, "NVIDIA's layout of DP_SET_MANUAL_DISPLAYPORT's parameters");

// NVIDIA's NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, the control that gives the firmware where a display
// channel's push buffer lies, sent before the channel's allocation
constexpr uint32_t CONTROL_SET_DISPLAY_PUSH_BUFFER = 0x20800A58;

// NVIDIA's ADDR_SYSMEM, the address space of system memory, where an address is a physical address on the system bus
constexpr uint32_t ADDRESS_SPACE_SYSTEM_MEMORY = 1;

// The parameters of `CONTROL_SET_DISPLAY_PUSH_BUFFER`
struct display_push_buffer_params {
    uint32_t address_space;
    uint64_t address;          // NVIDIA's physicalAddr
    uint64_t limit;            // the offset of the push buffer's last byte, its size less 1
    uint32_t snoops;           // NVIDIA's cacheSnoop, 1 when the display engine's reads snoop the CPU's caches
    uint32_t channel_class;    // NVIDIA's hclass
    uint32_t channel_instance; // 0 for the core channel
    uint8_t  is_valid;         // NVIDIA's valid, 1 for a channel with a push buffer
};

// NVIDIA's NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS, the parameters of a display channel's allocation under the
// display. The context DMA handles name objects of NVIDIA's kernel-side driver, which the firmware never holds
struct display_channel_params {
    uint32_t channel_instance;
    handle   push_buffer_context_dma; // NVIDIA's hObjectBuffer
    handle   notifier_context_dma;    // NVIDIA's hObjectNotify
    uint32_t offset;                  // where PUT and GET start in the push buffer
    uint64_t control;                 // NVIDIA's pControl, never read, a virtual address of PUT and GET from the reply
    uint32_t flags;
};

// NVIDIA's NV5070_CTRL_CMD_SET_RMFREE_FLAGS, the control to the display setting flags for the next release only, and
// NVIDIA's NV5070_CTRL_SET_RMFREE_FLAGS_PRESERVE_HW, the flag keeping the hardware as it is through that release
constexpr uint32_t CONTROL_SET_DISPLAY_RELEASE_FLAGS = 0x50700117;
constexpr uint32_t RELEASE_PRESERVES_HARDWARE        = 0x1;

// The parameters of `CONTROL_SET_DISPLAY_RELEASE_FLAGS`
struct display_release_flags_params {
    uint32_t subdevice_index; // as in `display_capabilities_params`
    uint32_t flags;
};

// The core channel's methods for head 0, NVIDIA's NVC67D_HEAD_SET_*(0), and the stride between two heads' methods. A
// scaler coefficient holds DATA in bits 9:0 and INDEX in bits 19:12, and ALPHA_RED holds ALPHA in bits 7:0
constexpr uint32_t CORE_HEAD_METHODS_STRIDE           = 0x400;
constexpr uint32_t CORE_HEAD_OUTPUT_SCALER_COEFFICIENT = 0x2214;
constexpr uint32_t CORE_HEAD_DESKTOP_COLOR_ALPHA_RED   = 0x2220;
constexpr uint32_t CORE_HEAD_DESKTOP_COLOR_GREEN_BLUE  = 0x2224;
constexpr uint32_t SCALER_COEFFICIENT_DATA_MASK        = 0x3FF;
constexpr uint32_t SCALER_COEFFICIENT_INDEX_SHIFT      = 12;

// The core channel's method naming window 0's owner (the head it scans out onto), NVIDIA's
// NVC67D_WINDOW_SET_CONTROL(0), and the stride between two windows' methods. OWNER, bits 3:0, names the head or none
constexpr uint32_t CORE_WINDOW_CONTROL        = 0x1000;
constexpr uint32_t CORE_WINDOW_METHODS_STRIDE = 0x80;
constexpr uint32_t WINDOW_CONTROL_OWNER_MASK  = 0xF;

static_assert(sizeof(display_push_buffer_params) == 40, "NVIDIA's layout of DISPLAY_CHANNEL_PUSHBUFFER's parameters");
static_assert(sizeof(display_channel_params) == 32, "NVIDIA's layout of a display channel's allocation parameters");
static_assert(sizeof(display_release_flags_params) == 8, "NVIDIA's layout of SET_RMFREE_FLAGS's parameters");

// NVIDIA's NVC370_CTRL_CMD_GET_CHANNEL_INFO, the control to the display that gives the state of one of its channels
constexpr uint32_t CONTROL_GET_DISPLAY_CHANNEL_STATE = 0xC3700104;

// The parameters of `CONTROL_GET_DISPLAY_CHANNEL_STATE`, whose reply fills `state`. `is_in_debug_mode` is not read
struct display_channel_state_params {
    uint32_t subdevice_index;  // as in `display_capabilities_params`
    uint32_t channel_class;    // the class the channel was allocated with
    uint32_t channel_instance; // 0 for the core channel
    uint8_t  is_in_debug_mode; // NVIDIA's IsChannelInDebugMode
    uint32_t state;            // NVIDIA's channelState, the channel's state as a mask with a bit for each state
};

// NVIDIA's NVC370_CTRL_GET_CHANNEL_INFO_STATE_IDLE, the state of a channel with no method pending, every method it
// took having passed through the hardware's internal queue
constexpr uint32_t CHANNEL_STATE_IDLE = 0x1;

static_assert(sizeof(display_channel_state_params) == 20, "NVIDIA's layout of GET_CHANNEL_INFO's parameters");

// NVIDIA's NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE, the control to display common that gives the display id a head shows
constexpr uint32_t CONTROL_GET_ACTIVE_DISPLAY = 0x730126;

// The parameters of `CONTROL_GET_ACTIVE_DISPLAY`, whose reply fills `display_id`. Flags of 0 (NVIDIA's
// NV0073_CTRL_SYSTEM_GET_ACTIVE_FLAGS_CLIENT_DISABLE) find a display the boot firmware lit, not only a client's
struct active_display_params {
    uint32_t subdevice_index; // NVIDIA's subDeviceInstance, as in `head_count_params`
    uint32_t head;
    uint32_t flags;
    uint32_t display_id;      // the display id the head shows, 0 for none
};

static_assert(sizeof(active_display_params) == 16, "NVIDIA's layout of GET_ACTIVE's parameters");

// NVIDIA's NVC372_CTRL_CMD_IS_MODE_POSSIBLE, the control to the bandwidth check's object that asks whether the display
// engine has the memory bandwidth and clocks to scan out at once the heads and windows its parameters name
constexpr uint32_t CONTROL_IS_MODE_POSSIBLE = 0xC3720101;

// NVIDIA's NVC372_CTRL_MAX_POSSIBLE_HEADS and NVC372_CTRL_MAX_POSSIBLE_WINDOWS, the most heads and windows in a request
constexpr uint32_t MODE_POSSIBLE_HEADS_MAX   = 8;
constexpr uint32_t MODE_POSSIBLE_WINDOWS_MAX = 32;

// NVIDIA's NV_DISP_LOCK_MODE_NO_LOCK and NV_DISP_LOCK_PIN_UNSPECIFIED, a head whose raster is locked to no other's
constexpr uint32_t LOCK_MODE_NONE = 0x0;
constexpr uint32_t LOCK_PIN_NONE  = 0x10;

// NVIDIA's NVC372_CTRL_IMP_LUT_USAGE_1025, a look-up table of 1025 entries
constexpr uint8_t LUT_USAGE_1025 = 2;

// NVIDIA's NVC372_CTRL_FORMAT_RGB_PACKED_4_BPP, the formats of 4 bytes a pixel
constexpr uint32_t FORMAT_RGB_PACKED_4_BPP = 0x4;

// A head a request names, NVIDIA's NVC372_CTRL_IMP_HEAD. Raster positions count from the start of the sync, and a
// downscale factor counts 1024ths
struct mode_possible_head {
    uint8_t  head;                         // NVIDIA's headIndex
    uint32_t pixel_clock_khz;              // NVIDIA's maxPixelClkKHz
    uint32_t raster_width;                 // NVIDIA's rasterSize
    uint32_t raster_height;
    uint32_t blank_start_x;                // NVIDIA's rasterBlankStart
    uint32_t blank_start_y;
    uint32_t blank_end_x;                  // NVIDIA's rasterBlankEnd
    uint32_t blank_end_y;
    uint32_t second_blank_start_y;         // NVIDIA's rasterVertBlank2, 0 for a progressive raster
    uint32_t second_blank_end_y;
    uint32_t master_lock_mode;             // NVIDIA's control
    uint32_t master_lock_pin;
    uint32_t slave_lock_mode;
    uint32_t slave_lock_pin;
    uint32_t max_downscale_x;              // NVIDIA's maxDownscaleFactorH
    uint32_t max_downscale_y;              // NVIDIA's maxDownscaleFactorV
    uint8_t  output_scaler_taps;           // NVIDIA's outputScalerVerticalTaps
    uint8_t  allows_upscaling_y;           // NVIDIA's bUpscalingAllowedV
    uint8_t  enables_overfetch;            // NVIDIA's bOverfetchEnabled
    uint16_t leading_idle_lines;           // NVIDIA's minFrameIdle
    uint16_t trailing_idle_lines;
    uint8_t  lut;                          // the output look-up table's usage
    uint8_t  cursor_width_32;              // NVIDIA's cursorSize32p, the cursor's width in units of 32 pixels
    uint8_t  enables_dsc;                  // NVIDIA's bEnableDsc
    uint8_t  is_yuv420;                    // NVIDIA's bYUV420Format
    uint8_t  is_two_heads_one_or;          // NVIDIA's bIs2Head1Or
    uint8_t  gets_osld_output;             // NVIDIA's bGetOSLDOutput
    uint8_t  disables_mid_frame_watermark; // NVIDIA's bDisableMidFrameAndDWCFWatermark
};

// A window a request names, NVIDIA's NVC372_CTRL_IMP_WINDOW
struct mode_possible_window {
    uint32_t window;             // NVIDIA's windowIndex
    uint32_t head;               // NVIDIA's owningHead
    uint32_t formats;            // NVIDIA's formatUsageBound, the FORMAT_* bits of the formats the window scans out
    uint32_t rotated_formats;    // NVIDIA's rotatedFormatUsageBound
    uint32_t max_pixels_fetched; // NVIDIA's maxPixelsFetchedPerLine
    uint32_t max_downscale_x;    // NVIDIA's maxDownscaleFactorH
    uint32_t max_downscale_y;    // NVIDIA's maxDownscaleFactorV
    uint8_t  input_scaler_taps;  // NVIDIA's inputScalerVerticalTaps
    uint8_t  allows_upscaling_y; // NVIDIA's bUpscalingAllowedV
    uint8_t  enables_overfetch;  // NVIDIA's bOverfetchEnabled
    uint8_t  lut;                // the input look-up table's usage
    uint8_t  tmo_lut;            // the tone-mapping output look-up table's usage
};

// The parameters of `CONTROL_IS_MODE_POSSIBLE`, whose reply fills every field from `is_possible` on but the last.
// Options of 0 ask for no margin and no lowest performance state, and the fields from `is_osld_possible` are not read
struct mode_possible_params {
    uint32_t             subdevice_index;       // NVIDIA's base.subdeviceIndex, as in `display_capabilities_params`
    uint8_t              head_count;            // NVIDIA's numHeads
    uint8_t              window_count;          // NVIDIA's numWindows
    mode_possible_head   heads[MODE_POSSIBLE_HEADS_MAX];
    mode_possible_window windows[MODE_POSSIBLE_WINDOWS_MAX];
    uint32_t             options;
    uint32_t             test_memory_clock_khz; // NVIDIA's testMclkFreqKHz
    uint8_t              is_possible;           // NVIDIA's bIsPossible
    uint8_t              is_osld_possible[MODE_POSSIBLE_HEADS_MAX];
    uint32_t             min_imp_vpstate;
    uint32_t             min_pstate;
    uint32_t             min_bandwidth_kbps;
    uint32_t             floor_bandwidth_kbps;
    uint32_t             min_hub_clock_khz;
    uint32_t             osld_vblank_increase_lines[MODE_POSSIBLE_HEADS_MAX];
    uint32_t             osld_wake_up_lines[MODE_POSSIBLE_HEADS_MAX];
    uint32_t             worst_case_margin;
    uint32_t             display_clock_khz;
    char                 worst_case_domain[8];
    uint8_t              uses_cached_perf_state; // NVIDIA's bUseCachedPerfState
};

static_assert(sizeof(mode_possible_head) == 80, "NVIDIA's layout of a head of IS_MODE_POSSIBLE");
static_assert(sizeof(mode_possible_window) == 36, "NVIDIA's layout of a window of IS_MODE_POSSIBLE");
static_assert(sizeof(mode_possible_params) == 1924, "NVIDIA's layout of IS_MODE_POSSIBLE's parameters");

} // namespace drivers::nvidia::rm

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
