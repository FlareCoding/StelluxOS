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

static_assert(sizeof(display_push_buffer_params) == 40, "NVIDIA's layout of DISPLAY_CHANNEL_PUSHBUFFER's parameters");
static_assert(sizeof(display_channel_params) == 32, "NVIDIA's layout of a display channel's allocation parameters");
static_assert(sizeof(display_release_flags_params) == 8, "NVIDIA's layout of SET_RMFREE_FLAGS's parameters");

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

} // namespace drivers::nvidia::rm

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
