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

} // namespace drivers::nvidia::rm

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_RM_H
