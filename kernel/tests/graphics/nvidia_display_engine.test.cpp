// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_registration.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/rm.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "drivers/graphics/nvidia/window_methods.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_display_engine);

using drivers::nvidia::active_heads;
using drivers::nvidia::base_buffer_pitch;
using drivers::nvidia::build_window_usage_methods;
using drivers::nvidia::CHANNEL_GET;
using drivers::nvidia::CHANNEL_PUT;
using drivers::nvidia::chip_displayport;
using drivers::nvidia::chip_info;
using drivers::nvidia::COMPLETION_NOTIFIER_WAIT_NS;
using drivers::nvidia::CORE_CHANNEL_AREA;
using drivers::nvidia::CORE_INITIAL_METHODS_PER_HEAD;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::display_channel;
using drivers::nvidia::display_engine;
using drivers::nvidia::display_heads;
using drivers::nvidia::DPCD_PHY_REPEATER_CAPABILITIES;
using drivers::nvidia::DPCD_PHY_REPEATER_CAPABILITY_BYTES;
using drivers::nvidia::DPCD_RECEIVER_CAPABILITY_BYTES;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_NOT_REGISTERED;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::first_second_statistics;
using drivers::nvidia::HEAD_LIGHT_FIXED_METHODS;
using drivers::nvidia::INSTANCE_MEMORY_BYTES;
using drivers::nvidia::LOOK_UP_TABLE_SURFACE_BYTES;
using drivers::nvidia::look_up_table_word;
using drivers::nvidia::mode_times;
using drivers::nvidia::NO_HEAD;
using drivers::nvidia::NO_OUTPUT;
using drivers::nvidia::NOTIFIER_SLOTS;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::OK;
using drivers::nvidia::PUSH_BUFFER_BYTES;
using drivers::nvidia::register_display_engine;
using drivers::nvidia::registers;
using drivers::nvidia::release_display_channel;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
using drivers::nvidia::window_channel_area;
using drivers::nvidia::WINDOW_IMAGE_METHODS;
using drivers::nvidia::WINDOW_OWNER_NONE;
using drivers::nvidia::window_usage_methods;
using drivers::nvidia::WINDOW_USAGE_METHODS;
namespace displayport = drivers::nvidia::displayport;
namespace rm = drivers::nvidia::rm;

// The driver's client and device and the first handle the double gives, made up
static constexpr rm::handle TEST_CLIENT  = 0xC1D0000B;
static constexpr rm::handle TEST_DEVICE  = 0x0005000A;
static constexpr rm::handle FIRST_HANDLE = 0x0005000D;

// A display class no chip has and GA102's channel classes swapped, so each allocation's class names its object
static constexpr uint32_t TEST_DISPLAY_CLASS        = 0xC67F;
static constexpr uint32_t TEST_CORE_CHANNEL_CLASS   = 0xC67E;
static constexpr uint32_t TEST_WINDOW_CHANNEL_CLASS = 0xC67D;

// A card's full address width, and a limit `dma::alloc_pages` refuses, so a set-up for a card limited to it fails at
// the push buffer
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The heads, windows and serial links of the chip `set_up` describes, whose initial methods outgrow the push buffer
static constexpr uint8_t CHIP_HEADS        = 4;
static constexpr uint8_t CHIP_WINDOWS      = 8;
static constexpr uint8_t CHIP_SERIAL_LINKS = 4;

// Whether the heads of the chip `set_up` describes report output scalers, as GA102's do, or none
static constexpr bool HEADS_WITH_OUTPUT_SCALERS    = true;
static constexpr bool HEADS_WITHOUT_OUTPUT_SCALERS = false;

// The links of the chip `set_up` describes by default, which run up to HBR3 with FEC, and the same links without FEC
static constexpr chip_displayport CHIP_LINKS = {
    .trains_phy_repeaters       = false,
    .max_rate                   = displayport::link_rate::hbr3,
    .fec_supported              = true,
    .increased_watermark_limits = false,
};

static constexpr chip_displayport CHIP_LINKS_WITHOUT_FEC = {
    .trains_phy_repeaters       = false,
    .max_rate                   = displayport::link_rate::hbr3,
    .fec_supported              = false,
    .increased_watermark_limits = false,
};

// The heads the double reports by default, where their initial methods (each a header and one value) end, so a GET
// reading it has reached PUT, and a GET past the push buffer's writable end
static constexpr uint8_t  DOUBLE_HEADS        = 2;
static constexpr uint32_t INITIAL_METHODS_END = CORE_INITIAL_METHODS_PER_HEAD * DOUBLE_HEADS * 2 * sizeof(uint32_t);
static constexpr uint32_t BAD_GET             = 0xFFFFFFFF;

// NVIDIA's instance memory layout, restated: 8-byte hash table entries whose second word holds the client's low 14
// bits, from bit 14 the record's offset in 32-byte units, 0x101 at 0x2020, and from bit 25 the channel's number
static constexpr uint32_t NVIDIA_HASH_ENTRIES        = 1024;
static constexpr uint64_t NVIDIA_ENTRY_BYTES         = 8;
static constexpr uint32_t NVIDIA_CLIENT_MASK         = 0x3FFF;
static constexpr uint32_t NVIDIA_RECORD_FIELD_SHIFT  = 14;
static constexpr uint32_t NVIDIA_CHANNEL_SHIFT       = 25;
static constexpr uint32_t NVIDIA_FIRST_RECORD_FIELD  = 0x101;
static constexpr uint64_t NVIDIA_FIRST_RECORD_OFFSET = 0x2020;
static constexpr uint64_t NVIDIA_RECORD_BYTES        = 32;

// A record's first word for snooped pages and for video memory, read and write and pitch, the bytes of a record's
// address unit and NVIDIA's 4 KiB notifier in those units
static constexpr uint32_t NVIDIA_SNOOPED_PAGES_WORD_0 = 7;
static constexpr uint32_t NVIDIA_VIDEO_MEMORY_WORD_0  = 5;
static constexpr uint64_t NVIDIA_RECORD_UNIT_BYTES    = 256;
static constexpr uint64_t NVIDIA_NOTIFIER_UNITS       = 16;

// A display channel's busy state in NVIDIA's GET_CHANNEL_INFO reply, restated
static constexpr uint32_t NVIDIA_CHANNEL_STATE_BUSY = 0x40;

// How long after the first channel state request the double refuses every state request with `ERR_TIMEOUT`, so a wait
// still running a second after its first request fails instead of hanging the suite
static constexpr uint64_t LATE_STATE_REQUEST_NS = 1000 * NS_PER_MS;

// Where window 0's control lies in the core channel's area, at 0x1000 in NVIDIA's armed state from 0x8000, and the
// stride between two windows' controls, restated
static constexpr uint32_t NVIDIA_ARMED_WINDOW_CONTROL = 0x9000;
static constexpr uint32_t NVIDIA_WINDOW_STRIDE        = 0x80;

// The look-up table surface's whole pages, which the video memory double hands out after the instance memory
static constexpr uint64_t LOOK_UP_TABLE_OFFSET     = INSTANCE_MEMORY_BYTES;
static constexpr uint64_t LOOK_UP_TABLE_PAGE_BYTES = 0x5000;

// The bytes past each general mapping that the video memory double backs, so a write running past the range shows
static constexpr uint64_t GENERAL_GUARD_BYTES = 0x1000;

// The general memory the video memory double hands out, and the copy a CPU mapping reaches until its unmap writes the
// range back, so a write after the unmap is lost as through BAR1
static uint8_t g_video_memory[INSTANCE_MEMORY_BYTES + LOOK_UP_TABLE_PAGE_BYTES];
static uint8_t g_mapped_video_memory[sizeof(g_video_memory) + GENERAL_GUARD_BYTES];

// The first display register the engine reaches, head 0's timing event (NVIDIA's NV_PDISP_FE_EVT_STAT_HEAD_TIMING)
static constexpr uint32_t DISPLAY_REGISTERS_START = 0x611800;

// The words standing in for BAR0 from the heads' timing registers to the end of the last window channel's area
static uint32_t g_display_registers[(window_channel_area(CHIP_WINDOWS) - DISPLAY_REGISTERS_START) / sizeof(uint32_t)];

// The words from the core channel's area to the end of the last window channel's, within `g_display_registers`
static uint32_t* const g_channel_areas =
    g_display_registers + (CORE_CHANNEL_AREA - DISPLAY_REGISTERS_START) / sizeof(uint32_t);

// Where the video memory double hands out scanout memory, the bytes past it the double backs too, so a write running
// past the range shows, and the byte the double fills them with, as it fills each general range and guard
static constexpr uint64_t SCANOUT_OFFSET      = 0x1000000;
static constexpr uint64_t SCANOUT_GUARD_BYTES = 0x1000;
static constexpr uint8_t  SCANOUT_FILL        = 0xA5;

// `SCANOUT_FILL` in each byte of a 64-bit word, as the video memory double finds it in scanout words left unwritten
static constexpr uint64_t SCANOUT_FILL_WORD = 0xA5A5A5A5A5A5A5A5;

// The physical addresses in BAR1 the video memory double gives its scanout mappings, the first at
// `SCANOUT_BAR1_ADDRESS` and each later one `SCANOUT_BAR1_STRIDE` past the one before
static constexpr uint64_t SCANOUT_BAR1_ADDRESS = 0x6001000000;
static constexpr uint64_t SCANOUT_BAR1_STRIDE  = 0x1000000;

// Where the double maps the base buffer the second time, for userland, after the mapping that clears it
static constexpr uint64_t USERLAND_BAR1_ADDRESS = SCANOUT_BAR1_ADDRESS + SCANOUT_BAR1_STRIDE;

// The pages the video memory double hands general memory out in, as the allocator rounds each request to them
static constexpr uint64_t DOUBLE_PAGE_BYTES = 0x1000;

// The core channel's area, where `g_channel_areas` begins, holding its PUT, GET and armed state
static uint32_t* const g_core_channel_registers = g_channel_areas;

// The object under test, which its instance memory's copy of the hash table makes too large for a test's stack, and
// the engine last built there, null before the first
alignas(display_engine) static uint8_t g_engine_bytes[sizeof(display_engine)];
static display_engine* g_built_engine = nullptr;

// Two display ids, which are outputs 0 and 1 when both are supported
static constexpr uint32_t OUTPUT_0_ID = 0x100;
static constexpr uint32_t OUTPUT_1_ID = 0x200;

// The digital facts a supported display id gives by default: a DisplayPort signal, lane code 3 for four lanes and link
// rate code 4 for HBR3
static constexpr uint32_t DISPLAYPORT_FACTS = (rm::DIGITAL_SIGNAL_DISPLAYPORT << rm::DIGITAL_SIGNAL_SHIFT) |
                                              (3u << rm::DIGITAL_LANES_SHIFT) | (4u << rm::DIGITAL_LINK_RATE_SHIFT);

// The receiver capability field a connected monitor gives by default: DPCD revision 1.4, HBR3 and four lanes with
// enhanced framing, and no extended copy present
static constexpr uint8_t DPCD_RECEIVER[DPCD_RECEIVER_CAPABILITY_BYTES] = {0x14, 0x1E, 0xC4};

// The field of a monitor taking one lane at RBR, too few for either of the AOC's timings, and the same field at
// revision 0x0F, just below the 1.0 the driver requires, so that a monitor giving it fails the revision and the link
static constexpr uint8_t DPCD_RECEIVER_ONE_RBR_LANE[DPCD_RECEIVER_CAPABILITY_BYTES] = {0x14, 0x06, 0x81};
static constexpr uint8_t DPCD_RECEIVER_BELOW_1_0[DPCD_RECEIVER_CAPABILITY_BYTES]    = {0x0F, 0x06, 0x81};

// The AOC AG241QG4's base block with its serial number and extension count zeroed and its checksum recomputed, giving
// two timings of 2560 by 1440, at 59.950 Hz, preferred, and at 143.911 Hz
static constexpr uint8_t AOC_BASE_BLOCK[edid::BLOCK_BYTES] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x05, 0xE3, 0x10, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x26, 0x1C, 0x01, 0x04, 0xA5, 0x35, 0x28, 0x78, 0x3B, 0xE1, 0x61, 0xA4, 0x57, 0x53, 0xA0, 0x27,
    0x0D, 0x50, 0x54, 0xBF, 0xEF, 0x00, 0xD1, 0xC0, 0x81, 0x80, 0x31, 0x68, 0x31, 0x7C, 0x45, 0x68,
    0x45, 0x7C, 0x61, 0x68, 0x61, 0x7C, 0x56, 0x5E, 0x00, 0xA0, 0xA0, 0xA0, 0x29, 0x50, 0x30, 0x20,
    0x35, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E, 0x40, 0xE7, 0x00, 0x6A, 0xA0, 0xA0, 0x67, 0x50,
    0x08, 0x20, 0x98, 0x04, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x1E,
    0x92, 0xE6, 0xE6, 0x3C, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,
    0x00, 0x41, 0x47, 0x32, 0x34, 0x31, 0x51, 0x47, 0x34, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x00, 0xCD,
};

// The base block's features byte, whose bit 1 marks the first timing preferred, and its checksum byte
static constexpr uint32_t FEATURES_BYTE   = 24;
static constexpr uint8_t  FIRST_PREFERRED = 0x02;
static constexpr uint32_t CHECKSUM_BYTE   = 127;

// The base block's byte counting the extension blocks after it
static constexpr uint32_t EXTENSION_COUNT_BYTE = 126;

// A byte of the header every EDID starts with, the bytes of each descriptor, where the AOC's two timing descriptors
// start and their bytes, and where its third descriptor, its range limits, starts
static constexpr uint32_t HEADER_FF_BYTE           = 1;
static constexpr uint32_t DESCRIPTOR_BYTES         = 18;
static constexpr uint32_t TIMING_DESCRIPTORS_BYTE  = 54;
static constexpr uint32_t TIMING_DESCRIPTORS_BYTES = 36;
static constexpr uint32_t RANGE_DESCRIPTOR_BYTE    = 90;

// An 18-byte timing descriptor of 1920 by 1080 at 60.000 Hz, a 148.5 MHz pixel clock over 2200 by 1125 pixels, with the
// AOC's image size and sync flags
static constexpr uint8_t TIMING_1080_AT_60_HZ[] = {
    0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E,
};

// The EDID of every connected monitor, a copy of `AOC_BASE_BLOCK` a test may change
static uint8_t g_edid[edid::BLOCK_BYTES];

// The link status's copy among a monitor's event status indicators, NVIDIA's NV_DPCD_LANE0_1_STATUS_ESI, and its 3
// bytes once 1 lane is trained: its clock recovered, its equalization done, its symbols locked, lanes aligned
static constexpr uint32_t DPCD_LINK_STATUS_INDICATORS = 0x0200C;
static constexpr uint8_t  ONE_LANE_TRAINED[]          = {0x07, 0x00, 0x01};

// A firmware that records each request in order, its repeater status reads and the last training's command and data,
// answers one a refusal matches with its result, the replies its fields describe from them, and others as sent
class engine_firmware : public firmware {
public:
    static constexpr uint32_t MAX_CALLS = 64;

    enum class kind { allocation, control, release, internal_control };

    struct call {
        kind       made;
        rm::handle object; // the allocated, controlled or released object, 0 for an internal control
        uint32_t   number; // the class of an allocation or the command of a control, else 0
    };

    // Matches the calls of its kind whose number and object equal its own, each 0 matching any
    struct refusal {
        kind       made;
        uint32_t   number;
        rm::handle object;
        int32_t    result;
    };

    call       calls[MAX_CALLS] = {};
    uint32_t   call_count = 0;
    uint32_t   handle_count = 0;
    refusal    refusals[2] = {};
    uint8_t    heads = DOUBLE_HEADS;
    uint8_t    windows_per_head = 1;          // the windows each head has, head 0's first, then head 1's and so on
    uint32_t   present_windows = (1u << CHIP_WINDOWS) - 1; // the windows reported present
    rm::handle reported_client = TEST_CLIENT; // the client `client()` reports, 0 making `bind` refuse
    uint64_t   push_buffer_address = 0;       // the address `CONTROL_SET_DISPLAY_PUSH_BUFFER` last carried
    uint32_t   push_buffer_class = 0;         // and the channel class and instance it carried with it
    uint32_t   push_buffer_instance = 0;
    uint32_t   refused_push_buffer_class = 0; // a class whose push buffer is refused with `ERR_FIRMWARE`, 0 for none
    uint8_t    boot_head = 0;                 // the head showing `boot_display_id`
    uint32_t   boot_display_id = 0;
    uint32_t   other_display_id = 0;          // the display id every other head shows, 0 for none
    uint32_t   boot_link_index = 0;           // the link index every link request's reply names
    uint32_t   boot_link_type = rm::OUTPUT_RESOURCE_TYPE_SERIAL; // the link type every link request's reply names
    uint32_t   boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_A; // the protocol every link request's reply names
    uint8_t    assigned_resource = 0;         // the output resource an assignment's reply names for the display id
    bool       has_crossbar = false;          // whether display common's capabilities report a crossbar
    uint32_t   supported_ids = 0;             // the display ids reported supported, each a DisplayPort output
    uint32_t   digital_facts = DISPLAYPORT_FACTS; // the digital facts of every supported display id
    uint32_t   connected_ids = 0;             // those reported connected, the only ones with a monitor, giving `g_edid`
    const uint8_t* dpcd_receiver = DPCD_RECEIVER; // the receiver capability field of every connected monitor
    const uint8_t* link_status = ONE_LANE_TRAINED; // the 3 link status bytes every connected monitor gives, if any
    uint32_t   link_status_address = DPCD_LINK_STATUS_INDICATORS; // where it gives them
    bool       refuses_link_status = false;   // whether reads of them are refused with `ERR_FIRMWARE`
    const uint8_t* phy_repeater_capabilities = nullptr; // the PHY repeater bytes every connected monitor gives, if any
    uint32_t   repeater_status_reads = 0;     // the DPCD reads past those bytes, where the repeaters' status lies
    uint32_t   displayport_max_rate = 4;      // the highest rate code the DisplayPort facts give, the RTX 3080's HBR3
    uint32_t   displayport_versions = 0x3;    // the DisplayPort versions they give, the RTX 3080's 1.2 and 1.4
    uint8_t    displayport_trains_phy_repeaters = 1; // whether they give the firmware as training PHY repeaters
    uint32_t   training_command = 0;          // the command of the last training request
    uint32_t   training_data = 0;             // and its data
    uint32_t   refused_modes = 0;             // how many of the first bandwidth check requests the check refuses
    uint32_t   put_at_first_active_read = 0;  // the core channel's PUT when a head's display id was first asked
    bool       is_channel_busy = false;       // whether each channel state reply names the busy state, else idle
    bool       has_state_request = false;     // whether a channel state request has come
    uint64_t   first_state_request_ns = 0;    // when the first channel state request came
    uint32_t   armed_control_at_reply = 0;    // window 0's armed control after each state request, 0 leaving it alone
    uint64_t   core_push_buffer_address = 0;  // the address the core channel's push buffer request carried
    uint32_t   mode_set_ids[2] = {};          // the display ids the mode set's first two requests named
    uint32_t   mode_set_phases[2] = {};       // and their phases, start or end
    uint32_t   mode_set_puts[2] = {};         // and the core channel's PUT when each came
    uint64_t   mode_set_ns[2] = {};           // and the time each came
    uint32_t   mode_set_requests = 0;
    int32_t    mode_set_end_result = OK;      // the result of the mode set's end
    uint32_t   get_at_mode_set_start = 0;     // the core channel's GET from the mode set's start, 0 for the default
    uint32_t   get_at_link = 0;               // the core channel's GET from the mode set's first link request, if set
    uint32_t   put_at_link = 0;               // the core channel's PUT when that request came
    uint32_t   get_at_stream = 0;             // the core channel's GET from the stream's attributes, 0 for the default

    int32_t allocate(rm::handle, rm::handle object, uint32_t class_id, void*, size_t) override {
        return answer(kind::allocation, object, class_id);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t) override {
        bool is_first_active_read = command == rm::CONTROL_GET_ACTIVE_DISPLAY && control_count(command) == 0;
        if (is_first_active_read) {
            put_at_first_active_read = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
        }

        bool is_link_request = command == rm::CONTROL_ASSIGN_OUTPUT_RESOURCE;
        is_link_request = is_link_request || command == rm::CONTROL_GET_OUTPUT_RESOURCE;
        if (is_link_request && mode_set_requests == 1 && put_at_link == 0) {
            put_at_link = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
            if (get_at_link != 0) {
                g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)] = get_at_link;
            }
        }

        if (command == rm::CONTROL_SET_STREAM_ATTRIBUTES) {
            move_get_past_light();
        }

        bool is_state_request = command == rm::CONTROL_GET_DISPLAY_CHANNEL_STATE;
        if (is_state_request && !has_state_request) {
            has_state_request = true;
            first_state_request_ns = clock::now_ns();
        }

        uint64_t since_first_state_request_ns = clock::now_ns() - first_state_request_ns;
        bool is_late_state_request = is_state_request && since_first_state_request_ns > LATE_STATE_REQUEST_NS;
        int32_t rc = answer(kind::control, object, command);
        if (is_late_state_request) {
            return ERR_TIMEOUT;
        }

        bool arms_window_0 = is_state_request && armed_control_at_reply != 0;
        if (arms_window_0) {
            g_core_channel_registers[NVIDIA_ARMED_WINDOW_CONTROL / sizeof(uint32_t)] = armed_control_at_reply;
        }

        if (rc != OK || params == nullptr) {
            return rc;
        }

        bool is_refused_status_read = refuses_link_status && reads_link_status(command, params);
        if (reads_unconnected_monitor(command, params) || is_refused_status_read) {
            return ERR_FIRMWARE;
        }

        if (command == rm::CONTROL_DISPLAY_CHANGE) {
            return answer_mode_set(*static_cast<const rm::display_change_params*>(params));
        }

        fill_reply(command, params);
        fill_output_reply(command, params);
        fill_check_reply(command, params);
        fill_state_reply(command, params);
        fill_link_reply(command, params);

        keep_training(command, params);
        count_repeater_status_read(command, params);

        return OK;
    }

    int32_t release(rm::handle object) override {
        return answer(kind::release, object, 0);
    }

    int32_t internal_control(uint32_t command, void* params, size_t) override {
        int32_t rc = answer(kind::internal_control, 0, command);
        if (rc == OK && command == rm::CONTROL_GET_DISPLAY_STATIC_INFO) {
            static_cast<rm::display_static_info_params*>(params)->present_windows = present_windows;
        }

        if (command != rm::CONTROL_SET_DISPLAY_PUSH_BUFFER) {
            return rc;
        }

        const rm::display_push_buffer_params* location = static_cast<rm::display_push_buffer_params*>(params);
        push_buffer_address = location->address;
        push_buffer_class = location->channel_class;
        push_buffer_instance = location->channel_instance;
        if (push_buffer_class == TEST_CORE_CHANNEL_CLASS) {
            core_push_buffer_address = location->address;
        }

        bool is_refused_class = refused_push_buffer_class != 0 && push_buffer_class == refused_push_buffer_class;
        return is_refused_class ? ERR_FIRMWARE : rc;
    }

    rm::handle new_handle() override {
        return FIRST_HANDLE + handle_count++;
    }

    rm::handle client() const override {
        return reported_client;
    }

    // The handle of the allocation of `class_id`, or 0 when none was made
    rm::handle allocated(uint32_t class_id) const {
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::allocation && calls[i].number == class_id) {
                return calls[i].object;
            }
        }

        return 0;
    }

    // How many of the calls so far are controls of `command` sent to `object`, an `object` of 0 matching any
    uint32_t control_count(uint32_t command, rm::handle object = 0) const {
        uint32_t controls = 0;
        for (uint32_t i = 0; i < recorded(); i++) {
            bool is_control = calls[i].made == kind::control;
            bool is_command = calls[i].number == command;
            bool is_object = object == 0 || calls[i].object == object;
            if (is_control && is_command && is_object) {
                controls++;
            }
        }

        return controls;
    }

    // The index of the first of the calls so far that is a control of `command`, or `MAX_CALLS` when none is
    uint32_t first_control(uint32_t command) const {
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::control && calls[i].number == command) {
                return i;
            }
        }

        return MAX_CALLS;
    }

    // The index of the last of the calls so far that is a control of `command`, or `MAX_CALLS` when none is
    uint32_t last_control(uint32_t command) const {
        uint32_t last = MAX_CALLS;
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::control && calls[i].number == command) {
                last = i;
            }
        }

        return last;
    }

    // How many of the calls so far release an object
    uint32_t release_count() const {
        uint32_t releases = 0;
        for (uint32_t i = 0; i < recorded(); i++) {
            if (calls[i].made == kind::release) {
                releases++;
            }
        }

        return releases;
    }

    // Whether the calls from `first` on release `object` exactly once
    bool releases_once(uint32_t first, rm::handle object) const {
        uint32_t releases = 0;
        for (uint32_t i = first; i < recorded(); i++) {
            if (calls[i].made == kind::release && calls[i].object == object) {
                releases++;
            }
        }

        return releases == 1;
    }

    // Whether the calls from `first` on begin with every call `other` recorded, in the same order
    bool begins_with(uint32_t first, const engine_firmware& other) const {
        if (first + other.recorded() > recorded()) {
            return false;
        }

        for (uint32_t i = 0; i < other.recorded(); i++) {
            const call& mine = calls[first + i];
            const call& theirs = other.calls[i];

            bool is_same_kind = mine.made == theirs.made;
            bool is_same_object = mine.object == theirs.object;
            bool is_same_number = mine.number == theirs.number;
            if (!is_same_kind || !is_same_object || !is_same_number) {
                return false;
            }
        }

        return true;
    }

private:
    // How many calls `calls` holds
    uint32_t recorded() const {
        return call_count < MAX_CALLS ? call_count : MAX_CALLS;
    }

    // The bytes of a shutdown update stopping `stopped_heads` heads: each present window's owner and usage bounds, each
    // head's four methods and the update's five, each method a header and one value
    static uint32_t shutdown_update_bytes(uint32_t windows, uint32_t stopped_heads) {
        uint32_t window_count = 0;
        for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
            if ((windows & (1u << window)) != 0) {
                window_count++;
            }
        }

        uint32_t methods = window_count * 2 + stopped_heads * 4 + 5;
        return methods * 2 * sizeof(uint32_t);
    }

    // The bytes of the light's update on `head` after a shutdown update: the head's fixed methods, a usage bound for
    // each present window serving it, the lit window's three usage methods and the update's five
    static uint32_t light_update_bytes(uint32_t windows, uint8_t windows_per_head, uint8_t head) {
        uint32_t served = 0;
        for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
            bool is_present = (windows & (1u << window)) != 0;
            if (is_present && window / windows_per_head == head) {
                served++;
            }
        }

        uint32_t methods = HEAD_LIGHT_FIXED_METHODS + served + WINDOW_USAGE_METHODS + 5;

        return methods * 2 * sizeof(uint32_t);
    }

    // From the stream's attributes, the link's last request ahead of the light, leaves the core channel's GET at
    // `get_at_stream`, or, with a head showing a display, where the light's update after the shutdown update ends
    void move_get_past_light() const {
        uint32_t& get = g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)];
        uint32_t put = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
        if (get_at_stream != 0) {
            get = get_at_stream;
        } else if (boot_display_id != 0) {
            get = put + light_update_bytes(present_windows, windows_per_head, boot_head);
        }
    }

    // Records the mode set's start or end, answering the end with `mode_set_end_result`, and from the start leaves the
    // core channel's GET at `get_at_mode_set_start`, or, with a head showing a display, where the shutdown update ends
    int32_t answer_mode_set(const rm::display_change_params& params) {
        if (mode_set_requests < 2) {
            mode_set_ids[mode_set_requests] = params.display_ids;
            mode_set_phases[mode_set_requests] = params.phase;
            mode_set_puts[mode_set_requests] = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
            mode_set_ns[mode_set_requests] = clock::now_ns();
        }

        mode_set_requests++;
        if (params.phase == rm::DISPLAY_CHANGE_END) {
            return mode_set_end_result;
        }

        uint32_t& get = g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)];
        uint32_t put = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
        if (get_at_mode_set_start != 0) {
            get = get_at_mode_set_start;
        } else if (boot_display_id != 0) {
            get = put + shutdown_update_bytes(present_windows, other_display_id != 0 ? heads : 1);
        }

        return OK;
    }

    // Records the call and gives the result of the first refusal matching it, else `OK`
    int32_t answer(kind made, rm::handle object, uint32_t number) {
        if (call_count < MAX_CALLS) {
            calls[call_count] = {.made = made, .object = object, .number = number};
        }

        call_count++;

        for (const refusal& refused : refusals) {
            bool is_number = refused.number == 0 || refused.number == number;
            bool is_object = refused.object == 0 || refused.object == object;
            if (refused.result != OK && refused.made == made && is_number && is_object) {
                return refused.result;
            }
        }

        return OK;
    }

    // Whether `command` reads the EDID or the DPCD of a display id not reported connected, a read the double refuses
    bool reads_unconnected_monitor(uint32_t command, const void* params) const {
        uint32_t display_id = 0;
        if (command == rm::CONTROL_GET_EDID) {
            display_id = static_cast<const rm::edid_params*>(params)->display_id;
        } else if (command == rm::CONTROL_DP_AUX) {
            display_id = static_cast<const rm::aux_params*>(params)->display_id;
        } else {
            return false;
        }

        return (display_id & connected_ids) == 0;
    }

    // Whether `command` is a DPCD read from `link_status_address`
    bool reads_link_status(uint32_t command, const void* params) const {
        bool is_aux = command == rm::CONTROL_DP_AUX;
        return is_aux && static_cast<const rm::aux_params*>(params)->address == link_status_address;
    }

    // Counts a DPCD read past the PHY repeaters' capability bytes
    void count_repeater_status_read(uint32_t command, const void* params) {
        uint32_t capabilities_end = DPCD_PHY_REPEATER_CAPABILITIES + DPCD_PHY_REPEATER_CAPABILITY_BYTES;
        bool is_aux = command == rm::CONTROL_DP_AUX;
        if (is_aux && static_cast<const rm::aux_params*>(params)->address >= capabilities_end) {
            repeater_status_reads++;
        }
    }

    // Keeps the command and data of a training request
    void keep_training(uint32_t command, const void* params) {
        if (command != rm::CONTROL_TRAIN_LINK) {
            return;
        }

        const rm::train_link_params* training = static_cast<const rm::train_link_params*>(params);
        training_command = training->command;
        training_data = training->data;
    }

    // Acknowledges a DPCD read with the bytes of `dpcd_receiver` from its address, of `link_status` from
    // `link_status_address` and of `phy_repeater_capabilities` from theirs, every other byte reading 0
    void fill_aux_reply(rm::aux_params& reply) const {
        uint32_t bytes = reply.size + 1;
        for (uint32_t i = 0; i < bytes; i++) {
            uint32_t address = reply.address + i;
            uint32_t status_byte = address - link_status_address;
            uint32_t repeater_byte = address - DPCD_PHY_REPEATER_CAPABILITIES;
            bool is_repeater_byte = address >= DPCD_PHY_REPEATER_CAPABILITIES;
            is_repeater_byte = is_repeater_byte && repeater_byte < DPCD_PHY_REPEATER_CAPABILITY_BYTES;
            if (address < DPCD_RECEIVER_CAPABILITY_BYTES) {
                reply.data[i] = dpcd_receiver[address];
            } else if (link_status != nullptr && address >= link_status_address && status_byte < 3) {
                reply.data[i] = link_status[status_byte];
            } else if (phy_repeater_capabilities != nullptr && is_repeater_byte) {
                reply.data[i] = phy_repeater_capabilities[repeater_byte];
            }
        }

        reply.size = bytes;
        reply.reply = rm::AUX_REPLY_ACK;
    }

    // Gives `heads` heads the display engine can use, from head 0, each with `windows_per_head` windows, the display
    // ids the fields name, link replies from the `boot_link_*` fields, and a crossbar if `has_crossbar`
    void fill_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_GET_HEAD_COUNT) {
            static_cast<rm::head_count_params*>(params)->head_count = heads;
        } else if (command == rm::CONTROL_GET_HEAD_MASK) {
            static_cast<rm::head_mask_params*>(params)->head_mask = (1u << heads) - 1;
        } else if (command == rm::CONTROL_GET_WINDOW_HEADS) {
            rm::window_heads_params* reply = static_cast<rm::window_heads_params*>(params);
            for (uint8_t window = 0; window < heads * windows_per_head; window++) {
                reply->window_heads[window] = static_cast<uint8_t>(1u << (window / windows_per_head));
            }
        } else if (command == rm::CONTROL_GET_ACTIVE_DISPLAY) {
            rm::active_display_params* reply = static_cast<rm::active_display_params*>(params);
            reply->display_id = reply->head == boot_head ? boot_display_id : other_display_id;
        } else if (command == rm::CONTROL_GET_OUTPUT_RESOURCE) {
            rm::output_resource_params* reply = static_cast<rm::output_resource_params*>(params);
            reply->index = boot_link_index;
            reply->type = boot_link_type;
            reply->protocol = boot_link_protocol;
        } else if (command == rm::CONTROL_GET_DISPLAY_COMMON_CAPABILITIES && has_crossbar) {
            uint8_t* table = static_cast<rm::display_common_capabilities_params*>(params)->table;
            table[rm::DISPLAY_COMMON_CROSSBAR_BYTE] = rm::DISPLAY_COMMON_CROSSBAR_BIT;
        }
    }

    // Reports `supported_ids` as digital display ids giving `digital_facts`, those in `connected_ids` connected, each
    // connected monitor giving `g_edid` as its EDID and `fill_aux_reply`'s bytes as its DPCD
    void fill_output_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_GET_SUPPORTED_DISPLAYS) {
            static_cast<rm::supported_displays_params*>(params)->display_mask = supported_ids;
        } else if (command == rm::CONTROL_GET_CONNECTED_DISPLAYS) {
            static_cast<rm::connected_displays_params*>(params)->display_mask = connected_ids;
        } else if (command == rm::CONTROL_GET_DISPLAY_KIND) {
            static_cast<rm::display_kind_params*>(params)->display_kind = rm::DISPLAY_KIND_DIGITAL;
        } else if (command == rm::CONTROL_GET_DIGITAL_DISPLAY_FACTS) {
            static_cast<rm::digital_display_facts_params*>(params)->flags = digital_facts;
        } else if (command == rm::CONTROL_GET_EDID) {
            rm::edid_params* reply = static_cast<rm::edid_params*>(params);
            reply->edid_bytes = sizeof(g_edid);
            string::memcpy(reply->edid, g_edid, sizeof(g_edid));
        } else if (command == rm::CONTROL_DP_AUX) {
            fill_aux_reply(*static_cast<rm::aux_params*>(params));
        }
    }

    // Answers the bandwidth check, refusing the first `refused_modes` requests
    void fill_check_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_IS_MODE_POSSIBLE) {
            bool is_refused = control_count(rm::CONTROL_IS_MODE_POSSIBLE) <= refused_modes;
            static_cast<rm::mode_possible_params*>(params)->is_possible = is_refused ? 0 : 1;
        }
    }

    // Answers a channel state request with the busy state if `is_channel_busy`, else the idle state
    void fill_state_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_GET_DISPLAY_CHANNEL_STATE) {
            uint32_t state = is_channel_busy ? NVIDIA_CHANNEL_STATE_BUSY : rm::CHANNEL_STATE_IDLE;
            static_cast<rm::display_channel_state_params*>(params)->state = state;
        }
    }

    // Answers an assignment with `assigned_resource` driving the display id alone, and a request for an output
    // resource's DisplayPort facts from the `displayport_*` fields, with FEC and the increased watermark limits
    void fill_link_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_ASSIGN_OUTPUT_RESOURCE) {
            rm::assign_output_resource_params* reply = static_cast<rm::assign_output_resource_params*>(params);
            reply->resources[assigned_resource] = {.display_mask = reply->display_id, .type = rm::ASSIGNED_SINGLE};
        } else if (command == rm::CONTROL_GET_LINK_CAPABILITIES) {
            rm::link_capabilities_params* reply = static_cast<rm::link_capabilities_params*>(params);
            reply->max_rate = displayport_max_rate;
            reply->versions = displayport_versions;
            reply->increased_watermark_limits = 1;
            reply->fec_supported = 1;
            reply->trains_phy_repeaters = displayport_trains_phy_repeaters;
        }
    }
};

// A video memory giving general memory in whole pages from offset 0 of `g_video_memory`, mapped one range at a time
// through its copy, and whole scanout pages at `SCANOUT_OFFSET` over `scanout`, counting calls and failing them as set
class engine_video_memory : public video_memory {
public:
    int32_t  general_allocate_result = OK;
    uint32_t failing_general_request = 0;       // the number, from 1, of the general request failing, 0 for none
    int32_t  general_map_result      = OK;
    uint32_t failing_general_map     = 0;       // the number, from 1, of the general mapping failing, 0 for none
    int32_t  second_map_result       = OK;
    uint32_t second_failing_map      = 0;       // a later general mapping failing with `second_map_result`, 0 for none
    uint64_t general_bytes           = 0;       // handed out from offset 0
    uint64_t general_size            = 0;       // of the last general request
    uint64_t general_alignment       = 0;       // of the last general request
    uint32_t general_requests        = 0;
    uint32_t general_maps            = 0;
    uint32_t general_unmaps          = 0;
    uint32_t general_releases        = 0;
    uint32_t general_overruns        = 0;       // general unmaps finding the guard past the range written
    int32_t  scanout_allocate_result = OK;
    int32_t  scanout_map_result      = OK;
    uint32_t failing_scanout_map     = 0;       // the number, from 1, of the scanout mapping failing, 0 for every one
    uint32_t physless_scanout_map    = 0;       // which scanout mapping, from 1, gets no physical address, 0 for none
    uint64_t scanout_bytes           = 0;       // backed by `scanout` once `give_scanout` sets it
    uint8_t* scanout                 = nullptr; // `scanout_bytes` and their guard, on the unprivileged heap, else null
    uint64_t scanout_size            = 0;       // of the last scanout request
    uint64_t scanout_alignment       = 0;       // of the last scanout request
    uint32_t scanout_requests        = 0;
    uint32_t scanout_maps            = 0;
    uint32_t scanout_unmaps          = 0;
    uint64_t scanout_fill_words      = 0;       // 64-bit words of the scanout range left unwritten at its last unmap
    uint32_t scanout_releases        = 0;

    vram_range general_mapped   = {}; // the general range mapped, of 0 bytes while none is
    vram_range general_released = {}; // the last general range released

    ~engine_video_memory() override {
        if (scanout != nullptr) {
            heap::ufree(scanout);
        }
    }

    // Backs `bytes` of scanout memory and `SCANOUT_GUARD_BYTES` after them, all filled with `SCANOUT_FILL`, or gives
    // false when the unprivileged heap cannot hold them
    bool give_scanout(uint64_t bytes) {
        scanout = static_cast<uint8_t*>(heap::ualloc(bytes + SCANOUT_GUARD_BYTES));
        if (scanout == nullptr) {
            return false;
        }

        string::memset(scanout, SCANOUT_FILL, bytes + SCANOUT_GUARD_BYTES);
        scanout_bytes = bytes;

        return true;
    }

    int32_t allocate(uint64_t size, uint64_t alignment, vram_use use, vram_range& out) override {
        if (use == vram_use::scanout) {
            scanout_requests++;
            scanout_size = size;
            scanout_alignment = alignment;
            if (scanout_allocate_result != OK) {
                return scanout_allocate_result;
            }

            uint64_t pages_bytes = (size + DOUBLE_PAGE_BYTES - 1) & ~(DOUBLE_PAGE_BYTES - 1);
            if (pages_bytes > scanout_bytes) {
                return ERR_NO_MEMORY;
            }

            out = {.offset = SCANOUT_OFFSET, .size = pages_bytes};
            return OK;
        }

        general_requests++;
        general_size = size;
        general_alignment = alignment;
        if (general_requests == failing_general_request) {
            return general_allocate_result;
        }

        uint64_t pages_bytes = (size + DOUBLE_PAGE_BYTES - 1) & ~(DOUBLE_PAGE_BYTES - 1);
        if (pages_bytes > sizeof(g_video_memory) - general_bytes) {
            return ERR_NO_MEMORY;
        }

        out = {.offset = general_bytes, .size = pages_bytes};
        general_bytes += pages_bytes;
        string::memset(g_video_memory + out.offset, SCANOUT_FILL, pages_bytes);

        return OK;
    }

    void release(const vram_range& range) override {
        if (range.offset == SCANOUT_OFFSET) {
            scanout_releases++;
        } else {
            general_releases++;
            general_released = range;
        }
    }

    int32_t map_for_cpu(const vram_range& range, cpu_mapping& out) override {
        if (range.offset != SCANOUT_OFFSET) {
            general_maps++;
            if (general_maps == failing_general_map) {
                return general_map_result;
            }

            if (general_maps == second_failing_map) {
                return second_map_result;
            }

            uint8_t* view = g_mapped_video_memory + range.offset;
            string::memcpy(view, g_video_memory + range.offset, range.size);
            string::memset(view + range.size, SCANOUT_FILL, GENERAL_GUARD_BYTES);
            general_mapped = range;

            out.virt = reinterpret_cast<uintptr_t>(view);
            out.phys = 0;
            return OK;
        }

        scanout_maps++;
        bool is_failing = failing_scanout_map == 0 || scanout_maps == failing_scanout_map;
        if (scanout_map_result != OK && is_failing) {
            return scanout_map_result;
        }

        bool is_physless = scanout_maps == physless_scanout_map;
        out.virt = reinterpret_cast<uintptr_t>(scanout);
        out.phys = is_physless ? 0 : SCANOUT_BAR1_ADDRESS + (scanout_maps - 1) * SCANOUT_BAR1_STRIDE;

        return OK;
    }

    void unmap_for_cpu(const vram_range& range) override {
        if (range.offset == SCANOUT_OFFSET) {
            scanout_unmaps++;
            scanout_fill_words = 0;
            for (uint64_t offset = 0; offset < range.size; offset += sizeof(uint64_t)) {
                uint64_t word = 0;
                string::memcpy(&word, scanout + offset, sizeof(word));
                if (word == SCANOUT_FILL_WORD) {
                    scanout_fill_words++;
                }
            }

            return;
        }

        general_unmaps++;
        bool is_mapped = general_mapped.size != 0 && range.offset == general_mapped.offset;
        if (!is_mapped) {
            return;
        }

        general_mapped = {};
        uint8_t* view = g_mapped_video_memory + range.offset;
        string::memcpy(g_video_memory + range.offset, view, range.size);
        for (uint64_t index = 0; index < GENERAL_GUARD_BYTES; index++) {
            if (view[range.size + index] != SCANOUT_FILL) {
                general_overruns++;
                return;
            }
        }
    }
};

namespace drivers::nvidia {

// Sets the wait an engine gives the completion notifier, which no display engine writes, and reaches the engine's
// notifier, look-up table, context DMAs, window queue, take-down and lit output's vertical blanks
struct display_engine_test_access {
    static void set_notifier_wait(display_engine& engine, uint64_t wait_ns) {
        engine.m_notifier_wait_ns = wait_ns;
    }

    static uint64_t notifier_wait_ns(const display_engine& engine) {
        return engine.m_notifier_wait_ns;
    }

    static rm::handle notifier_context_dma(const display_engine& engine) {
        return engine.m_notifier_context_dma;
    }

    static rm::handle base_buffer_context_dma(const display_engine& engine) {
        return engine.m_base_buffer_context_dma;
    }

    static const vram_range& look_up_table(const display_engine& engine) {
        return engine.m_look_up_table;
    }

    static rm::handle look_up_table_context_dma(const display_engine& engine) {
        return engine.m_look_up_table_context_dma;
    }

    static display_channel* window_queue(display_engine& engine) {
        return engine.m_window_queue;
    }

    static uint8_t lit_head(const display_engine& engine) {
        return engine.m_lit_head;
    }

    static uint8_t head_output(const display_engine& engine, uint8_t head) {
        return engine.m_head_outputs[head];
    }

    static uint32_t frames_per_second(const display_engine& engine) {
        return engine.m_frames_per_second;
    }

    static uint64_t frame_count(const display_engine& engine) {
        return engine.m_frame_count;
    }

    static const first_second_statistics& first_second(const display_engine& engine) {
        return engine.m_first_second;
    }

    static int32_t take_down(
        display_engine& engine,
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active,
        const active_heads& lit
    ) {
        return engine.take_down(heads, present_windows, active, lit);
    }

    static uint8_t lit_window(const display_engine& engine) {
        return engine.m_lit_window;
    }

    static rm::handle window_notifier_context_dma(const display_engine& engine) {
        return engine.m_window_notifier_context_dma;
    }

    static uint64_t window_notifier_address(const display_engine& engine) {
        return engine.m_window_notifier.physical_address();
    }

    static bool has_queued_present(const display_engine& engine) {
        return engine.m_has_queued_present;
    }

    static uint64_t last_reported_frame(const display_engine& engine) {
        return engine.m_last_reported_frame;
    }

    static bool is_latch_check_scheduled(const display_engine& engine) {
        return timer::is_pending(&engine.m_latch_check_timer);
    }

    static bool is_deadline_scheduled(const display_engine& engine) {
        return timer::is_pending(&engine.m_deadline_timer);
    }

    static void set_mode_times(display_engine& engine, const mode_times& times) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
            engine.m_mode_times = times;
            sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
        });
    }

    // Moves the queued present's deadline to `deadline_ns` but not its timer, for `run_deadline` to decide by
    static void move_deadline(display_engine& engine, uint64_t deadline_ns) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
            engine.m_queued_deadline_ns = deadline_ns;
            sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
        });
    }

    // Moves the queued present's deadline and its timer out of reach, so only the test ends the present. True when
    // the timer was scheduled
    static bool hold_deadline(display_engine& engine) {
        bool was_scheduled = false;
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
            engine.m_queued_deadline_ns = ~0ull;
            was_scheduled = timer::schedule(&engine.m_deadline_timer, ~0ull);
            sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
        });
        return was_scheduled;
    }

    static uint64_t queued_deadline_ns(const display_engine& engine) {
        return engine.m_queued_deadline_ns;
    }

    static uint64_t deadline_timer_ns(const display_engine& engine) {
        return engine.m_deadline_timer.deadline_ns;
    }

    static uint64_t latch_check_timer_ns(const display_engine& engine) {
        return engine.m_latch_check_timer.deadline_ns;
    }

    static uint64_t vertical_blank_ns(const display_engine& engine) {
        return engine.m_vertical_blank_ns;
    }

    static void set_last_estimate_log_ns(display_engine& engine, uint64_t log_ns) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(engine.m_output_lock);
            engine.m_last_estimate_log_ns = log_ns;
            sync::spin_unlock_irqrestore(engine.m_output_lock, irq);
        });
    }

    // Has `engine` report the presents it ends to `record` in place of gfxfb
    static void set_present_done(
        display_engine& engine,
        void (*record)(gfxfb::display*, uint32_t, const gfxfb::present_result&)
    ) {
        engine.m_present_done = record;
    }

    // Cancels both timers of an engine about to be rebuilt over, so neither fires into the next
    static void cancel_timers(display_engine& engine) {
        RUN_ELEVATED({
            timer::cancel(&engine.m_latch_check_timer);
            timer::cancel(&engine.m_deadline_timer);
        });
    }

    static void run_latch_check(display_engine& engine) {
        display_engine::check_latch(&engine.m_latch_check_timer);
    }

    static void run_deadline(display_engine& engine) {
        display_engine::expire_deadline(&engine.m_deadline_timer);
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::display_engine_test_access;

// An engine over `fw` and `memory`, built in `g_engine_bytes` once the timers of the engine built there before are
// cancelled, so neither fires into it, giving its wait for the notifier no time
static display_engine& new_engine(engine_firmware& fw, engine_video_memory& memory) {
    if (g_built_engine != nullptr) {
        display_engine_test_access::cancel_timers(*g_built_engine);
    }

    g_built_engine = new (g_engine_bytes) display_engine(fw, memory);
    display_engine& engine = *g_built_engine;
    display_engine_test_access::set_notifier_wait(engine, 0);
    return engine;
}

// Sets up `engine` for a card driving `address_bits` physical address bits, with four heads, eight windows, links doing
// `links` and output scalers when `heads_have_output_scalers`, the core channel's GET reading `get`, no DMA page freed
static int32_t set_up(
    display_engine& engine,
    uint8_t address_bits = FULL_ADDRESS_BITS,
    uint32_t get = INITIAL_METHODS_END,
    const chip_displayport& links = CHIP_LINKS,
    bool heads_have_output_scalers = HEADS_WITH_OUTPUT_SCALERS
) {
    g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)] = get;
    registers regs(reinterpret_cast<uintptr_t>(g_core_channel_registers) - CORE_CHANNEL_AREA);
    dma::device_profile card = dma::device_profile_test_access::build(address_bits, true);

    chip_info chip = {};
    chip.display = {
        .head_count                = CHIP_HEADS,
        .window_count              = CHIP_WINDOWS,
        .serial_link_count         = CHIP_SERIAL_LINKS,
        .display_class             = TEST_DISPLAY_CLASS,
        .core_channel_class        = TEST_CORE_CHANNEL_CLASS,
        .window_channel_class      = TEST_WINDOW_CHANNEL_CLASS,
        .heads_have_output_scalers = heads_have_output_scalers,
        .displayport               = links,
    };

    return engine.set_up(regs, card, chip, TEST_DEVICE);
}

// The 32-bit word at `offset` in the instance memory, as the last unmap wrote it back to `g_video_memory`
static uint32_t instance_word(uint64_t offset) {
    uint32_t word = 0;
    string::memcpy(&word, g_video_memory + offset, sizeof(word));
    return word;
}

// How many hash table entries name a context DMA
static uint32_t used_entry_count() {
    uint32_t used = 0;
    for (uint32_t entry = 0; entry < NVIDIA_HASH_ENTRIES; entry++) {
        if (instance_word(entry * NVIDIA_ENTRY_BYTES) != 0) {
            used++;
        }
    }

    return used;
}

// The first hash table entry naming `object`, or `NVIDIA_HASH_ENTRIES` when none does
static uint32_t entry_of(rm::handle object) {
    for (uint32_t entry = 0; entry < NVIDIA_HASH_ENTRIES; entry++) {
        if (instance_word(entry * NVIDIA_ENTRY_BYTES) == object) {
            return entry;
        }
    }

    return NVIDIA_HASH_ENTRIES;
}

// The 256-byte unit the first record's words `index` and `index + 1` name, low word first
static uint64_t first_record_unit(uint32_t index) {
    uint64_t low = instance_word(NVIDIA_FIRST_RECORD_OFFSET + index * sizeof(uint32_t));
    uint64_t high = instance_word(NVIDIA_FIRST_RECORD_OFFSET + (index + 1) * sizeof(uint32_t));
    return low | (high << 32);
}

// Copies `AOC_BASE_BLOCK` into `g_edid`, with its first timing unmarked preferred unless `is_first_preferred`, so its
// 143.911 Hz timing ranks first
static void copy_aoc_edid(bool is_first_preferred) {
    string::memcpy(g_edid, AOC_BASE_BLOCK, sizeof(g_edid));
    if (is_first_preferred) {
        return;
    }

    g_edid[FEATURES_BYTE] = static_cast<uint8_t>(g_edid[FEATURES_BYTE] & ~FIRST_PREFERRED);
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] + FIRST_PREFERRED);
}

// Zeroes `bytes` bytes of `g_edid` from `first`, keeping its checksum right, so timing descriptors there become display
// descriptors of a kind the parser passes over
static void zero_edid_bytes(uint32_t first, uint32_t bytes) {
    uint8_t zeroed_sum = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        zeroed_sum = static_cast<uint8_t>(zeroed_sum + g_edid[first + i]);
    }

    string::memset(g_edid + first, 0, bytes);
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] + zeroed_sum);
}

// Copies `AOC_BASE_BLOCK` into `g_edid` with its timing descriptors zeroed, so the EDID parses with no timing
static void copy_aoc_edid_without_timings() {
    copy_aoc_edid(true);
    zero_edid_bytes(TIMING_DESCRIPTORS_BYTE, TIMING_DESCRIPTORS_BYTES);
}

// Copies `AOC_BASE_BLOCK` into `g_edid` with its second timing descriptor zeroed, so the EDID gives only its preferred
// 59.950 Hz timing
static void copy_aoc_edid_with_one_timing() {
    copy_aoc_edid(true);
    zero_edid_bytes(TIMING_DESCRIPTORS_BYTE + DESCRIPTOR_BYTES, DESCRIPTOR_BYTES);
}

// Copies `AOC_BASE_BLOCK` into `g_edid` with no timing preferred and its range limits replaced by a third timing, 1920
// by 1080 at 60.000 Hz, so that a limit to 60 Hz keeps 2 of its 3 timings
static void copy_aoc_edid_with_a_third_timing() {
    copy_aoc_edid(false);

    uint8_t replaced_sum = 0;
    for (uint32_t i = 0; i < sizeof(TIMING_1080_AT_60_HZ); i++) {
        replaced_sum = static_cast<uint8_t>(replaced_sum + g_edid[RANGE_DESCRIPTOR_BYTE + i] - TIMING_1080_AT_60_HZ[i]);
    }

    string::memcpy(g_edid + RANGE_DESCRIPTOR_BYTE, TIMING_1080_AT_60_HZ, sizeof(TIMING_1080_AT_60_HZ));
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] + replaced_sum);
}

// An engine that has set up nothing sends no request on its release, which returns `OK`
TEST(nvidia_display_engine, release_before_set_up_sends_nothing) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.call_count, 0u);
}

// After a full set-up the release first sends what `release_display_channel` sends for the core channel and the display
// it was allocated under, then releases each of the display's objects once, and a second release sends nothing
TEST(nvidia_display_engine, release_frees_the_core_channel_before_the_objects_once) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle display = fw.allocated(TEST_DISPLAY_CLASS);
    rm::handle bandwidth_check = fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    engine_firmware channel_release;
    EXPECT_EQ(release_display_channel(channel_release, display, channel, true), OK);

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.begins_with(first, channel_release));

    EXPECT_TRUE(fw.releases_once(first, channel));
    EXPECT_TRUE(fw.releases_once(first, display));
    EXPECT_TRUE(fw.releases_once(first, bandwidth_check));
    EXPECT_TRUE(fw.releases_once(first, display_common));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.call_count, first + 5);
}

// A refused release of the core channel still releases the objects, and the release returns the channel's error rather
// than a refused object release's
TEST(nvidia_display_engine, refused_core_channel_release_still_releases_the_objects) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    uint32_t first = fw.call_count;

    fw.refusals[0] = {.made = engine_firmware::kind::release, .number = 0, .object = channel, .result = ERR_TIMEOUT};
    fw.refusals[1] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = display_common,
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(engine.release(), ERR_TIMEOUT);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, channel));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, display_common));
}

// When the core channel's release succeeds, the release returns a refused object release's error
TEST(nvidia_display_engine, object_release_error_is_returned_when_the_channel_release_succeeds) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
    uint32_t first = fw.call_count;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = fw.allocated(rm::CLASS_DISPLAY_COMMON),
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(engine.release(), ERR_FIRMWARE);
    EXPECT_TRUE(fw.releases_once(first, channel));
}

// A core channel whose allocation failed is released only after an error that may leave the firmware holding it, and
// the objects are released either way
TEST(nvidia_display_engine, failed_core_channel_is_released_only_when_the_firmware_may_hold_it) {
    struct failure {
        int32_t error;
        bool    is_channel_released;
    };

    const failure failures[] = {{ERR_TIMEOUT, true}, {ERR_FIRMWARE, false}};
    for (const failure& failed : failures) {
        engine_firmware fw;
        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);
        fw.refusals[0] = {
            .made   = engine_firmware::kind::allocation,
            .number = TEST_CORE_CHANNEL_CLASS,
            .object = 0,
            .result = failed.error,
        };

        EXPECT_EQ(set_up(engine), failed.error);
        EXPECT_EQ(fw.release_count(), 0u);
        uint32_t first = fw.call_count;

        EXPECT_EQ(engine.release(), OK);
        ASSERT_EQ(fw.call_count, first + (failed.is_channel_released ? 5 : 3));
        EXPECT_EQ(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)), failed.is_channel_released);
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
    }
}

// A set-up refused at the display's capabilities keeps the objects it allocated until the release frees them, with no
// request for a core channel it never allocated
TEST(nvidia_display_engine, set_up_failing_at_the_capabilities_keeps_the_objects_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_DISPLAY_CAPABILITIES,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    EXPECT_EQ(set_up(engine), ERR_FIRMWARE);
    EXPECT_EQ(fw.allocated(TEST_CORE_CHANNEL_CLASS), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 3);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up whose push buffer cannot be allocated fails with `ERR_NO_MEMORY` and keeps the objects until the release
// frees them, with no request for a core channel
TEST(nvidia_display_engine, set_up_failing_at_the_push_buffer_keeps_the_objects_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up(engine, REFUSED_ADDRESS_BITS), ERR_NO_MEMORY);
    EXPECT_EQ(fw.allocated(TEST_CORE_CHANNEL_CLASS), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 3);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up binds the notifier's context DMA, a new handle, to the core channel under the driver's client in the
// instance memory's only entry, its first record naming one snooped page other than the push buffer's
TEST(nvidia_display_engine, set_up_binds_the_completion_notifier_to_the_core_channel) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    rm::handle context_dma = FIRST_HANDLE + fw.handle_count - 1;
    EXPECT_NE(context_dma, fw.allocated(TEST_CORE_CHANNEL_CLASS));
    EXPECT_EQ(display_engine_test_access::notifier_context_dma(engine), context_dma);

    uint32_t entry = entry_of(context_dma);
    ASSERT_NE(entry, NVIDIA_HASH_ENTRIES);
    EXPECT_EQ(used_entry_count(), 1u);

    uint32_t client_bits = TEST_CLIENT & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = NVIDIA_FIRST_RECORD_FIELD << NVIDIA_RECORD_FIELD_SHIFT;
    EXPECT_EQ(instance_word(entry * NVIDIA_ENTRY_BYTES + sizeof(uint32_t)), client_bits | record_bits);

    uint64_t first_unit = first_record_unit(1);
    EXPECT_EQ(instance_word(NVIDIA_FIRST_RECORD_OFFSET), NVIDIA_SNOOPED_PAGES_WORD_0);
    EXPECT_NE(first_unit, 0u);
    EXPECT_EQ(first_unit % NVIDIA_NOTIFIER_UNITS, 0u);
    EXPECT_EQ(first_record_unit(3), first_unit + NVIDIA_NOTIFIER_UNITS - 1);

    ASSERT_NE(fw.push_buffer_address, 0u);
    EXPECT_NE(first_unit, fw.push_buffer_address / NVIDIA_RECORD_UNIT_BYTES);
}

// The double's client of 0, which `bind` refuses, stands for a refused binding: the set-up fails with `ERR_BAD_STATE`
// before any kick, writing no entry, and the core channel and the objects stay allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_the_notifiers_binding_kicks_nothing_and_keeps_the_core_channel) {
    engine_firmware fw;
    fw.reported_client = 0;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine), ERR_BAD_STATE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], 0u);
    EXPECT_EQ(used_entry_count(), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up kicks the core channel's initial methods for the double's two heads, its PUT left where they end
TEST(nvidia_display_engine, set_up_kicks_the_initial_methods_of_the_heads_read) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    ASSERT_EQ(set_up(engine), OK);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
}

// A set-up whose display ids give no outputs leaves the DisplayPort hardware to the firmware, as NVIDIA's library takes
// it only as it makes a connector's main link (its link to the monitor)
TEST(nvidia_display_engine, set_up_without_outputs_leaves_the_displayport_hardware_to_the_firmware) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_MANUAL_DISPLAYPORT), 0u);
}

// With two connected DisplayPort outputs, set-up tells display common once to leave the DisplayPort hardware to the
// driver, after the outputs' facts are read and before the monitors' EDIDs and DPCDs are
TEST(nvidia_display_engine, set_up_takes_the_displayport_hardware_before_the_monitors_are_read) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up(engine), OK);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_MANUAL_DISPLAYPORT), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_MANUAL_DISPLAYPORT, display_common), 1u);

    uint32_t take = fw.first_control(rm::CONTROL_SET_MANUAL_DISPLAYPORT);
    EXPECT_LT(fw.last_control(rm::CONTROL_GET_DIGITAL_DISPLAY_FACTS), take);
    EXPECT_LT(take, fw.first_control(rm::CONTROL_GET_EDID));
    EXPECT_LT(take, fw.first_control(rm::CONTROL_DP_AUX));
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_EDID), 2u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DP_AUX), 4u);
}

// A refusal to leave the DisplayPort hardware to the driver is no failure, so the monitor's EDID and DPCD are still
// read and the set-up succeeds
TEST(nvidia_display_engine, set_up_reads_the_monitors_after_a_refused_take_of_the_displayport_hardware) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_SET_MANUAL_DISPLAYPORT,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_MANUAL_DISPLAYPORT), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_EDID), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DP_AUX), 2u);
}

// Another error of the take fails the set-up with it, before any monitor is read or the core channel allocated
TEST(nvidia_display_engine, set_up_failing_at_the_take_of_the_displayport_hardware_reads_no_monitor) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_SET_MANUAL_DISPLAYPORT,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up(engine), ERR_TIMEOUT);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_EDID), 0u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DP_AUX), 0u);
    EXPECT_EQ(fw.allocated(TEST_CORE_CHANNEL_CLASS), 0u);
}

// An output with no monitor connected still has set-up take the DisplayPort hardware, which NVIDIA's library takes for
// every DisplayPort connector, while no EDID or DPCD is read
TEST(nvidia_display_engine, set_up_takes_the_displayport_hardware_for_an_output_with_no_monitor) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_MANUAL_DISPLAYPORT), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_EDID), 0u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DP_AUX), 0u);
}

// A GET past the push buffer's writable end fails the set-up with `ERR_BAD_STATE` once the initial methods are kicked,
// before any head's display id is asked, and the core channel and the objects stay allocated until the release
TEST(nvidia_display_engine, set_up_failing_at_the_initial_methods_keeps_the_core_channel_until_release) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine, FULL_ADDRESS_BITS, BAD_GET), ERR_BAD_STATE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_ACTIVE_DISPLAY), 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// The initial methods of four heads outgrow the push buffer, so a GET past its writable end fails the set-up with
// `ERR_BAD_STATE` in a write, before any kick, and the core channel and the objects stay allocated until the release
TEST(nvidia_display_engine, set_up_failing_in_a_write_kicks_nothing_and_keeps_the_core_channel) {
    engine_firmware fw;
    fw.heads = CHIP_HEADS;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine, FULL_ADDRESS_BITS, BAD_GET), ERR_BAD_STATE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], 0u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up asks display common for the display ids of the double's two heads, not the chip's four, only once the
// initial methods are kicked, and for the link of the one head showing a display id
TEST(nvidia_display_engine, set_up_reads_the_active_heads_once_the_initial_methods_are_kicked) {
    engine_firmware fw;
    fw.boot_display_id = 0x1;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    ASSERT_EQ(set_up(engine), OK);
    rm::handle display_common = fw.allocated(rm::CLASS_DISPLAY_COMMON);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_ACTIVE_DISPLAY), static_cast<uint32_t>(DOUBLE_HEADS));
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_ACTIVE_DISPLAY, display_common), static_cast<uint32_t>(DOUBLE_HEADS));
    EXPECT_EQ(fw.put_at_first_active_read, INITIAL_METHODS_END);

    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_OUTPUT_RESOURCE), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_OUTPUT_RESOURCE, display_common), 1u);
}

// A head reported showing several display ids fails the set-up with `ERR_BAD_MESSAGE` once the initial methods are
// kicked, and the core channel and the objects stay allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_the_active_heads_keeps_the_core_channel_until_release) {
    engine_firmware fw;
    fw.boot_display_id = 0x3;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

    EXPECT_EQ(set_up(engine), ERR_BAD_MESSAGE);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A refused link request and, without a crossbar, a link reply naming no link each fail the set-up once the initial
// methods are kicked, and the core channel and the objects stay allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_an_active_heads_link_keeps_the_core_channel_until_release) {
    engine_firmware refusing;
    refusing.boot_display_id = 0x1;
    refusing.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_OUTPUT_RESOURCE,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_firmware linkless;
    linkless.boot_display_id = 0x1;
    linkless.boot_link_index = rm::NO_OUTPUT_RESOURCE;

    engine_firmware* doubles[] = {&refusing, &linkless};
    const int32_t errors[] = {ERR_FIRMWARE, ERR_BAD_MESSAGE};
    for (uint32_t i = 0; i < 2; i++) {
        engine_firmware& fw = *doubles[i];
        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);
        g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)] = 0;

        EXPECT_EQ(set_up(engine), errors[i]);
        EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
        EXPECT_EQ(fw.release_count(), 0u);
        uint32_t first = fw.call_count;

        EXPECT_EQ(engine.release(), OK);
        ASSERT_EQ(fw.call_count, first + 5);
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
    }
}

// The text logged while `ENGINE_LOG_BACKEND` is set
static char     s_engine_log[16384] = {};
static uint32_t s_engine_log_bytes = 0;

// Keeps each logged line's text, up to the buffer's end
static void keep_engine_log(const char* data, size_t len) {
    for (size_t i = 0; i < len && s_engine_log_bytes < sizeof(s_engine_log); i++) {
        s_engine_log[s_engine_log_bytes++] = data[i];
    }
}

static constexpr log::backend ENGINE_LOG_BACKEND = {.write = keep_engine_log};

// How many times the kept text holds `text`
static uint32_t engine_log_count(const char* text) {
    size_t text_bytes = string::strlen(text);
    uint32_t count = 0;
    for (uint32_t start = 0; start + text_bytes <= s_engine_log_bytes; start++) {
        if (string::memcmp(s_engine_log + start, text, text_bytes) == 0) {
            count++;
        }
    }

    return count;
}

// Whether the kept text holds `text` anywhere
static bool engine_log_contains(const char* text) {
    return engine_log_count(text) != 0;
}

// Where `text` first begins in the kept text, or the kept text's length when it never does
static uint32_t engine_log_offset(const char* text) {
    size_t text_bytes = string::strlen(text);
    for (uint32_t start = 0; start + text_bytes <= s_engine_log_bytes; start++) {
        if (string::memcmp(s_engine_log + start, text, text_bytes) == 0) {
            return start;
        }
    }

    return s_engine_log_bytes;
}

// Sets up `engine` with the log kept, for a chip whose links do `links` and whose heads report output scalers when
// `heads_have_output_scalers`, and a card driving `address_bits` physical address bits
static int32_t set_up_logged(
    display_engine& engine,
    const chip_displayport& links = CHIP_LINKS,
    uint8_t address_bits = FULL_ADDRESS_BITS,
    bool heads_have_output_scalers = HEADS_WITH_OUTPUT_SCALERS
) {
    s_engine_log_bytes = 0;
    log::set_backend(&ENGINE_LOG_BACKEND);
    int32_t rc = set_up(engine, address_bits, INITIAL_METHODS_END, links, heads_have_output_scalers);
    log::set_backend(nullptr);

    return rc;
}

// The double reports no supported display id, so the display id 0x1 that head 0 shows names no output, and a set-up
// logs that head with it and its link, and as the boot head with no boot output, then that no output is chosen to light
TEST(nvidia_display_engine, set_up_logs_a_boot_head_whose_display_id_names_no_output) {
    engine_firmware fw;
    fw.boot_display_id = 0x1;
    fw.boot_link_index = 3;
    fw.boot_link_type = 2;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: head 0 shows display id 0x1, which names no output, through link 3 of NVIDIA's output resource type 2"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: head 0 is the boot head, and its display id 0x1 names no output, so there is no boot output"
    ));
    EXPECT_FALSE(engine_log_contains("nvidia: head 1 shows"));
    EXPECT_TRUE(engine_log_contains("nvidia: no output is chosen to light, as no connected output has a mode"));
}

// With no head showing a display id, a set-up logs that no head is active and names no boot head, then that no output
// is chosen to light
TEST(nvidia_display_engine, set_up_logs_that_no_head_is_active) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: no head is active, so there is no boot head"));
    EXPECT_FALSE(engine_log_contains("shows display id"));
    EXPECT_FALSE(engine_log_contains("is the boot head"));
    EXPECT_TRUE(engine_log_contains("nvidia: no output is chosen to light, as no connected output has a mode"));
}

// With display common reporting a crossbar, a set-up counts a head whose link reply names no link as showing nothing,
// so no head is active
TEST(nvidia_display_engine, set_up_counts_a_linkless_head_as_showing_nothing_with_a_crossbar) {
    engine_firmware fw;
    fw.has_crossbar = true;
    fw.boot_display_id = 0x1;
    fw.boot_link_index = rm::NO_OUTPUT_RESOURCE;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine can reach push buffers"));
    EXPECT_TRUE(engine_log_contains("and display common reports a crossbar"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: head 0 counts as showing nothing, as display common names no link for its display id 0x1"
    ));
    EXPECT_TRUE(engine_log_contains("nvidia: no head is active, so there is no boot head"));
}

// With both outputs on the AOC over four lanes at HBR3, a set-up logs each output's mode: the preferred 59.950 Hz
// timing over one lane at HBR3, ranked first of the two its link carries, without the ending of a 60 Hz limit
TEST(nvidia_display_engine, set_up_logs_the_mode_of_each_connected_output) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz, over 1 lane at DisplayPort rate "
        "0x1E, ranked first of the 2 timings its link carries"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz, over 1 lane at DisplayPort rate "
        "0x1E, ranked first of the 2 timings its link carries"
    ));
    EXPECT_FALSE(engine_log_contains("counting only those below 60.600 Hz"));
}

// With no timing preferred the AOC's 143.911 Hz timing, its EDID's timing 1, ranks first, which a set-up logs as the
// output's mode over four lanes at HBR2
TEST(nvidia_display_engine, set_up_logs_the_mode_of_an_output_whose_edid_prefers_no_timing) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    copy_aoc_edid(false);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's mode is its EDID's timing 1, 2560 by 1440 at 143.911 Hz, over 4 lanes at DisplayPort rate "
        "0x14, ranked first of the 2 timings its link carries"
    ));
}

// An EDID without the header does not parse, so a set-up gives its output no mode, logging only the EDID's failure
TEST(nvidia_display_engine, set_up_gives_no_mode_to_an_output_whose_edid_does_not_parse) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;

    copy_aoc_edid(true);
    g_edid[HEADER_FF_BYTE] = 0;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's 128-byte EDID does not start with the EDID header"
    ));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0's mode"));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0 has no mode"));
}

// Display common refusing the AUX reads leaves the DPCD unread, so a set-up gives the output no mode, logging the last
// refusal and no line of the output's mode
TEST(nvidia_display_engine, set_up_gives_no_mode_to_an_output_whose_dpcd_is_not_read) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_DP_AUX,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common refused display id 0x100's 16-byte AUX read at 0x00000, so no more of its DPCD is read"
    ));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0's mode"));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0 has no mode"));
}

// A DPCD below revision 1.0 with one lane at RBR gives its output no mode for its revision, checked before its link,
// and a set-up logs that reason and that no output is chosen to light, asking the bandwidth check nothing
TEST(nvidia_display_engine, set_up_gives_no_mode_to_an_output_whose_dpcd_is_below_revision_1_0) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.dpcd_receiver = DPCD_RECEIVER_BELOW_1_0;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: output 0 has no mode, as its monitor's DPCD gives a revision below 1.0"));
    EXPECT_FALSE(engine_log_contains("its link carries none"));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0's mode"));
    EXPECT_TRUE(engine_log_contains("nvidia: no output is chosen to light, as no connected output has a mode"));
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 0u);
}

// Links without FEC give no output a mode, which a set-up logs once for all outputs rather than for each
TEST(nvidia_display_engine, set_up_gives_no_output_a_mode_when_the_links_lack_fec) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine, CHIP_LINKS_WITHOUT_FEC), OK);
    EXPECT_EQ(engine_log_count("nvidia: no output has a mode, as the GPU's links lack FEC support"), 1u);
    EXPECT_FALSE(engine_log_contains("'s mode is"));
    EXPECT_FALSE(engine_log_contains("has no mode, as"));
}

// An EDID giving no timing gives its output no mode, which a set-up logs with that reason
TEST(nvidia_display_engine, set_up_gives_no_mode_to_an_output_whose_edid_gives_no_timing) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    copy_aoc_edid_without_timings();
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: output 0 has no mode, as its EDID gives no timing"));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0's mode"));
}

// A DPCD below revision 1.0 is the reason a set-up logs for an output whose EDID also gives no timing
TEST(nvidia_display_engine, set_up_logs_the_dpcd_revision_before_an_edid_giving_no_timing) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.dpcd_receiver = DPCD_RECEIVER_BELOW_1_0;

    copy_aoc_edid_without_timings();
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: output 0 has no mode, as its monitor's DPCD gives a revision below 1.0"));
    EXPECT_FALSE(engine_log_contains("as its EDID gives no timing"));
}

// One lane at RBR carries neither of the AOC's timings, so a set-up gives the output no mode and logs that reason
TEST(nvidia_display_engine, set_up_gives_no_mode_to_an_output_whose_link_carries_no_timing) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.dpcd_receiver = DPCD_RECEIVER_ONE_RBR_LANE;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 has no mode, as its EDID gives 2 timings and its link carries none\r\n"
    ));
}

// An output limited to 60 Hz whose link carries neither timing has no mode, which a set-up logs with the limit
TEST(nvidia_display_engine, set_up_gives_no_mode_to_a_limited_output_whose_link_carries_no_timing) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.digital_facts = DISPLAYPORT_FACTS | rm::DIGITAL_LIMITED_TO_60_HZ;
    fw.dpcd_receiver = DPCD_RECEIVER_ONE_RBR_LANE;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 has no mode, as its EDID gives 2 timings and its link carries none below 60.600 Hz\r\n"
    ));
}

// With no timing preferred the AOC's 143.911 Hz timing ranks first, so an output limited to 60 Hz gets the 59.950 Hz
// timing, its mode line counting only the 2 of its EDID's 3 timings below the limit
TEST(nvidia_display_engine, set_up_gives_a_limited_output_a_mode_below_60_600_hz) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.digital_facts = DISPLAYPORT_FACTS | rm::DIGITAL_LIMITED_TO_60_HZ;

    copy_aoc_edid_with_a_third_timing();
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz, over 1 lane at DisplayPort rate "
        "0x1E, ranked first of the 2 timings its link carries, counting only those below 60.600 Hz"
    ));
}

// The default digital facts but for lane code 1, so a link on the connector takes one lane
static constexpr uint32_t DISPLAYPORT_FACTS_ONE_LANE = (rm::DIGITAL_SIGNAL_DISPLAYPORT << rm::DIGITAL_SIGNAL_SHIFT) |
                                                       (1u << rm::DIGITAL_LANES_SHIFT) |
                                                       (4u << rm::DIGITAL_LINK_RATE_SHIFT);

// The receiver capability field of a monitor taking one lane at HBR3, enough for the AOC's 59.950 Hz timing
static constexpr uint8_t DPCD_RECEIVER_ONE_HBR3_LANE[DPCD_RECEIVER_CAPABILITY_BYTES] = {0x14, 0x1E, 0x81};

// One output, one lane at each end of its link and an EDID giving one timing and counting one extension block its 128
// bytes do not hold make a set-up log each one in the singular, the mode line's counts of lanes and timings included
TEST(nvidia_display_engine, set_up_logs_counts_of_1_in_the_singular) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.digital_facts = DISPLAYPORT_FACTS_ONE_LANE;
    fw.dpcd_receiver = DPCD_RECEIVER_ONE_HBR3_LANE;

    copy_aoc_edid_with_one_timing();
    g_edid[EXTENSION_COUNT_BYTE] = 1;
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] - 1);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: the static digital display ids give 1 DisplayPort output\r\n"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: a link on output 0's connector takes up to 1 lane at DisplayPort rate 0x1E\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's link to its monitor takes up to 1 lane at DisplayPort rate 0x1E with enhanced framing\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's monitor is AOC \"AG241QG4\", its EDID giving 1 timing, the first preferred, with 0 more "
        "that did not fit and 1 extension block skipped\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz, over 1 lane at DisplayPort rate "
        "0x1E, ranked first of the 1 timing its link carries\r\n"
    ));
}

// Two outputs, four lanes at each end of a link and an EDID giving two timings and counting two extension blocks its
// 128 bytes do not hold make a set-up log each of those counts in the plural
TEST(nvidia_display_engine, set_up_logs_counts_past_1_in_the_plural) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID;

    copy_aoc_edid(true);
    g_edid[EXTENSION_COUNT_BYTE] = 2;
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] - 2);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: the static digital display ids give 2 DisplayPort outputs\r\n"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: a link on output 0's connector takes up to 4 lanes at DisplayPort rate 0x1E\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's link to its monitor takes up to 4 lanes at DisplayPort rate 0x1E with enhanced framing\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's monitor is AOC \"AG241QG4\", its EDID giving 2 timings, the first preferred, with 0 more "
        "that did not fit and 2 extension blocks skipped\r\n"
    ));
}

// An EDID giving one timing and counting two extension blocks its 128 bytes do not hold makes a set-up log "1 timing"
// and "2 extension blocks", each word chosen by its own count
TEST(nvidia_display_engine, set_up_chooses_each_counted_word_of_the_monitor_line_by_its_own_count) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;

    copy_aoc_edid_with_one_timing();
    g_edid[EXTENSION_COUNT_BYTE] = 2;
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] - 2);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's monitor is AOC \"AG241QG4\", its EDID giving 1 timing, the first preferred, with 0 more "
        "that did not fit and 2 extension blocks skipped\r\n"
    ));
}

// An output limited to 60 Hz keeps the AOC's 59.950 Hz timing alone of its 2, so its mode line counts it in the
// singular
TEST(nvidia_display_engine, set_up_counts_the_lone_timing_a_limited_output_keeps_in_the_singular) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.digital_facts = DISPLAYPORT_FACTS | rm::DIGITAL_LIMITED_TO_60_HZ;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz, over 1 lane at DisplayPort rate "
        "0x1E, ranked first of the 1 timing its link carries, counting only those below 60.600 Hz\r\n"
    ));
}

// One lane at RBR cannot carry the AOC's 59.950 Hz timing, so when the EDID gives that timing alone, the line saying
// why the output has no mode counts it in the singular
TEST(nvidia_display_engine, set_up_counts_the_lone_timing_a_link_cannot_carry_in_the_singular) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.dpcd_receiver = DPCD_RECEIVER_ONE_RBR_LANE;

    copy_aoc_edid_with_one_timing();
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 has no mode, as its EDID gives 1 timing and its link carries none\r\n"
    ));
}

// One lane at RBR cannot carry the AOC's 59.950 Hz timing, so when the EDID gives that timing alone, the line saying
// why an output limited to 60 Hz has no mode counts it in the singular
TEST(nvidia_display_engine, set_up_counts_the_lone_timing_of_a_limited_output_without_a_mode_in_the_singular) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.digital_facts = DISPLAYPORT_FACTS | rm::DIGITAL_LIMITED_TO_60_HZ;
    fw.dpcd_receiver = DPCD_RECEIVER_ONE_RBR_LANE;

    copy_aoc_edid_with_one_timing();
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 has no mode, as its EDID gives 1 timing and its link carries none below 60.600 Hz\r\n"
    ));
}

// With no timing preferred, the AOC's EDID timing 1 ranks first, so a set-up chooses it and names it by that index
TEST(nvidia_display_engine, set_up_names_the_chosen_mode_by_its_edid_index) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    copy_aoc_edid(false);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 is chosen to light first, at its EDID's timing 1, 2560 by 1440 at 143.911 Hz, on head 0 with "
        "window 0, which the bandwidth check accepts"
    ));
}

// The bandwidth check refusing the AOC's EDID timing 1, ranked first, leaves the choice to EDID timing 0, ranked
// second, which a set-up names by that index
TEST(nvidia_display_engine, set_up_names_the_next_ranked_mode_by_its_edid_index_once_the_first_is_refused) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refused_modes = 1;

    copy_aoc_edid(false);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 2u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 is chosen to light first, at its EDID's timing 0, 2560 by 1440 at 59.950 Hz, on head 0 with "
        "window 0, which the bandwidth check accepts"
    ));
}

// Output 0 is not connected, so a set-up keeps modes for output 1 alone and chooses it, tried after output 0
TEST(nvidia_display_engine, set_up_chooses_the_only_output_with_modes_past_output_0) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_1_ID;
    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_EDID), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1 is chosen to light first, at its EDID's timing 0, 2560 by 1440 at 59.950 Hz, on head 0 with "
        "window 0, which the bandwidth check accepts"
    ));
}

// With both outputs given modes and head 0 showing output 1, a set-up chooses output 1 alone, the boot output being
// tried before output 0, and asks the bandwidth check once
TEST(nvidia_display_engine, set_up_chooses_the_boot_output_alone_of_two_with_modes) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.boot_display_id = OUTPUT_1_ID;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 1u);
    EXPECT_EQ(engine_log_count("is chosen to light first"), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1 is chosen to light first, at its EDID's timing 0, 2560 by 1440 at 59.950 Hz, on head 0 with "
        "window 0, which the bandwidth check accepts"
    ));
}

// A set-up keeping output 0's modes and then failing at the push buffer leaves them to the release, which releases the
// objects as when no output has modes
TEST(nvidia_display_engine, set_up_failing_at_the_push_buffer_with_modes_kept_leaves_them_to_release) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine, CHIP_LINKS, REFUSED_ADDRESS_BITS), ERR_NO_MEMORY);
    EXPECT_TRUE(engine_log_contains("nvidia: output 0's mode is its EDID's timing 0, 2560 by 1440 at 59.950 Hz"));
    EXPECT_FALSE(engine_log_contains("to light"));
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 3);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// The bandwidth check refusing both of the AOC's modes leaves no output chosen, logged, and the set-up `OK`
TEST(nvidia_display_engine, set_up_logs_every_mode_refused) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refused_modes = 2;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 2u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: no output is chosen to light, as every mode of the 1 output with modes is refused"
    ));
    EXPECT_FALSE(engine_log_contains("is chosen to light first"));
}

// The bandwidth check refusing the 4 modes of two outputs on the AOC leaves no output chosen, logged with the count of
// outputs with modes
TEST(nvidia_display_engine, set_up_logs_every_mode_of_two_outputs_refused) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.refused_modes = 4;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 4u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: no output is chosen to light, as every mode of the 2 outputs with modes is refused"
    ));
}

// A bandwidth check request failing with `ERR_NO_MEMORY` ends the choice, logged, and leaves the set-up `OK`
TEST(nvidia_display_engine, set_up_lights_nothing_when_the_bandwidth_check_request_cannot_be_allocated) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_IS_MODE_POSSIBLE,
        .object = 0,
        .result = ERR_NO_MEMORY,
    };

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: no output is chosen to light, as the bandwidth check's request cannot be allocated"
    ));
}

// A bandwidth check request timing out fails the set-up with `ERR_TIMEOUT`, and the core channel and the objects stay
// allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_the_bandwidth_check_keeps_the_core_channel_until_release) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_IS_MODE_POSSIBLE,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up(engine), ERR_TIMEOUT);
    EXPECT_EQ(fw.control_count(rm::CONTROL_IS_MODE_POSSIBLE), 1u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// The window a set-up chooses when head 1 shows output 0 with each head having three windows, neither twice the head's
// number nor one above it
static constexpr uint8_t HEAD_1_WINDOW = 3;

// Words put in that window's PUT and GET before a set-up, which one writing nothing into its channel leaves as they are
static constexpr uint32_t WINDOW_PUT_WORD = 0x40;
static constexpr uint32_t WINDOW_GET_WORD = 0x80;

// Has the double report output 0, on the AOC, as the one connected output and the one head 1 shows, each head having
// three windows, so a set-up chooses it on head 1 with `HEAD_1_WINDOW`
static void show_output_0_on_head_1(engine_firmware& fw) {
    fw.supported_ids = OUTPUT_0_ID;
    fw.connected_ids = OUTPUT_0_ID;
    fw.windows_per_head = 3;
    fw.boot_head = 1;
    fw.boot_display_id = OUTPUT_0_ID;
    copy_aoc_edid(true);
}

// The word at `offset` in `window`'s channel's area of `g_channel_areas`
static uint32_t& window_channel_word(uint8_t window, uint32_t offset) {
    return g_channel_areas[(window_channel_area(window) + offset - CORE_CHANNEL_AREA) / sizeof(uint32_t)];
}

// Whether the push buffer at physical address `address` holds only zeros, read through the kernel's mapping of all
// physical memory
static bool is_push_buffer_blank(uint64_t address) {
    const uint8_t* bytes = static_cast<const uint8_t*>(paging::phys_to_virt(address));
    for (uint32_t i = 0; i < PUSH_BUFFER_BYTES; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }

    return true;
}

// A set-up choosing output 0 on head 1 gives the firmware the push buffer of window 3, that head's lowest window, and
// allocates its channel, logging its PUT and GET as read, and writes nothing into the channel or its push buffer
TEST(nvidia_display_engine, set_up_allocates_the_chosen_windows_channel_and_writes_nothing_into_it) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    window_channel_word(HEAD_1_WINDOW, CHANNEL_PUT) = WINDOW_PUT_WORD;
    window_channel_word(HEAD_1_WINDOW, CHANNEL_GET) = WINDOW_GET_WORD;

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 is chosen to light first, at its EDID's timing 0, 2560 by 1440 at 59.950 Hz, on head 1 with "
        "window 3, which the bandwidth check accepts"
    ));
    EXPECT_NE(fw.allocated(TEST_WINDOW_CHANNEL_CLASS), 0u);
    EXPECT_EQ(fw.push_buffer_class, TEST_WINDOW_CHANNEL_CLASS);
    EXPECT_EQ(fw.push_buffer_instance, static_cast<uint32_t>(HEAD_1_WINDOW));

    EXPECT_TRUE(engine_log_contains("nvidia: the GSP's firmware keeps window 3's channel 0x"));
    EXPECT_TRUE(engine_log_contains("its PUT and GET reading 0x40 and 0x80, offsets into the push buffer"));
    EXPECT_EQ(window_channel_word(HEAD_1_WINDOW, CHANNEL_PUT), WINDOW_PUT_WORD);
    EXPECT_EQ(window_channel_word(HEAD_1_WINDOW, CHANNEL_GET), WINDOW_GET_WORD);
    EXPECT_TRUE(is_push_buffer_blank(fw.push_buffer_address));
}

// A method and value written through the chosen window's queue, any method of its class and any value but 0
static constexpr uint32_t WINDOW_QUEUE_METHOD = 0x308;
static constexpr uint32_t WINDOW_QUEUE_VALUE = 0x1;

// The chosen window's channel takes its queue of methods once allocated, writing into the window's push buffer and
// kicking that channel's PUT alone, and the release drops it
TEST(nvidia_display_engine, set_up_gives_the_windows_channel_a_queue_the_release_drops) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    EXPECT_EQ(display_engine_test_access::window_queue(engine), nullptr);

    ASSERT_EQ(set_up_logged(engine), OK);
    display_channel* queue = display_engine_test_access::window_queue(engine);
    ASSERT_NE(queue, nullptr);

    uint32_t core_put = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
    ASSERT_EQ(queue->write(WINDOW_QUEUE_METHOD, &WINDOW_QUEUE_VALUE, 1), OK);
    queue->kick();

    const uint32_t* words = static_cast<const uint32_t*>(paging::phys_to_virt(fw.push_buffer_address));
    EXPECT_EQ(words[1], WINDOW_QUEUE_VALUE);
    EXPECT_EQ(window_channel_word(HEAD_1_WINDOW, CHANNEL_PUT), 2 * sizeof(uint32_t));
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], core_put);

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(display_engine_test_access::window_queue(engine), nullptr);
}

// The present windows lacking the chosen window leave it with no channel or queue, logged, the set-up `OK`, neither its
// push buffer given nor its channel allocated, and no base buffer asked for
TEST(nvidia_display_engine, set_up_gives_no_channel_to_a_window_not_reported_present) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.present_windows &= ~(1u << HEAD_1_WINDOW);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 3 has no channel, as the GSP's firmware does not report the window present"
    ));
    EXPECT_EQ(fw.push_buffer_class, TEST_CORE_CHANNEL_CLASS);
    EXPECT_EQ(fw.allocated(TEST_WINDOW_CHANNEL_CLASS), 0u);
    EXPECT_EQ(display_engine_test_access::window_queue(engine), nullptr);
    EXPECT_EQ(memory.scanout_requests, 0u);
}

// A chip whose heads report no output scaler leaves the chosen window with no channel, logged ahead of the window not
// being present, the set-up `OK` and neither its push buffer given nor its channel allocated
TEST(nvidia_display_engine, set_up_gives_no_window_channel_when_the_heads_report_no_output_scaler) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.present_windows &= ~(1u << HEAD_1_WINDOW);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine, CHIP_LINKS, FULL_ADDRESS_BITS, HEADS_WITHOUT_OUTPUT_SCALERS), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 3 has no channel, as the chip's heads report no output scaler, and the driver loads no "
        "window's scaler coefficients"
    ));
    EXPECT_FALSE(engine_log_contains("does not report the window present"));
    EXPECT_EQ(fw.push_buffer_class, TEST_CORE_CHANNEL_CLASS);
    EXPECT_EQ(fw.allocated(TEST_WINDOW_CHANNEL_CLASS), 0u);
}

// The firmware refusing the chosen window's push buffer leaves the window with no channel, logged, the set-up `OK` and
// no channel asked for, so the release frees the core channel and the objects alone
TEST(nvidia_display_engine, set_up_gives_no_channel_to_a_window_whose_push_buffer_is_refused) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.refused_push_buffer_class = TEST_WINDOW_CHANNEL_CLASS;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: window 3 has no channel, as the GSP's firmware refused a request for it"));
    EXPECT_EQ(fw.allocated(TEST_WINDOW_CHANNEL_CLASS), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 5);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
}

// A window channel allocation the firmware refuses, or one whose request cannot be allocated, leaves the window with no
// channel, logged, and the set-up `OK`, and the release frees the core channel and the objects alone
TEST(nvidia_display_engine, set_up_gives_no_channel_to_a_window_whose_allocation_request_is_refused_or_unallocated) {
    struct failure {
        int32_t     error;
        const char* line;
    };

    const failure failures[] = {
        {ERR_FIRMWARE, "nvidia: window 3 has no channel, as the GSP's firmware refused a request for it"},
        {ERR_NO_MEMORY, "nvidia: window 3 has no channel, as a request for it cannot be allocated"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        fw.refusals[0] = {
            .made   = engine_firmware::kind::allocation,
            .number = TEST_WINDOW_CHANNEL_CLASS,
            .object = 0,
            .result = failed.error,
        };

        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);

        ASSERT_EQ(set_up_logged(engine), OK);
        EXPECT_TRUE(engine_log_contains(failed.line));
        uint32_t first = fw.call_count;

        EXPECT_EQ(engine.release(), OK);
        ASSERT_EQ(fw.call_count, first + 5);
        EXPECT_FALSE(fw.releases_once(first, fw.allocated(TEST_WINDOW_CHANNEL_CLASS)));
        EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    }
}

// A window channel allocation that may leave the firmware holding the channel fails the set-up with its error, no base
// buffer asked for, and the release sends what `release_display_channel` sends for the window channel, then the core's
TEST(nvidia_display_engine, set_up_failing_at_the_window_channel_leaves_it_to_release_before_the_core_channel) {
    const int32_t errors[] = {ERR_TIMEOUT, ERR_BAD_MESSAGE};
    for (int32_t error : errors) {
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        fw.refusals[0] = {
            .made   = engine_firmware::kind::allocation,
            .number = TEST_WINDOW_CHANNEL_CLASS,
            .object = 0,
            .result = error,
        };

        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up(engine), error);
        EXPECT_EQ(fw.release_count(), 0u);
        EXPECT_EQ(memory.scanout_requests, 0u);

        rm::handle display = fw.allocated(TEST_DISPLAY_CLASS);
        rm::handle window_channel = fw.allocated(TEST_WINDOW_CHANNEL_CLASS);
        rm::handle core_channel = fw.allocated(TEST_CORE_CHANNEL_CLASS);
        engine_firmware channel_releases;

        EXPECT_EQ(release_display_channel(channel_releases, display, window_channel, true), OK);
        EXPECT_EQ(release_display_channel(channel_releases, display, core_channel, true), OK);
        uint32_t first = fw.call_count;

        EXPECT_EQ(engine.release(), OK);
        ASSERT_EQ(fw.call_count, first + 7);
        EXPECT_TRUE(fw.begins_with(first, channel_releases));
    }
}

// A release whose window channel release times out and whose core channel release is refused still sends every
// request, and returns the window channel's error ahead of the core channel's
TEST(nvidia_display_engine, release_returns_the_window_channels_error_ahead_of_the_core_channels) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up(engine), OK);
    fw.refusals[0] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = fw.allocated(TEST_WINDOW_CHANNEL_CLASS),
        .result = ERR_TIMEOUT,
    };

    fw.refusals[1] = {
        .made   = engine_firmware::kind::release,
        .number = 0,
        .object = fw.allocated(TEST_CORE_CHANNEL_CLASS),
        .result = ERR_FIRMWARE,
    };

    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), ERR_TIMEOUT);
    ASSERT_EQ(fw.call_count, first + 7);
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_WINDOW_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
}

// Either AOC timing's base buffer, 1440 rows of 2560 4-byte pixels, 10240 bytes already a multiple of 256, and NVIDIA's
// alignment of the surfaces its modesetting module scans out and of its look-up table surfaces
static constexpr uint64_t AOC_BASE_BUFFER_BYTES    = 0xE10000;
static constexpr uint64_t NVIDIA_SURFACE_ALIGNMENT = 0x1000;

// An 18-byte timing descriptor of 1366 by 768, an 85.5 MHz pixel clock over 1792 by 798 pixels, with the AOC's image
// size and sync flags
static constexpr uint8_t TIMING_1366_BY_768[] = {
    0x66, 0x21, 0x56, 0xAA, 0x51, 0x00, 0x1E, 0x30, 0x46, 0x8F, 0x33, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E,
};

// `TIMING_1366_BY_768`'s base buffer, 768 rows of 1366 4-byte pixels, 5464 bytes rounded up to 5632
static constexpr uint64_t BASE_BUFFER_1366_BY_768_BYTES = 0x420000;

// `TIMING_1366_BY_768` with 767 rows, which at 5632 bytes apart end inside a page
static constexpr uint8_t TIMING_1366_BY_767[] = {
    0x66, 0x21, 0x56, 0xAA, 0x51, 0xFF, 0x1E, 0x20, 0x46, 0x8F, 0x33, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E,
};

// Replaces the first timing in `g_edid`, the preferred one (the AOC's or one put there before), with the timing
// descriptor `timing`, which stays preferred
static void put_timing_first(const uint8_t (&timing)[DESCRIPTOR_BYTES]) {
    uint8_t replaced_sum = 0;
    for (uint32_t i = 0; i < DESCRIPTOR_BYTES; i++) {
        replaced_sum = static_cast<uint8_t>(replaced_sum + g_edid[TIMING_DESCRIPTORS_BYTE + i] - timing[i]);
    }

    string::memcpy(g_edid + TIMING_DESCRIPTORS_BYTE, timing, DESCRIPTOR_BYTES);
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] + replaced_sum);
}

// Whether each of the `bytes` bytes of the scanout backing from `start` holds `value`
static bool scanout_holds(const engine_video_memory& memory, uint64_t start, uint64_t bytes, uint8_t value) {
    for (uint64_t i = start; i < start + bytes; i++) {
        if (memory.scanout[i] != value) {
            return false;
        }
    }

    return true;
}

// Output 0's 1366 by 768 timing gets 768 rows of 5632 bytes, rounded up from 5464, as scanout memory at NVIDIA's
// alignment, cleared through a mapping taken down after, the bytes past it untouched, logged, and kept by the release
TEST(nvidia_display_engine, set_up_allocates_and_clears_the_chosen_outputs_base_buffer) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_timing_first(TIMING_1366_BY_768);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.scanout_requests, 1u);
    EXPECT_EQ(memory.scanout_size, BASE_BUFFER_1366_BY_768_BYTES);
    EXPECT_EQ(memory.scanout_alignment, NVIDIA_SURFACE_ALIGNMENT);

    EXPECT_EQ(memory.scanout_maps, 2u);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(memory.scanout_fill_words, 0u);
    EXPECT_TRUE(scanout_holds(memory, 0, BASE_BUFFER_1366_BY_768_BYTES, 0));
    EXPECT_TRUE(scanout_holds(memory, BASE_BUFFER_1366_BY_768_BYTES, SCANOUT_GUARD_BYTES, SCANOUT_FILL));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's base buffer lies at 0x1000000 in video memory, its 768 rows of 5632 bytes, and is cleared "
        "to black\r\n"
    ));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(memory.scanout_releases, 0u);
}

// `TIMING_1366_BY_768` cut to one row, 1366 by 1 over 1792 by 31 pixels, its 30 lines of blanking kept
static constexpr uint8_t TIMING_1366_BY_1[] = {
    0x66, 0x21, 0x56, 0xAA, 0x51, 0x01, 0x1E, 0x00, 0x46, 0x8F, 0x33, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E,
};

// `TIMING_1366_BY_1`'s base buffer, one row of 1366 4-byte pixels, 5464 bytes rounded up to 5632
static constexpr uint64_t BASE_BUFFER_1366_BY_1_BYTES = 0x1600;

// The two 4 KiB pages video memory hands out for `BASE_BUFFER_1366_BY_1_BYTES`, as `allocate` rounds a request up
static constexpr uint64_t BASE_BUFFER_1366_BY_1_PAGES_BYTES = 0x2000;

// Output 0's timing one row high gets a base buffer of one row, which the set-up logs in the singular
TEST(nvidia_display_engine, set_up_counts_the_lone_row_of_a_base_buffer_in_the_singular) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_timing_first(TIMING_1366_BY_1);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_1_PAGES_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.scanout_size, BASE_BUFFER_1366_BY_1_BYTES);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's base buffer lies at 0x1000000 in video memory, its 1 row of 5632 bytes, and is cleared to "
        "black\r\n"
    ));
}

// Scanout memory the video memory has no room for leaves output 0 with no base buffer, logged, the set-up `OK` and
// nothing mapped
TEST(nvidia_display_engine, set_up_gives_no_base_buffer_to_an_output_whose_memory_cannot_be_allocated) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 has no base buffer, as 0xE10000 bytes of video memory for it cannot be allocated"
    ));
    EXPECT_EQ(memory.scanout_requests, 1u);
    EXPECT_EQ(memory.scanout_maps, 0u);
}

// A scanout allocation failing other than for room fails the set-up with its error, logged, with nothing mapped
TEST(nvidia_display_engine, set_up_fails_with_the_error_of_a_base_buffer_allocation_failing_otherwise) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    memory.scanout_allocate_result = ERR_INVALID_ARG;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), ERR_INVALID_ARG);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: allocating 0xE10000 bytes of video memory for output 0's base buffer failed: -13"
    ));
    EXPECT_EQ(memory.scanout_maps, 0u);
}

// A mapping of the base buffer finding no room leaves output 0 with no base buffer and the set-up `OK`, and one failing
// otherwise fails the set-up with its error, each logged, with the memory released and never unmapped
TEST(nvidia_display_engine, set_up_releases_a_base_buffer_it_cannot_map) {
    struct failure {
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    const failure failures[] = {
        {ERR_NO_MEMORY,
         OK,
         "nvidia: output 0 has no base buffer, as mapping it for the CPU to clear it found no room, so its video "
         "memory is released"},
        {ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for the CPU to clear it failed: -4, so "
         "the memory is released"},
        {ERR_BAD_STATE,
         ERR_BAD_STATE,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for the CPU to clear it failed: -5, so "
         "the memory is released"},
        {ERR_INVALID_ARG,
         ERR_INVALID_ARG,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for the CPU to clear it failed: -13, so "
         "the memory is released"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        engine_video_memory memory;
        memory.scanout_bytes = AOC_BASE_BUFFER_BYTES;
        memory.scanout_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_EQ(memory.scanout_maps, 1u);
        EXPECT_EQ(memory.scanout_releases, 1u);
        EXPECT_EQ(memory.scanout_unmaps, 0u);
    }
}

// The numbers of the core channel and of window 0's and window 3's channels, which a set-up chooses for output 1 alone
// and for output 0 on head 1: NVIDIA's NV_PDISP_CHN_NUM_CORE, NV_PDISP_CHN_NUM_WIN(0) and NV_PDISP_CHN_NUM_WIN(3)
static constexpr uint32_t CORE_CHANNEL            = 0;
static constexpr uint32_t OUTPUT_1_WINDOW_CHANNEL = 1;
static constexpr uint32_t HEAD_1_WINDOW_CHANNEL   = 4;

// A set-up's general mappings by number for a chosen output, after the instance memory's clear and the notifier's
// binding, the core channel's table binding undone after a failed one to the window's, and its table's general request
static constexpr uint32_t BASE_BUFFER_BINDING_MAP  = 3;
static constexpr uint32_t TABLE_FILL_MAP           = 4;
static constexpr uint32_t TABLE_CORE_BINDING_MAP   = 5;
static constexpr uint32_t TABLE_WINDOW_BINDING_MAP = 6;
static constexpr uint32_t TABLE_CORE_UNBINDING_MAP = 7;
static constexpr uint32_t TABLE_REQUEST            = 2;

// The window notifier's binding, the mapping after the table's window binding when that succeeds
static constexpr uint32_t WINDOW_NOTIFIER_BINDING_MAP = TABLE_WINDOW_BINDING_MAP + 1;

// The records of a binding of `TIMING_1366_BY_768`'s base buffer at `SCANOUT_OFFSET` and of the table at
// `LOOK_UP_TABLE_OFFSET`, each naming its first and last 256-byte unit
static constexpr uint32_t BASE_BUFFER_RECORD[] = {NVIDIA_VIDEO_MEMORY_WORD_0, 0x10000, 0, 0x141FF, 0};
static constexpr uint32_t TABLE_RECORD[]       = {NVIDIA_VIDEO_MEMORY_WORD_0, 0x100, 0, 0x14F, 0};

// Has the double report output 1, on the AOC with `TIMING_1366_BY_768` first, as the one connected output of two, so
// a set-up chooses it on head 0 with window 0
static void connect_output_1_alone(engine_firmware& fw) {
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_1_ID;
    copy_aoc_edid(true);
    put_timing_first(TIMING_1366_BY_768);
}

// Whether a hash table entry naming `object` binds it under the double's client to `hardware_channel` with the record
// `record`, from 0 the lowest, whose words are `words`
static bool binds(rm::handle object, uint32_t hardware_channel, uint32_t record, const uint32_t (&words)[5]) {
    uint32_t client_bits = TEST_CLIENT & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = (NVIDIA_FIRST_RECORD_FIELD + record) << NVIDIA_RECORD_FIELD_SHIFT;
    uint32_t channel_bits = hardware_channel << NVIDIA_CHANNEL_SHIFT;
    bool is_bound = false;
    for (uint32_t entry = 0; entry < NVIDIA_HASH_ENTRIES; entry++) {
        bool is_object = instance_word(entry * NVIDIA_ENTRY_BYTES) == object;
        uint32_t context = instance_word(entry * NVIDIA_ENTRY_BYTES + sizeof(uint32_t));
        bool is_binding = context == (client_bits | record_bits | channel_bits);
        if (is_object && is_binding) {
            is_bound = true;
        }
    }

    if (!is_bound) {
        return false;
    }

    uint64_t record_offset = NVIDIA_FIRST_RECORD_OFFSET + record * NVIDIA_RECORD_BYTES;
    for (uint32_t index = 0; index < 5; index++) {
        if (instance_word(record_offset + index * sizeof(uint32_t)) != words[index]) {
            return false;
        }
    }

    return true;
}

// Output 1's base buffer and then the look-up table surface are bound to window 0's channel, number 1, each under a new
// handle the engine keeps and the next record, logged, the table to the core channel as well, and the release keeps all
TEST(nvidia_display_engine, set_up_binds_both_surfaces_to_the_windows_channel_and_the_table_to_the_core_channel_too) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle base_buffer_context_dma = FIRST_HANDLE + fw.handle_count - 3;
    rm::handle table_context_dma = FIRST_HANDLE + fw.handle_count - 2;

    EXPECT_EQ(used_entry_count(), 5u);
    EXPECT_TRUE(binds(base_buffer_context_dma, OUTPUT_1_WINDOW_CHANNEL, 1, BASE_BUFFER_RECORD));
    EXPECT_TRUE(binds(table_context_dma, CORE_CHANNEL, 2, TABLE_RECORD));
    EXPECT_TRUE(binds(table_context_dma, OUTPUT_1_WINDOW_CHANNEL, 2, TABLE_RECORD));

    EXPECT_EQ(display_engine_test_access::base_buffer_context_dma(engine), base_buffer_context_dma);
    EXPECT_EQ(display_engine_test_access::look_up_table_context_dma(engine), table_context_dma);

    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1's base buffer is bound to window 0's channel as context DMA 0x"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface is bound to the core channel and window 0's channel as context DMA 0x"
    ));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(used_entry_count(), 5u);
    EXPECT_EQ(memory.scanout_releases, 0u);
    EXPECT_EQ(memory.general_releases, 0u);
}

// The window notifier's context DMA, the set-up's last new handle, is bound to the lit window's channel in the record
// after the surfaces', a snooped system memory page as the core channel's notifier is, and logged
TEST(nvidia_display_engine, set_up_binds_the_window_notifier_to_the_lit_windows_channel) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up_logged(engine), OK);

    rm::handle notifier_context_dma = FIRST_HANDLE + fw.handle_count - 1;
    EXPECT_EQ(display_engine_test_access::window_notifier_context_dma(engine), notifier_context_dma);
    uint32_t entry = entry_of(notifier_context_dma);
    ASSERT_NE(entry, NVIDIA_HASH_ENTRIES);

    uint32_t client_bits = TEST_CLIENT & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = (NVIDIA_FIRST_RECORD_FIELD + 3) << NVIDIA_RECORD_FIELD_SHIFT;
    uint32_t channel_bits = OUTPUT_1_WINDOW_CHANNEL << NVIDIA_CHANNEL_SHIFT;
    EXPECT_EQ(instance_word(entry * NVIDIA_ENTRY_BYTES + sizeof(uint32_t)), client_bits | record_bits | channel_bits);
    EXPECT_EQ(instance_word(NVIDIA_FIRST_RECORD_OFFSET + 3 * NVIDIA_RECORD_BYTES), NVIDIA_SNOOPED_PAGES_WORD_0);
    EXPECT_TRUE(engine_log_contains("nvidia: window 0's notifier lies at physical address 0x"));

    EXPECT_EQ(engine.release(), OK);
}

// A window notifier binding finding no room lights output 0, the boot head's, on head 1 through window 3 with presents
// unsupported and no notifier, logged, and one failing otherwise fails the set-up with its error
TEST(nvidia_display_engine, set_up_lights_with_presents_unsupported_when_the_window_notifier_finds_no_room) {
    struct failure {
        int32_t error;
        int32_t result;
    };

    const failure failures[] = {
        {ERR_NO_MEMORY, OK},
        {ERR_TIMEOUT, ERR_TIMEOUT},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        put_timing_first(TIMING_1366_BY_768);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
        memory.failing_general_map = WINDOW_NOTIFIER_BINDING_MAP;
        memory.general_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        bool is_shortage = failed.error == ERR_NO_MEMORY;
        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_EQ(engine.has_lit_output(), is_shortage);
        EXPECT_EQ(display_engine_test_access::window_notifier_context_dma(engine), 0u);
        EXPECT_FALSE(engine_log_contains("nvidia: window 3's notifier lies at"));
        EXPECT_EQ(
            engine_log_contains("nvidia: window 3 has no notifier, so presents on its output are unsupported"),
            is_shortage
        );

        if (is_shortage) {
            engine.turn_on_flips();
            gfxfb::buffer* base = engine.base_buffer(0);
            ASSERT_NOT_NULL(base);
            EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_UNSUPPORTED);
            EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::OK);
        }

        EXPECT_EQ(engine.release(), OK);
    }
}

// Output 0 on head 1 has both surfaces and the window notifier bound to its window's channel, window 3's, number 4,
// rather than head 1's or window 0's, the table bound to the core channel as well
TEST(nvidia_display_engine, set_up_binds_head_1s_surfaces_to_window_3s_channel) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_timing_first(TIMING_1366_BY_768);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle base_buffer_context_dma = FIRST_HANDLE + fw.handle_count - 3;
    rm::handle table_context_dma = FIRST_HANDLE + fw.handle_count - 2;
    EXPECT_TRUE(binds(base_buffer_context_dma, HEAD_1_WINDOW_CHANNEL, 1, BASE_BUFFER_RECORD));
    EXPECT_TRUE(binds(table_context_dma, CORE_CHANNEL, 2, TABLE_RECORD));
    EXPECT_TRUE(binds(table_context_dma, HEAD_1_WINDOW_CHANNEL, 2, TABLE_RECORD));

    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's base buffer is bound to window 3's channel as context DMA 0x"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface is bound to the core channel and window 3's channel as context DMA 0x"
    ));

    rm::handle notifier_context_dma = FIRST_HANDLE + fw.handle_count - 1;
    EXPECT_EQ(display_engine_test_access::window_notifier_context_dma(engine), notifier_context_dma);
    uint32_t entry = entry_of(notifier_context_dma);
    ASSERT_NE(entry, NVIDIA_HASH_ENTRIES);

    uint32_t client_bits = TEST_CLIENT & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = (NVIDIA_FIRST_RECORD_FIELD + 3) << NVIDIA_RECORD_FIELD_SHIFT;
    uint32_t channel_bits = HEAD_1_WINDOW_CHANNEL << NVIDIA_CHANNEL_SHIFT;
    EXPECT_EQ(instance_word(entry * NVIDIA_ENTRY_BYTES + sizeof(uint32_t)), client_bits | record_bits | channel_bits);
    EXPECT_EQ(instance_word(NVIDIA_FIRST_RECORD_OFFSET + 3 * NVIDIA_RECORD_BYTES), NVIDIA_SNOOPED_PAGES_WORD_0);
    EXPECT_TRUE(engine_log_contains("nvidia: window 3's notifier lies at physical address 0x"));
}

// The table takes `LOOK_UP_TABLE_SURFACE_BYTES` of general memory at NVIDIA's alignment, its whole pages and nothing
// past them written as `look_up_table_word` gives through one mapping taken down after, and where it lies logged
TEST(nvidia_display_engine, set_up_fills_the_look_up_table_surface_with_nvidias_identity) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.general_requests, TABLE_REQUEST);
    EXPECT_EQ(memory.general_size, LOOK_UP_TABLE_SURFACE_BYTES);
    EXPECT_EQ(memory.general_alignment, NVIDIA_SURFACE_ALIGNMENT);
    EXPECT_EQ(memory.general_maps, WINDOW_NOTIFIER_BINDING_MAP);
    EXPECT_EQ(memory.general_unmaps, WINDOW_NOTIFIER_BINDING_MAP);

    EXPECT_EQ(memory.general_overruns, 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface lies at 0x10000 in video memory, its input and output tables passing each "
        "color through unchanged"
    ));

    uint32_t wrong_words = 0;
    for (uint32_t index = 0; index < LOOK_UP_TABLE_PAGE_BYTES / sizeof(uint64_t); index++) {
        uint64_t word = 0;
        string::memcpy(&word, g_video_memory + LOOK_UP_TABLE_OFFSET + index * sizeof(uint64_t), sizeof(word));
        if (word != look_up_table_word(index)) {
            wrong_words++;
        }
    }

    EXPECT_EQ(wrong_words, 0u);
}

// A base buffer binding finding no room leaves output 1 with no base buffer, the set-up `OK`, and one failing otherwise
// fails it with its error, each logged, not as bound, the buffer released with its context DMA 0 and no table asked for
TEST(nvidia_display_engine, set_up_releases_a_base_buffer_whose_binding_fails) {
    struct failure {
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    const failure failures[] = {
        {ERR_NO_MEMORY,
         OK,
         "nvidia: output 1 has no base buffer, as binding it to window 0's channel found no room, so its video memory "
         "is released"},
        {ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: binding output 1's base buffer to window 0's channel failed: -4, so its video memory is released"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
        memory.failing_general_map = BASE_BUFFER_BINDING_MAP;
        memory.general_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_FALSE(engine_log_contains("nvidia: output 1's base buffer is bound"));
        EXPECT_EQ(display_engine_test_access::base_buffer_context_dma(engine), 0u);

        EXPECT_EQ(used_entry_count(), 1u);
        EXPECT_EQ(memory.scanout_releases, 1u);
        EXPECT_EQ(memory.general_requests, 1u);
    }
}

// A table allocation finding no room leaves the engine with none and the set-up `OK`, and one failing otherwise fails
// the set-up with its error, each logged, not as filled or bound, nothing mapped for it and the base buffer kept bound
TEST(nvidia_display_engine, set_up_gives_no_look_up_table_surface_when_its_allocation_fails) {
    struct failure {
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    const failure failures[] = {
        {ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface, as 0x4200 bytes of video memory for it cannot be "
         "allocated"},
        {ERR_INVALID_ARG,
         ERR_INVALID_ARG,
         "nvidia: allocating 0x4200 bytes of video memory for the look-up table surface failed: -13"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
        memory.failing_general_request = TABLE_REQUEST;
        memory.general_allocate_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_FALSE(engine_log_contains("nvidia: the look-up table surface lies at"));
        EXPECT_FALSE(engine_log_contains("nvidia: the look-up table surface is bound"));

        EXPECT_EQ(memory.general_maps, BASE_BUFFER_BINDING_MAP);
        EXPECT_EQ(used_entry_count(), 2u);
        EXPECT_EQ(memory.scanout_releases, 0u);
    }
}

// A table mapping or binding, to either channel, finding no room leaves no table and the set-up `OK`, and one failing
// otherwise fails it with its error, logged, the table released once unbound and the base buffer kept bound
TEST(nvidia_display_engine, set_up_releases_a_look_up_table_surface_it_cannot_fill_or_bind) {
    struct failure {
        uint32_t    map;
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    static constexpr failure failures[] = {
        {TABLE_FILL_MAP,
         ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface, as mapping it for the CPU to fill it found no room, "
         "so its video memory is released"},
        {TABLE_FILL_MAP,
         ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: mapping the look-up table surface at 0x10000 in video memory for the CPU to fill it failed: -4, so "
         "the memory is released"},
        {TABLE_CORE_BINDING_MAP,
         ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface for window 0, as binding it to the core channel "
         "found no room, so its video memory is released"},
        {TABLE_CORE_BINDING_MAP,
         ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: binding the look-up table surface for window 0 to the core channel failed: -4, so its video memory "
         "is released"},
        {TABLE_WINDOW_BINDING_MAP,
         ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface for window 0, as binding it to the window's channel "
         "found no room, so its video memory is released"},
        {TABLE_WINDOW_BINDING_MAP,
         ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: binding the look-up table surface for window 0 to the window's channel failed: -4, so its video "
         "memory is released"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
        memory.failing_general_map = failed.map;
        memory.general_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_FALSE(engine_log_contains("nvidia: the look-up table surface is bound"));
        if (failed.map == TABLE_FILL_MAP) {
            EXPECT_FALSE(engine_log_contains("nvidia: the look-up table surface lies at"));
        }

        EXPECT_EQ(memory.general_releases, 1u);
        EXPECT_EQ(memory.general_released.offset, LOOK_UP_TABLE_OFFSET);
        EXPECT_EQ(used_entry_count(), 2u);
        EXPECT_EQ(memory.scanout_releases, 0u);

        EXPECT_EQ(display_engine_test_access::look_up_table(engine).size, 0u);
        EXPECT_EQ(display_engine_test_access::look_up_table_context_dma(engine), 0u);
        EXPECT_NE(display_engine_test_access::base_buffer_context_dma(engine), 0u);

        if (failed.map == TABLE_WINDOW_BINDING_MAP) {
            EXPECT_EQ(memory.general_maps, TABLE_CORE_UNBINDING_MAP);
        }
    }
}

// A table whose window binding fails and whose core binding cannot then be undone stays bound to the core channel,
// logged and forgotten, the set-up failing with the first of the two errors other than no room, else `OK`
TEST(nvidia_display_engine, set_up_keeps_a_look_up_table_surface_it_cannot_unbind_from_the_core_channel) {
    struct failure {
        int32_t     binding_error;
        int32_t     undoing_error;
        int32_t     result;
        const char* line;
    };

    static constexpr failure failures[] = {
        {ERR_TIMEOUT,
         ERR_BAD_STATE,
         ERR_TIMEOUT,
         "nvidia: binding the look-up table surface for window 0 to the window's channel failed: -4, so its video "
         "memory stays allocated, still bound to the core channel"},
        {ERR_TIMEOUT,
         ERR_NO_MEMORY,
         ERR_TIMEOUT,
         "nvidia: binding the look-up table surface for window 0 to the window's channel failed: -4, so its video "
         "memory stays allocated, still bound to the core channel"},
        {ERR_NO_MEMORY,
         ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: the display engine has no look-up table surface for window 0, as binding it to the window's channel "
         "found no room, so its video memory stays allocated, still bound to the core channel"},
        {ERR_NO_MEMORY,
         ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface for window 0, as binding it to the window's channel "
         "found no room, so its video memory stays allocated, still bound to the core channel"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
        memory.failing_general_map = TABLE_WINDOW_BINDING_MAP;
        memory.general_map_result = failed.binding_error;
        memory.second_failing_map = TABLE_CORE_UNBINDING_MAP;
        memory.second_map_result = failed.undoing_error;

        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_FALSE(engine_log_contains("nvidia: the look-up table surface is bound"));

        rm::handle table_context_dma = FIRST_HANDLE + fw.handle_count - 1;
        EXPECT_TRUE(binds(table_context_dma, CORE_CHANNEL, 2, TABLE_RECORD));
        EXPECT_EQ(used_entry_count(), 3u);
        EXPECT_EQ(memory.general_releases, 0u);

        EXPECT_EQ(display_engine_test_access::look_up_table(engine).size, 0u);
        EXPECT_EQ(display_engine_test_access::look_up_table_context_dma(engine), 0u);
    }
}

// A set-up whose base buffer binding or table allocation, mapping or binding finds no room asks nothing of the core
// channel's state and logs neither end of a wait for it, since the wait follows both surfaces' bindings
TEST(nvidia_display_engine, set_up_without_both_surfaces_bound_asks_nothing_of_the_core_channels_state) {
    struct no_room {
        uint32_t request;
        uint32_t map;
    };

    const no_room cases[] = {
        {0, BASE_BUFFER_BINDING_MAP},
        {TABLE_REQUEST, 0},
        {0, TABLE_FILL_MAP},
        {0, TABLE_CORE_BINDING_MAP},
        {0, TABLE_WINDOW_BINDING_MAP},
    };

    for (const no_room& missing : cases) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        memory.failing_general_request = missing.request;
        memory.general_allocate_result = ERR_NO_MEMORY;
        memory.failing_general_map = missing.map;
        memory.general_map_result = ERR_NO_MEMORY;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));

        display_engine& engine = new_engine(fw, memory);
        ASSERT_EQ(set_up_logged(engine), OK);
        EXPECT_EQ(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE), 0u);
        EXPECT_FALSE(engine_log_contains("the core channel idle"));
    }
}

// Has the double report output 0 as `show_output_0_on_head_1` does, preferring `TIMING_1366_BY_768`, and backs its base
// buffer, so a set-up clears it and goes on, or gives false when the unprivileged heap cannot back it
static bool give_output_0_a_base_buffer(engine_firmware& fw, engine_video_memory& memory) {
    show_output_0_on_head_1(fw);
    put_timing_first(TIMING_1366_BY_768);
    return memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES);
}

// A set-up choosing no output, or leaving the chosen output with no base buffer for want of video memory, asks nothing
// of the core channel's state and logs neither end of a wait for it
TEST(nvidia_display_engine, set_up_without_a_base_buffer_asks_nothing_of_the_core_channels_state) {
    const bool is_output_0_shown[] = {false, true};
    for (bool is_shown : is_output_0_shown) {
        engine_firmware fw;
        if (is_shown) {
            show_output_0_on_head_1(fw);
        }

        engine_video_memory memory;
        display_engine& engine = new_engine(fw, memory);

        ASSERT_EQ(set_up_logged(engine), OK);
        EXPECT_EQ(memory.scanout_requests, is_shown ? 1u : 0u);
        EXPECT_EQ(memory.scanout_maps, 0u);
        EXPECT_EQ(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE), 0u);
        EXPECT_FALSE(engine_log_contains("the core channel idle"));
    }
}

// A request for the core channel's state failing other than with `ERR_FIRMWARE` fails the set-up once both surfaces are
// bound, and both channels and the objects stay allocated until the release frees them
TEST(nvidia_display_engine, set_up_failing_at_the_core_channels_idle_wait_keeps_both_channels_until_release) {
    engine_firmware fw;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_DISPLAY_CHANNEL_STATE,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up(engine), ERR_TIMEOUT);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE), 1u);
    EXPECT_EQ(fw.release_count(), 0u);
    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    ASSERT_EQ(fw.call_count, first + 7);

    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_WINDOW_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_CORE_CHANNEL_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(TEST_DISPLAY_CLASS)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_BANDWIDTH_CHECK)));
    EXPECT_TRUE(fw.releases_once(first, fw.allocated(rm::CLASS_DISPLAY_COMMON)));
}

// A set-up binding output 0's base buffer and the look-up table surface asks the display once for the core channel's
// state, and with the first reply naming the idle state logs that the firmware reported the channel idle
TEST(nvidia_display_engine, set_up_logs_that_the_firmware_reported_the_core_channel_idle) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle display = fw.allocated(TEST_DISPLAY_CLASS);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE, display), 1u);

    EXPECT_TRUE(engine_log_contains("nvidia: the GSP's firmware reports the core channel idle"));
    EXPECT_FALSE(engine_log_contains("did not report the core channel idle"));
}

// With the firmware reporting the core channel busy at every request, a set-up logs an error in place of the idle
// line once the wait gives up, and goes on to succeed
TEST(nvidia_display_engine, set_up_logs_an_error_for_a_core_channel_never_reported_idle) {
    engine_firmware fw;
    fw.is_channel_busy = true;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_GT(fw.control_count(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the GSP's firmware did not report the core channel idle within 100 milliseconds"
    ));
    EXPECT_FALSE(engine_log_contains("reports the core channel idle"));
}

// Sets window `window`'s control in the core channel's armed state to `control`
static void set_armed_control(uint8_t window, uint32_t control) {
    uint32_t offset = NVIDIA_ARMED_WINDOW_CONTROL + window * NVIDIA_WINDOW_STRIDE;
    g_core_channel_registers[offset / sizeof(uint32_t)] = control;
}

// Once the core channel's wait ends, a set-up logs the owner of each present window from its control in the armed
// state: a head of the chip's, no head, or a warning for an owner naming a head the chip lacks
TEST(nvidia_display_engine, set_up_logs_each_present_windows_owner_from_the_armed_state) {
    const uint32_t controls[CHIP_WINDOWS] = {0x0, 0xFFFFFFF3, 0x1, 0x7, 0x4, 0xE, WINDOW_OWNER_NONE, 0x3};
    for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
        set_armed_control(window, controls[window]);
    }

    engine_firmware fw;
    fw.present_windows = 0x7F;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: head 0 owns window 0 in the core channel's armed state"));
    EXPECT_TRUE(engine_log_contains("nvidia: head 3 owns window 1 in the core channel's armed state"));
    EXPECT_TRUE(engine_log_contains("nvidia: head 1 owns window 2 in the core channel's armed state"));

    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 3's owner in the core channel's armed state reads 7, a head the chip lacks"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 4's owner in the core channel's armed state reads 4, a head the chip lacks"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 5's owner in the core channel's armed state reads 14, a head the chip lacks"
    ));
    EXPECT_TRUE(engine_log_contains("nvidia: no head owns window 6 in the core channel's armed state"));
    EXPECT_FALSE(engine_log_contains("window 7"));
}

// With the firmware reporting the core channel busy at every request, a set-up whose wait gives up reads the owners
// after the wait, logging window 0's owner as the state requests left its armed control, not as it was before them
TEST(nvidia_display_engine, set_up_reads_the_owners_after_a_core_channel_never_reported_idle) {
    set_armed_control(0, 0x0);
    set_armed_control(5, 0x2);

    engine_firmware fw;
    fw.is_channel_busy = true;
    fw.armed_control_at_reply = 0x1;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("did not report the core channel idle within 100 milliseconds"));
    EXPECT_TRUE(engine_log_contains("nvidia: head 1 owns window 0 in the core channel's armed state"));
    EXPECT_FALSE(engine_log_contains("head 0 owns window 0"));
    EXPECT_TRUE(engine_log_contains("nvidia: head 2 owns window 5 in the core channel's armed state"));
}

// A set-up whose request for the core channel's state fails returns that error and logs no window's owner
TEST(nvidia_display_engine, set_up_failing_at_the_core_channels_idle_wait_logs_no_window_owner) {
    set_armed_control(0, 0x0);
    engine_firmware fw;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_DISPLAY_CHANNEL_STATE,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), ERR_TIMEOUT);
    EXPECT_FALSE(engine_log_contains("owns window"));
    EXPECT_FALSE(engine_log_contains("a head the chip lacks"));
    EXPECT_EQ(engine.release(), OK);
}

// The light's first update on head 0 with no head active, the 8 windows mapped ahead of head 0's methods, where it
// ends, and where the second core update ends after the notifier's stop: window 0's usage methods and the update's five
static constexpr uint32_t FIRST_LIGHT_METHODS = 8 * 2 + HEAD_LIGHT_FIXED_METHODS + 3 + 5;
static constexpr uint32_t FIRST_LIGHT_END     = INITIAL_METHODS_END + FIRST_LIGHT_METHODS * 2 * sizeof(uint32_t);
static constexpr uint32_t SECOND_UPDATE_BYTES = (WINDOW_USAGE_METHODS + 5) * 2 * sizeof(uint32_t);
static constexpr uint32_t SECOND_UPDATE_END   = FIRST_LIGHT_END + 16 + SECOND_UPDATE_BYTES;

// With no head active, the set-up sends the mode set's start and end naming the chosen output's display id around the
// light's updates, and the release then asks to keep nothing
TEST(nvidia_display_engine, set_up_brackets_the_chosen_outputs_mode_set_with_no_head_active) {
    engine_firmware fw;
    fw.get_at_stream = FIRST_LIGHT_END;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fw.boot_display_id = 0;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    ASSERT_EQ(fw.mode_set_requests, 2u);
    EXPECT_EQ(fw.mode_set_ids[0], OUTPUT_0_ID);
    EXPECT_EQ(fw.mode_set_phases[0], 1u);
    EXPECT_EQ(fw.mode_set_ids[1], OUTPUT_0_ID);
    EXPECT_EQ(fw.mode_set_phases[1], 0u);

    EXPECT_EQ(fw.mode_set_puts[0], INITIAL_METHODS_END);
    EXPECT_EQ(fw.mode_set_puts[1], SECOND_UPDATE_END);

    uint32_t start = fw.first_control(rm::CONTROL_DISPLAY_CHANGE);
    EXPECT_TRUE(start > fw.last_control(rm::CONTROL_GET_DISPLAY_CHANNEL_STATE));
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], SECOND_UPDATE_END);
    EXPECT_TRUE(engine_log_contains("display common took the start of the mode set naming display id 0x100"));
    EXPECT_TRUE(engine_log_contains("display common took the end of the mode set naming display id 0x100"));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 0u);
}

// A refused start of the mode set is logged, and then no end is sent, no method written, no link facts asked for, and
// the release keeps the hardware
TEST(nvidia_display_engine, set_up_writes_nothing_and_sends_no_end_after_a_refused_start) {
    engine_firmware fw;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_DISPLAY_CHANGE,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DISPLAY_CHANGE), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 0u);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
    EXPECT_TRUE(engine_log_contains("display common refused the start of the mode set naming display id 0x100"));
    EXPECT_FALSE(engine_log_contains("display common took the start"));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 2u);
}

// The offset in the core channel's push buffer where the shutdown update's methods of 8 windows and one head end, and
// where a take-down's four methods of that head and its update's five end after them
static constexpr uint32_t SHUTDOWN_UPDATE_END = INITIAL_METHODS_END + (8 * 2 + 4 + 5) * 2 * sizeof(uint32_t);
static constexpr uint32_t TAKE_DOWN_END       = SHUTDOWN_UPDATE_END + (4 + 5) * 2 * sizeof(uint32_t);

// The methods of the light's update on head 1 after the shutdown update, head 1's fixed methods, a usage bound for each
// of its three windows, window 3's usage methods and the update's five, and where they end after the notifier's stop
static constexpr uint32_t LIGHT_METHODS = HEAD_LIGHT_FIXED_METHODS + 3 + WINDOW_USAGE_METHODS + 5;
static constexpr uint32_t LIGHT_END     = SHUTDOWN_UPDATE_END + 16 + LIGHT_METHODS * 2 * sizeof(uint32_t);

// A start of the mode set failing otherwise fails the set-up with its error, writing nothing and sending no end, and
// the release then asks to keep the hardware
TEST(nvidia_display_engine, set_up_fails_with_the_error_of_a_mode_set_start_failing_otherwise) {
    engine_firmware fw;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_DISPLAY_CHANGE,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), ERR_TIMEOUT);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DISPLAY_CHANGE), 1u);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);
    EXPECT_FALSE(engine_log_contains("display common took the start"));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 2u);
}

// A refused end of the mode set is logged and changes nothing, while an end missing its deadline fails the set-up,
// each sent after the light's update and the stop of the notifier's writes, with no take-down or other method after
TEST(nvidia_display_engine, set_up_logs_a_refused_mode_set_end_and_fails_on_a_late_one) {
    const int32_t results[] = {ERR_FIRMWARE, ERR_TIMEOUT};
    for (int32_t result : results) {
        engine_firmware fw;
        fw.mode_set_end_result = result;
        engine_video_memory memory;
        ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
        display_engine& engine = new_engine(fw, memory);

        bool is_refused = result == ERR_FIRMWARE;
        EXPECT_EQ(set_up_logged(engine), is_refused ? OK : ERR_TIMEOUT);
        EXPECT_EQ(fw.mode_set_requests, 2u);
        EXPECT_EQ(fw.mode_set_puts[1], LIGHT_END + 16);
        EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], LIGHT_END + 16);

        EXPECT_EQ(engine_log_contains("nvidia: display common refused the end of the mode set"), is_refused);
        EXPECT_FALSE(engine_log_contains("display common took the end"));
        EXPECT_FALSE(engine_log_contains("taken down"));
        EXPECT_EQ(engine.release(), OK);
    }
}

// NVIDIA's core channel methods and values of a mode set's update, window 0's control and usage bounds, and the shift
// of a method header's count, restated
static constexpr uint32_t NVIDIA_NOTIFIER_CONTEXT_DMA = 0x208;
static constexpr uint32_t NVIDIA_NOTIFIER_CONTROL     = 0x20C;
static constexpr uint32_t NVIDIA_INTERLOCK_FLAGS      = 0x218;
static constexpr uint32_t NVIDIA_WINDOW_INTERLOCKS    = 0x21C;
static constexpr uint32_t NVIDIA_UPDATE               = 0x200;
static constexpr uint32_t NVIDIA_AWAKEN_AND_NOTIFY    = 0x1001;
static constexpr uint32_t NVIDIA_RELEASE_ELV          = 0x1;
static constexpr uint32_t NVIDIA_WINDOW_USAGE_BOUNDS  = 0x1010;
static constexpr uint32_t NVIDIA_DEFAULT_USAGE_BOUNDS = 0x10110000;
static constexpr uint32_t NVIDIA_CURSOR_DISABLED      = 0xCF;
static constexpr uint32_t NVIDIA_WINDOW_CONTROL       = 0x1000;
static constexpr uint32_t NVIDIA_COUNT_SHIFT          = 18;

// Head 0's cursor context DMA, cursor control and display id, the stride of each later head's, and serial link 3's
// control, the link every active head in the tests shows its display through
static constexpr uint32_t HEAD_0_CURSOR_CONTEXT = 0x2088;
static constexpr uint32_t HEAD_0_CURSOR_CONTROL = 0x209C;
static constexpr uint32_t HEAD_0_DISPLAY_ID     = 0x2020;
static constexpr uint32_t NVIDIA_HEAD_STRIDE    = 0x400;
static constexpr uint32_t LINK_3_CONTROL        = 0x360;
static constexpr uint32_t BOOT_LINK             = 3;
static constexpr uint32_t SERIAL_LINK_TYPE      = 2;

// Expects the core channel's push buffer to hold at `offset` a one-value method writing `value` into `method`
static void expect_core_method(const engine_firmware& fw, uint32_t offset, uint32_t method, uint32_t value) {
    const uint32_t* words = static_cast<const uint32_t*>(paging::phys_to_virt(fw.core_push_buffer_address));
    EXPECT_EQ(words[offset / sizeof(uint32_t)], (1u << NVIDIA_COUNT_SHIFT) | method);
    EXPECT_EQ(words[offset / sizeof(uint32_t) + 1], value);
}

// Expects `head`'s four stopping methods from `offset`: its cursor disabled, the link whose control method is
// `link_control` carrying no head, no display id
static void expect_head_stopped(const engine_firmware& fw, uint32_t offset, uint8_t head, uint32_t link_control) {
    uint32_t head_methods = head * NVIDIA_HEAD_STRIDE;

    expect_core_method(fw, offset, HEAD_0_CURSOR_CONTEXT + head_methods, 0);
    expect_core_method(fw, offset + 8, HEAD_0_CURSOR_CONTROL + head_methods, NVIDIA_CURSOR_DISABLED);
    expect_core_method(fw, offset + 16, link_control, 0);
    expect_core_method(fw, offset + 24, HEAD_0_DISPLAY_ID + head_methods, 0);
}

// Has the double show output 0 on head 1 over serial link 3 and give it a base buffer
static bool show_output_0_on_link_3(engine_firmware& fw, engine_video_memory& memory) {
    fw.boot_link_index = BOOT_LINK;
    fw.boot_link_type = SERIAL_LINK_TYPE;
    return give_output_0_a_base_buffer(fw, memory);
}

// The shutdown update maps every present window, stops head 1, names the notifier and interlocks nothing, then stops
// later updates from writing the notifier, after the mode set's start, and the release then asks to keep nothing
TEST(nvidia_display_engine, set_up_stops_the_active_head_with_the_shutdown_update) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    const uint32_t owners[CHIP_WINDOWS] = {0, 0, 0, 1, 1, 1, WINDOW_OWNER_NONE, WINDOW_OWNER_NONE};
    for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
        uint32_t offset = INITIAL_METHODS_END + window * 8;
        expect_core_method(fw, offset, NVIDIA_WINDOW_CONTROL + window * NVIDIA_WINDOW_STRIDE, owners[window]);
    }

    for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
        uint32_t offset = INITIAL_METHODS_END + (CHIP_WINDOWS + window) * 8;
        uint32_t method = NVIDIA_WINDOW_USAGE_BOUNDS + window * NVIDIA_WINDOW_STRIDE;
        expect_core_method(fw, offset, method, NVIDIA_DEFAULT_USAGE_BOUNDS);
    }

    uint32_t update = INITIAL_METHODS_END + CHIP_WINDOWS * 2 * 8;
    expect_head_stopped(fw, update, 1, LINK_3_CONTROL);
    rm::handle notifier = display_engine_test_access::notifier_context_dma(engine);
    EXPECT_NE(notifier, 0u);
    expect_core_method(fw, update + 32, NVIDIA_NOTIFIER_CONTEXT_DMA, notifier);

    expect_core_method(fw, update + 40, NVIDIA_NOTIFIER_CONTROL, NVIDIA_AWAKEN_AND_NOTIFY);
    expect_core_method(fw, update + 48, NVIDIA_INTERLOCK_FLAGS, 0);
    expect_core_method(fw, update + 56, NVIDIA_WINDOW_INTERLOCKS, 0);
    expect_core_method(fw, update + 64, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);

    expect_core_method(fw, SHUTDOWN_UPDATE_END, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, SHUTDOWN_UPDATE_END + 8, NVIDIA_NOTIFIER_CONTROL, 0);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], LIGHT_END + 16);

    ASSERT_EQ(fw.mode_set_requests, 2u);
    EXPECT_EQ(fw.mode_set_puts[0], INITIAL_METHODS_END);
    EXPECT_EQ(fw.mode_set_puts[1], LIGHT_END + 16);

    EXPECT_TRUE(engine_log_contains("the core channel's completion notifier reads 0x00000000, a lost notification"));
    EXPECT_TRUE(engine_log_contains("nvidia: the core channel read the shutdown update for 1 active head,"));
    EXPECT_TRUE(engine_log_contains("after the shutdown update, head 1's display id reads 0x0 and serial link 3's"));
    EXPECT_FALSE(engine_log_contains("taken down"));

    uint32_t first = fw.call_count;

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 0u);
    EXPECT_EQ(fw.call_count, first + 5);
}

// With heads 0 and 1 active, one shutdown update stops both ahead of its single update, and its log counts both heads
// and names each
TEST(nvidia_display_engine, set_up_stops_two_active_heads_with_one_shutdown_update) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_head = 0;
    fw.other_display_id = OUTPUT_1_ID;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    uint32_t head_stops = INITIAL_METHODS_END + CHIP_WINDOWS * 2 * 8;
    expect_head_stopped(fw, head_stops, 0, LINK_3_CONTROL);
    expect_head_stopped(fw, head_stops + 32, 1, LINK_3_CONTROL);
    expect_core_method(fw, head_stops + 96, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], LIGHT_END + 32 + 16);

    EXPECT_TRUE(engine_log_contains("nvidia: the core channel read the shutdown update for 2 active heads"));
    EXPECT_TRUE(engine_log_contains("after the shutdown update, head 0's display id reads 0x0 and serial link 3's"));
    EXPECT_TRUE(engine_log_contains("after the shutdown update, head 1's display id reads 0x0 and serial link 3's"));
    EXPECT_EQ(engine.release(), OK);
}

// A notifier wait ending with GET past PUT takes the engine down: head 1's methods again with no window mapping,
// since the shutdown update was kicked, an update naming no notifier and a wait for GET, then the mode set's end
TEST(nvidia_display_engine, set_up_takes_the_engine_down_after_a_failed_notifier_wait) {
    engine_firmware fw;
    fw.get_at_mode_set_start = TAKE_DOWN_END;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    expect_head_stopped(fw, SHUTDOWN_UPDATE_END, 1, LINK_3_CONTROL);
    expect_core_method(fw, SHUTDOWN_UPDATE_END + 32, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, SHUTDOWN_UPDATE_END + 40, NVIDIA_NOTIFIER_CONTROL, 0);
    expect_core_method(fw, SHUTDOWN_UPDATE_END + 48, NVIDIA_INTERLOCK_FLAGS, 0);
    expect_core_method(fw, SHUTDOWN_UPDATE_END + 56, NVIDIA_WINDOW_INTERLOCKS, 0);

    expect_core_method(fw, SHUTDOWN_UPDATE_END + 64, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], TAKE_DOWN_END);

    EXPECT_TRUE(engine_log_contains("nvidia: the core channel's completion notifier did not read finished"));
    EXPECT_TRUE(engine_log_contains("the shutdown update failed: -4, so bring-up takes the display engine down"));
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core channel"));
    EXPECT_FALSE(engine_log_contains("nvidia: the core channel read the shutdown update"));
    EXPECT_FALSE(engine_log_contains("after the shutdown update, head"));
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 0u);

    ASSERT_EQ(fw.mode_set_requests, 2u);
    EXPECT_EQ(fw.mode_set_phases[1], 0u);
    EXPECT_EQ(fw.mode_set_puts[1], TAKE_DOWN_END);

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 0u);
}

// A take-down whose wait finds GET past the push buffer's writable end is logged, and the mode set still ends
TEST(nvidia_display_engine, set_up_ends_the_mode_set_after_a_failed_take_down) {
    engine_firmware fw;
    fw.get_at_mode_set_start = BAD_GET;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: taking the display engine down failed: "));
    EXPECT_FALSE(engine_log_contains("nvidia: the display engine is taken down"));
    EXPECT_EQ(fw.mode_set_requests, 2u);
    EXPECT_EQ(fw.mode_set_phases[1], 0u);
    EXPECT_EQ(engine.release(), OK);
}

// A take-down that is the first update maps every present window ahead of the head's methods and an update naming no
// notifier, and the release then asks to keep nothing
TEST(nvidia_display_engine, take_down_as_the_first_update_maps_the_windows) {
    engine_firmware fw;
    fw.refusals[0] = {
        .made = engine_firmware::kind::control,
        .number = rm::CONTROL_DISPLAY_CHANGE,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    ASSERT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], INITIAL_METHODS_END);

    display_heads heads = {};
    heads.count = 2;
    for (uint8_t window = 0; window < CHIP_WINDOWS; window++) {
        heads.window_head[window] = NO_HEAD;
    }

    heads.window_head[0] = 0;
    heads.window_head[1] = 1;
    active_heads active = {};
    active.heads[1] = {.display_id = OUTPUT_0_ID, .link_index = BOOT_LINK, .link_type = SERIAL_LINK_TYPE};
    uint32_t end = INITIAL_METHODS_END + (2 * 2 + 4 + 5) * 2 * sizeof(uint32_t);
    g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)] = end;

    EXPECT_EQ(display_engine_test_access::take_down(engine, heads, 0x3, active, {}), OK);
    expect_core_method(fw, INITIAL_METHODS_END, NVIDIA_WINDOW_CONTROL, 0);
    expect_core_method(fw, INITIAL_METHODS_END + 8, NVIDIA_WINDOW_CONTROL + NVIDIA_WINDOW_STRIDE, 1);
    expect_core_method(fw, INITIAL_METHODS_END + 16, NVIDIA_WINDOW_USAGE_BOUNDS, NVIDIA_DEFAULT_USAGE_BOUNDS);
    uint32_t window_1_bounds = NVIDIA_WINDOW_USAGE_BOUNDS + NVIDIA_WINDOW_STRIDE;
    expect_core_method(fw, INITIAL_METHODS_END + 24, window_1_bounds, NVIDIA_DEFAULT_USAGE_BOUNDS);

    expect_head_stopped(fw, INITIAL_METHODS_END + 32, 1, LINK_3_CONTROL);
    expect_core_method(fw, INITIAL_METHODS_END + 64, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, INITIAL_METHODS_END + 96, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], end);

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_DISPLAY_RELEASE_FLAGS), 0u);
}

// The wait for the notifier one test gives an engine, long enough to measure and well below NVIDIA's 3 seconds
static constexpr uint64_t TEST_NOTIFIER_WAIT_NS = 20 * NS_PER_MS;

// The mode set's end comes after the shutdown update's wait for the notifier ran for the time the engine gives it
TEST(nvidia_display_engine, set_up_waits_for_the_notifier_for_the_engines_time) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    display_engine_test_access::set_notifier_wait(engine, TEST_NOTIFIER_WAIT_NS);

    ASSERT_EQ(set_up_logged(engine), OK);
    ASSERT_EQ(fw.mode_set_requests, 2u);
    EXPECT_TRUE(engine_log_contains("a lost notification"));

    uint64_t mode_set_ns = fw.mode_set_ns[1] - fw.mode_set_ns[0];
    EXPECT_GE(mode_set_ns, TEST_NOTIFIER_WAIT_NS);
    EXPECT_LT(mode_set_ns, COMPLETION_NOTIFIER_WAIT_NS);

    EXPECT_EQ(engine.release(), OK);
}

// A constructed engine gives the completion notifier NVIDIA's wait, which `new_engine` skips for the other tests
TEST(nvidia_display_engine, constructed_engine_waits_for_the_notifier_for_nvidias_time) {
    engine_firmware fw;
    engine_video_memory memory;
    display_engine& engine = *new (g_engine_bytes) display_engine(fw, memory);

    EXPECT_EQ(display_engine_test_access::notifier_wait_ns(engine), COMPLETION_NOTIFIER_WAIT_NS);
}

// After the shutdown update and the notifier's stop, a set-up asks for output 0's resource facts a third time, then for
// the resource's DisplayPort facts, both before the mode set's end, logging both and taking nothing down
TEST(nvidia_display_engine, set_up_finds_the_chosen_outputs_link_between_the_shutdown_update_and_the_end) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    ASSERT_TRUE(fw.call_count <= engine_firmware::MAX_CALLS);
    EXPECT_EQ(fw.control_count(rm::CONTROL_ASSIGN_OUTPUT_RESOURCE), 0u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_OUTPUT_RESOURCE), 3u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 1u);

    uint32_t facts = fw.last_control(rm::CONTROL_GET_OUTPUT_RESOURCE);
    uint32_t link_facts = fw.first_control(rm::CONTROL_GET_LINK_CAPABILITIES);
    EXPECT_LT(fw.first_control(rm::CONTROL_DISPLAY_CHANGE), facts);
    EXPECT_LT(facts, link_facts);
    EXPECT_LT(link_facts, fw.last_control(rm::CONTROL_DISPLAY_CHANGE));

    EXPECT_EQ(fw.put_at_link, SHUTDOWN_UPDATE_END + 16);
    EXPECT_EQ(fw.mode_set_puts[1], LIGHT_END + 16);

    EXPECT_TRUE(engine_log_contains("nvidia: display id 0x100 is driven by output resource 3 through its link A\r\n"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output resource 3's DisplayPort links run up to DisplayPort rate 0x1E, support FEC, take the "
        "increased watermark limits and support DisplayPort 1.2, and the firmware trains PHY repeaters\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("taken down"));
    EXPECT_EQ(engine.release(), OK);
}

// With the crossbar, the set-up asks display common to assign display id 0x100 an output resource after the mode set's
// start and before its facts, taking resource 2, the one the reply names, through link B, the facts' protocol
TEST(nvidia_display_engine, set_up_assigns_the_chosen_outputs_link_through_the_crossbar) {
    engine_firmware fw;
    fw.has_crossbar = true;
    fw.assigned_resource = 2;
    fw.boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_B;

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    uint32_t assignment = fw.first_control(rm::CONTROL_ASSIGN_OUTPUT_RESOURCE);
    EXPECT_EQ(fw.control_count(rm::CONTROL_ASSIGN_OUTPUT_RESOURCE), 1u);
    EXPECT_LT(fw.first_control(rm::CONTROL_DISPLAY_CHANGE), assignment);
    EXPECT_LT(assignment, fw.last_control(rm::CONTROL_GET_OUTPUT_RESOURCE));

    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100 is driven by output resource 2 through its link B, as display common assigned it\r\n"
    ));
    EXPECT_TRUE(engine_log_contains("nvidia: output resource 2's DisplayPort links run up to DisplayPort rate 0x1E"));
    EXPECT_FALSE(engine_log_contains("taken down"));
}

// Where a take-down after the shutdown update of one head and the notifier's stop ends, and where one with no head
// active ends, the 8 windows mapped ahead of its update
static constexpr uint32_t LINK_TAKE_DOWN_END    = TAKE_DOWN_END + 16;
static constexpr uint32_t NO_HEAD_TAKE_DOWN_END = INITIAL_METHODS_END + (8 * 2 + 5) * 2 * sizeof(uint32_t);

// A refused assignment leaves the link not set up, with no DisplayPort facts asked for, so a set-up with no head active
// takes the engine down, mapping the windows ahead of an update naming no notifier, then ends the mode set
TEST(nvidia_display_engine, set_up_takes_the_engine_down_after_a_refused_assignment) {
    engine_firmware fw;
    fw.has_crossbar = true;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_ASSIGN_OUTPUT_RESOURCE,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fw.boot_display_id = 0;
    fw.get_at_mode_set_start = NO_HEAD_TAKE_DOWN_END;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 0u);

    expect_core_method(fw, INITIAL_METHODS_END, NVIDIA_WINDOW_CONTROL, 0);
    expect_core_method(fw, INITIAL_METHODS_END + 128, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, INITIAL_METHODS_END + 160, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], NO_HEAD_TAKE_DOWN_END);

    ASSERT_EQ(fw.mode_set_requests, 2u);
    EXPECT_EQ(fw.mode_set_puts[1], NO_HEAD_TAKE_DOWN_END);

    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common refused to assign display id 0x100 an output resource, so "
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link is not set up, so bring-up takes the display engine down\r\n"
    ));
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core channel"));
    EXPECT_FALSE(engine_log_contains("is driven by output resource"));
    EXPECT_EQ(engine.release(), OK);
}

// A refused or late DisplayPort facts request after the shutdown update stops head 1 again and ends the mode set, a
// late one failing the set-up with its error even when the end is refused or fails too
TEST(nvidia_display_engine, set_up_takes_the_engine_down_after_a_failed_link_facts_request) {
    struct failure {
        int32_t facts_result;
        int32_t end_result;
        int32_t result;
    };

    const failure failures[] = {
        {ERR_FIRMWARE, OK, OK},
        {ERR_TIMEOUT, OK, ERR_TIMEOUT},
        {ERR_TIMEOUT, ERR_FIRMWARE, ERR_TIMEOUT},
        {ERR_TIMEOUT, ERR_BAD_STATE, ERR_TIMEOUT},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        fw.refusals[0] = {
            .made   = engine_firmware::kind::control,
            .number = rm::CONTROL_GET_LINK_CAPABILITIES,
            .object = 0,
            .result = failed.facts_result,
        };

        fw.get_at_link = LINK_TAKE_DOWN_END;
        fw.mode_set_end_result = failed.end_result;

        engine_video_memory memory;
        ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        expect_head_stopped(fw, SHUTDOWN_UPDATE_END + 16, 1, LINK_3_CONTROL);
        expect_core_method(fw, SHUTDOWN_UPDATE_END + 48, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
        expect_core_method(fw, SHUTDOWN_UPDATE_END + 80, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
        EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], LINK_TAKE_DOWN_END);

        ASSERT_EQ(fw.mode_set_requests, 2u);
        EXPECT_EQ(fw.mode_set_puts[1], LINK_TAKE_DOWN_END);

        bool is_refused = failed.facts_result == ERR_FIRMWARE;
        const char* refusal = "nvidia: display common refused the DisplayPort facts of output resource 3";
        EXPECT_EQ(engine_log_contains(refusal), is_refused);

        EXPECT_TRUE(engine_log_contains(
            "nvidia: display id 0x100's link is not set up, so bring-up takes the display engine down\r\n"
        ));
        EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core"));
        EXPECT_FALSE(engine_log_contains("DisplayPort links run up to"));
        EXPECT_EQ(engine.release(), OK);
    }
}

// A base buffer that cannot be allocated leaves the link alone: no output resource facts past the active head's and
// static check's, no assignment and no DisplayPort facts
TEST(nvidia_display_engine, set_up_finds_no_link_without_a_base_buffer) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.has_crossbar = true;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.scanout_requests, 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_OUTPUT_RESOURCE), 2u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_ASSIGN_OUTPUT_RESOURCE), 0u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 0u);
}

// Once the link's facts are read, a set-up trains it and reads its status at 0x0200C, once each and in that order,
// logging each
TEST(nvidia_display_engine, set_up_trains_the_chosen_outputs_link_after_reading_its_facts) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    ASSERT_TRUE(fw.call_count <= engine_firmware::MAX_CALLS);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_DP_AUX), 3u);

    uint32_t training = fw.first_control(rm::CONTROL_TRAIN_LINK);
    EXPECT_LT(fw.first_control(rm::CONTROL_GET_LINK_CAPABILITIES), training);
    EXPECT_LT(training, fw.last_control(rm::CONTROL_DP_AUX));

    EXPECT_EQ(fw.training_command, 0x2083u);
    EXPECT_EQ(fw.training_data, 0x1E01u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common trained display id 0x100's link at 1 lane at DisplayPort rate 0x1E (command 0x2083, 0 "
        "PHY repeaters trained first)\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link status reads 0x07 0x00 0x01, so its 1 lane is trained\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("taken down"));
}

// A refused request for the link's DisplayPort facts stops the link before its configuration is found, so no training
// is asked for
TEST(nvidia_display_engine, set_up_trains_no_link_whose_displayport_facts_are_refused) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_GET_LINK_CAPABILITIES,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 0u);
    EXPECT_TRUE(engine_log_contains("nvidia: display common refused the DisplayPort facts of output resource 0, so "));
    EXPECT_FALSE(engine_log_contains("link configuration"));
}

// A refused training stops the link before its status is read, logged without the training's own outcome
TEST(nvidia_display_engine, set_up_reads_no_status_of_a_link_whose_training_is_refused) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_TRAIN_LINK,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_LT(fw.last_control(rm::CONTROL_DP_AUX), fw.first_control(rm::CONTROL_TRAIN_LINK));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common refused to train display id 0x100's link at 1 lane at DisplayPort rate 0x1E (command "
        "0x2083, data 0x1E01), so "
    ));
    EXPECT_FALSE(engine_log_contains("trained display id"));
}

// A training whose reply never arrives fails the set-up with its error, and the link's status is not read
TEST(nvidia_display_engine, set_up_fails_with_the_error_of_a_training_failing_other_than_by_a_refusal) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_TRAIN_LINK,
        .object = 0,
        .result = ERR_TIMEOUT,
    };

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), ERR_TIMEOUT);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_LT(fw.last_control(rm::CONTROL_DP_AUX), fw.first_control(rm::CONTROL_TRAIN_LINK));
}

// A status read showing the lane untrained is logged as such, without the trained line, and takes the engine down
// before the mode set's end
TEST(nvidia_display_engine, set_up_logs_a_link_whose_status_shows_it_untrained) {
    static constexpr uint8_t CLOCK_RECOVERED_ONLY[] = {0x01, 0x00, 0x00};

    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.link_status = CLOCK_RECOVERED_ONLY;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link is not trained at 1 lane, its link status reading 0x01 0x00 0x00"
    ));
    EXPECT_FALSE(engine_log_contains("lane is trained"));

    EXPECT_TRUE(engine_log_contains("nvidia: display id 0x100's link is not set up, so bring-up takes the display"));
    EXPECT_EQ(g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)], LINK_TAKE_DOWN_END);
    EXPECT_EQ(fw.mode_set_puts[1], LINK_TAKE_DOWN_END);
}

// DisplayPort facts capping the links at HBR2, below the chip table's HBR3 that chose the mode, train the link at 2
// lanes at HBR2, and a status showing lane 0 trained but not lane 1 is logged as untrained
TEST(nvidia_display_engine, set_up_trains_at_the_lowest_configuration_the_displayport_facts_allow) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.displayport_max_rate = 3;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_EQ(fw.training_data, 0x1402u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common trained display id 0x100's link at 2 lanes at DisplayPort rate 0x14 (command 0x2083, "
        "0 PHY repeaters trained first)\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link is not trained at 2 lanes, its link status reading 0x07 0x00 0x01"
    ));
}

// DisplayPort facts capping the links at RBR leave no configuration carrying the mode, which stops the link before any
// training
TEST(nvidia_display_engine, set_up_trains_no_link_its_displayport_facts_leave_no_configuration) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.displayport_max_rate = 1;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_GET_LINK_CAPABILITIES), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: no link configuration carries display id 0x100's mode within output resource 0's DisplayPort facts, "
        "so "
    ));
    EXPECT_FALSE(engine_log_contains("trained display id"));
}

// DisplayPort facts giving the firmware as not training PHY repeaters train the link without the repeater bit
TEST(nvidia_display_engine, set_up_trains_without_the_phy_repeater_bit_when_the_firmware_trains_none) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.displayport_trains_phy_repeaters = 0;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_EQ(fw.training_command, 0x83u);
    EXPECT_EQ(fw.training_data, 0x1E01u);
}

// The links of a chip whose table gives its firmware as training PHY repeaters, as GA102's table does
static constexpr chip_displayport CHIP_LINKS_TRAINING_PHY_REPEATERS = {
    .trains_phy_repeaters       = true,
    .max_rate                   = displayport::link_rate::hbr3,
    .fec_supported              = true,
    .increased_watermark_limits = false,
};

// The capability bytes of one PHY repeater of revision 1.4 running up to HBR2 over four lanes, below the monitor's
// HBR3, so a configuration it capped would take 2 lanes for the mode
static constexpr uint8_t ONE_PHY_REPEATER[DPCD_PHY_REPEATER_CAPABILITY_BYTES] = {0x14, 0x14, 0x80, 0x00, 0x04};

// DisplayPort facts saying the firmware trains no PHY repeaters leave out one read under a chip table saying it does:
// the sink alone is trained, at 1 lane at HBR3 above the repeater's HBR2 and without the repeater bit, and checked
TEST(nvidia_display_engine, set_up_leaves_out_phy_repeaters_when_the_displayport_facts_say_the_firmware_trains_none) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.phy_repeater_capabilities = ONE_PHY_REPEATER;
    fw.displayport_trains_phy_repeaters = 0;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine, CHIP_LINKS_TRAINING_PHY_REPEATERS), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: output 0's PHY repeater capabilities from 0xF0000 are 14 14 80 00 04"));

    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_EQ(fw.training_command, 0x83u);
    EXPECT_EQ(fw.training_data, 0x1E01u);

    EXPECT_EQ(fw.repeater_status_reads, 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link status reads 0x07 0x00 0x01, so its 1 lane is trained\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("taken down"));
}

// A chip table saying the firmware trains no PHY repeaters leaves out a repeater NVIDIA's library would read under
// DisplayPort facts saying it trains them: the sink alone is trained, with the repeater bit, and checked
TEST(nvidia_display_engine, set_up_leaves_out_phy_repeaters_when_the_chip_table_says_the_firmware_trains_none) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.phy_repeater_capabilities = ONE_PHY_REPEATER;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_FALSE(engine_log_contains("PHY repeater capabilities"));

    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_EQ(fw.training_command, 0x2083u);
    EXPECT_EQ(fw.training_data, 0x1E01u);

    EXPECT_EQ(fw.repeater_status_reads, 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link status reads 0x07 0x00 0x01, so its 1 lane is trained\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("taken down"));
}

// The sink's own link status field, NVIDIA's NV_DPCD_LANE0_1_STATUS, read when either end lacks DisplayPort 1.2
static constexpr uint32_t DPCD_SINK_LINK_STATUS = 0x00202;

// DisplayPort facts without DisplayPort 1.2 have the status read from the sink's own field, so a monitor giving the
// trained bytes only there has its link found trained
TEST(nvidia_display_engine, set_up_reads_the_sinks_status_field_when_the_links_lack_displayport_1_2) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.displayport_versions = 0x2;
    fw.link_status_address = DPCD_SINK_LINK_STATUS;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link status reads 0x07 0x00 0x01, so its 1 lane is trained\r\n"
    ));
}

// A refused status read is logged with the set-up `OK` and the link stopped before its check
TEST(nvidia_display_engine, set_up_checks_no_link_whose_status_read_is_refused) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refuses_link_status = true;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_TRAIN_LINK), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common refused display id 0x100's 3-byte AUX read at 0x0200C, so "
    ));
    EXPECT_FALSE(engine_log_contains("link status read"));
}

// Once the link's status shows it trained, a set-up programs the head's stream on it and then the stream's attributes,
// once each, logging each
TEST(nvidia_display_engine, set_up_programs_the_stream_and_its_attributes_once_the_link_is_trained) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_CONFIGURE_STREAM), 1u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_STREAM_ATTRIBUTES), 1u);

    uint32_t stream = fw.first_control(rm::CONTROL_CONFIGURE_STREAM);
    EXPECT_LT(fw.last_control(rm::CONTROL_DP_AUX), stream);
    EXPECT_LT(stream, fw.first_control(rm::CONTROL_SET_STREAM_ATTRIBUTES));

    // The values `compute_stream` gives the AOC's 59.950 Hz timing at 1 lane at HBR3
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common programmed head 1's DisplayPort stream on output resource 0's link A: transfer units "
        "of 64 symbols, a watermark of 29 symbols, 499 symbols of horizontal blanking and 8534 of vertical\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common clears the stereo bits of display id 0x100's main stream attributes\r\n"
    ));
}

// With the crossbar, the stream goes on the output resource and link display common assigned, resource 2's link B
TEST(nvidia_display_engine, set_up_programs_the_stream_on_the_link_the_crossbar_assigns) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.has_crossbar = true;
    fw.assigned_resource = 2;
    fw.boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_B;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_CONFIGURE_STREAM), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common programmed head 1's DisplayPort stream on output resource 2's link B: "
    ));
}

// A refused assignment, DisplayPort facts request, training or stream stops the link there with the set-up `OK`,
// logged with every step it skips and without the step's own outcome, and sends none of the later requests
TEST(nvidia_display_engine, set_up_stops_the_link_at_its_first_refused_step) {
    struct stop {
        uint32_t    command;
        uint32_t    skipped;
        const char* line;
        const char* outcome;
    };

    const stop stops[] = {
        {rm::CONTROL_ASSIGN_OUTPUT_RESOURCE,
         rm::CONTROL_GET_LINK_CAPABILITIES,
         "nvidia: display common refused to assign display id 0x100 an output resource, so the link's DisplayPort "
         "facts are not read and the link is neither trained nor given a stream\r\n",
         "is driven by output resource"},
        {rm::CONTROL_GET_LINK_CAPABILITIES,
         rm::CONTROL_TRAIN_LINK,
         "nvidia: display common refused the DisplayPort facts of output resource 0, so the link is neither trained "
         "nor given a stream\r\n",
         "DisplayPort links run up to"},
        {rm::CONTROL_TRAIN_LINK,
         rm::CONTROL_CONFIGURE_STREAM,
         "nvidia: display common refused to train display id 0x100's link at 1 lane at DisplayPort rate 0x1E (command "
         "0x2083, data 0x1E01), so its link is neither checked nor given a stream\r\n",
         "trained display id"},
        {rm::CONTROL_CONFIGURE_STREAM,
         rm::CONTROL_SET_STREAM_ATTRIBUTES,
         "nvidia: display common refused head 1's DisplayPort stream on output resource 0's link A, so the stream's "
         "attributes are not overridden\r\n",
         "programmed head"},
    };

    for (const stop& stopped : stops) {
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        fw.has_crossbar = true;
        fw.get_at_link = LINK_TAKE_DOWN_END;
        fw.refusals[0] = {
            .made   = engine_firmware::kind::control,
            .number = stopped.command,
            .object = 0,
            .result = ERR_FIRMWARE,
        };

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), OK);
        EXPECT_EQ(fw.control_count(stopped.command), 1u);
        EXPECT_EQ(fw.control_count(stopped.skipped), 0u);
        EXPECT_TRUE(engine_log_contains(stopped.line));
        EXPECT_FALSE(engine_log_contains(stopped.outcome));
        EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core"));
    }
}

// A status read showing the lane untrained is logged with the stream it skips, and sends no stream
TEST(nvidia_display_engine, set_up_gives_no_stream_to_a_link_whose_status_shows_it_untrained) {
    static constexpr uint8_t CLOCK_RECOVERED_ONLY[] = {0x01, 0x00, 0x00};

    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.link_status = CLOCK_RECOVERED_ONLY;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_CONFIGURE_STREAM), 0u);
    EXPECT_EQ(fw.control_count(rm::CONTROL_SET_STREAM_ATTRIBUTES), 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display id 0x100's link is not trained at 1 lane, its link status reading 0x01 0x00 0x00, so the "
        "link is given no stream\r\n"
    ));
}

// A mode no configuration carries is logged with the training and stream it skips, and sends no stream
TEST(nvidia_display_engine, set_up_gives_no_stream_to_a_link_its_displayport_facts_leave_no_configuration) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.displayport_max_rate = 1;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_CONFIGURE_STREAM), 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: no link configuration carries display id 0x100's mode within output resource 0's DisplayPort facts, "
        "so the link is neither trained nor given a stream\r\n"
    ));
}

// A refused status read is logged with the check and stream it skips, and sends no stream
TEST(nvidia_display_engine, set_up_gives_no_stream_to_a_link_whose_status_read_is_refused) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refuses_link_status = true;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(fw.control_count(rm::CONTROL_CONFIGURE_STREAM), 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: display common refused display id 0x100's 3-byte AUX read at 0x0200C, so its link is neither checked "
        "nor given a stream\r\n"
    ));
}

// A refused override of the stream's attributes is logged and leaves the link ready with nothing taken down, while a
// late one takes the engine down and fails the set-up with its error, neither logging the stereo bits cleared
TEST(nvidia_display_engine, set_up_takes_the_engine_down_only_after_a_late_attributes_request) {
    const int32_t results[] = {ERR_FIRMWARE, ERR_TIMEOUT};
    for (int32_t result : results) {
        bool is_refused = result == ERR_FIRMWARE;
        engine_firmware fw;
        show_output_0_on_head_1(fw);
        fw.get_at_stream = is_refused ? 0 : LINK_TAKE_DOWN_END;
        fw.refusals[0] = {
            .made   = engine_firmware::kind::control,
            .number = rm::CONTROL_SET_STREAM_ATTRIBUTES,
            .object = 0,
            .result = result,
        };

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(AOC_BASE_BUFFER_BYTES));
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), is_refused ? OK : ERR_TIMEOUT);
        EXPECT_EQ(fw.control_count(rm::CONTROL_SET_STREAM_ATTRIBUTES), 1u);

        EXPECT_FALSE(engine_log_contains("clears the stereo bits"));
        EXPECT_EQ(
            engine_log_contains(
                "nvidia: display common refused to clear the stereo bits of display id 0x100's main stream attributes, "
                "so the stream keeps those the firmware sets\r\n"
            ),
            is_refused
        );
        EXPECT_EQ(
            engine_log_contains(
                "nvidia: display id 0x100's link is not set up, so bring-up takes the display engine down\r\n"
            ),
            !is_refused
        );
        EXPECT_EQ(engine_log_contains("bring-up takes the display engine down"), !is_refused);
        EXPECT_EQ(fw.mode_set_puts[1], is_refused ? LIGHT_END + 16 : LINK_TAKE_DOWN_END);
    }
}

// A window's first image in its push buffer, `WINDOW_IMAGE_METHODS` methods carrying 55 values
static constexpr uint32_t WINDOW_IMAGE_BYTES = (WINDOW_IMAGE_METHODS + 55) * sizeof(uint32_t);

// Where the core channel's armed state lies in its area, head 1's display id method, serial link 3's control naming
// head 1 over link B (NVIDIA's protocol 9), and serial link 2's control method
static constexpr uint32_t NVIDIA_ARMED_STATE = NVIDIA_ARMED_WINDOW_CONTROL - NVIDIA_WINDOW_CONTROL;
static constexpr uint32_t HEAD_1_DISPLAY_ID  = HEAD_0_DISPLAY_ID + NVIDIA_HEAD_STRIDE;
static constexpr uint32_t HEAD_1_ON_LINK_3_B = 0x902;
static constexpr uint32_t LINK_2_CONTROL     = 0x340;

// What `core_method_value` and `window_method_value` give for a method nothing writes
static constexpr uint32_t NO_VALUE = 0xFFFFFFFF;

// The value of the last one-value method writing `method` in the core channel's push buffer from `first` to `end`
static uint32_t core_method_value(const engine_firmware& fw, uint32_t first, uint32_t end, uint32_t method) {
    const uint32_t* words = static_cast<const uint32_t*>(paging::phys_to_virt(fw.core_push_buffer_address));
    uint32_t value = NO_VALUE;
    for (uint32_t offset = first; offset < end; offset += 2 * sizeof(uint32_t)) {
        if (words[offset / sizeof(uint32_t)] == ((1u << NVIDIA_COUNT_SHIFT) | method)) {
            value = words[offset / sizeof(uint32_t) + 1];
        }
    }

    return value;
}

// The value of the last method writing `method` in the window channel's push buffer before `end`, stepping by each
// method's count of values
static uint32_t window_method_value(const engine_firmware& fw, uint32_t end, uint32_t method) {
    const uint32_t* words = static_cast<const uint32_t*>(paging::phys_to_virt(fw.push_buffer_address));
    uint32_t value = NO_VALUE;
    for (uint32_t word = 0; word < end / sizeof(uint32_t); word += 1 + (words[word] >> NVIDIA_COUNT_SHIFT)) {
        if ((words[word] & ((1u << NVIDIA_COUNT_SHIFT) - 1)) == method) {
            value = words[word + 1];
        }
    }

    return value;
}

// Expects the chosen window's first image in its push buffer and its usage bounds among the core methods before
// `update`: the image interlocked with the core channel, naming the base buffer and look-up table, 1366 by 768
static void expect_first_image(const engine_firmware& fw, display_engine& engine, uint8_t window, uint32_t update) {
    window_usage_methods usage = {};
    build_window_usage_methods(window, 1366, usage);
    uint32_t bounds_method = NVIDIA_WINDOW_USAGE_BOUNDS + window * NVIDIA_WINDOW_STRIDE;
    uint32_t bounds = usage.methods[WINDOW_USAGE_METHODS - 1].value;
    EXPECT_EQ(core_method_value(fw, update - 8, update, bounds_method), bounds);

    rm::handle surface = display_engine_test_access::base_buffer_context_dma(engine);
    rm::handle look_up_table = display_engine_test_access::look_up_table_context_dma(engine);
    uint32_t end = WINDOW_IMAGE_BYTES;
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_INTERLOCK_FLAGS), rm::WINDOW_INTERLOCK_WITH_CORE);
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_SURFACE_CONTEXT_DMA), surface);
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_INPUT_TABLE_CONTEXT_DMA), look_up_table);

    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_SIZE), 1366u | 768u << rm::SIZE_HEIGHT_SHIFT);
    uint32_t pitch = base_buffer_pitch(1366) / rm::PLANE_PITCH_UNIT_BYTES;
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_PLANE_STORAGE), pitch);
}

// Expects an update's five methods from `offset`: the notifier named or 0, no cursor interlocked, the window interlocks
// and the update with RELEASE_ELV
static void expect_update(const engine_firmware& fw, uint32_t offset, rm::handle notifier, uint32_t window_interlocks) {
    uint32_t notifier_control = notifier != 0 ? NVIDIA_AWAKEN_AND_NOTIFY : 0;

    expect_core_method(fw, offset, NVIDIA_NOTIFIER_CONTEXT_DMA, notifier);
    expect_core_method(fw, offset + 8, NVIDIA_NOTIFIER_CONTROL, notifier_control);
    expect_core_method(fw, offset + 16, NVIDIA_INTERLOCK_FLAGS, 0);
    expect_core_method(fw, offset + 24, NVIDIA_WINDOW_INTERLOCKS, window_interlocks);
    expect_core_method(fw, offset + 32, NVIDIA_UPDATE, NVIDIA_RELEASE_ELV);
}

// After the shutdown update, head 1 lights output 0 on output resource 3's link B in one update naming the notifier and
// interlocked with window 3, whose first image shows the base buffer, and the armed state is logged
TEST(nvidia_display_engine, set_up_lights_the_boot_head_in_one_update_interlocked_with_its_window) {
    g_core_channel_registers[(NVIDIA_ARMED_STATE + HEAD_1_DISPLAY_ID) / sizeof(uint32_t)] = OUTPUT_0_ID;
    g_core_channel_registers[(NVIDIA_ARMED_STATE + LINK_3_CONTROL) / sizeof(uint32_t)] = HEAD_1_ON_LINK_3_B;

    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_B;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    uint32_t light = SHUTDOWN_UPDATE_END + 16;
    EXPECT_EQ(core_method_value(fw, light, LIGHT_END, HEAD_1_DISPLAY_ID), OUTPUT_0_ID);
    EXPECT_EQ(core_method_value(fw, light, LIGHT_END, LINK_3_CONTROL), HEAD_1_ON_LINK_3_B);
    expect_first_image(fw, engine, HEAD_1_WINDOW, LIGHT_END - 40);

    rm::handle look_up_table = display_engine_test_access::look_up_table_context_dma(engine);
    uint32_t head_1_output_table_context_dma = rm::CORE_HEAD_OUTPUT_TABLE_CONTEXT_DMA + NVIDIA_HEAD_STRIDE;
    EXPECT_EQ(core_method_value(fw, light, LIGHT_END, head_1_output_table_context_dma), look_up_table);

    rm::handle notifier = display_engine_test_access::notifier_context_dma(engine);
    expect_update(fw, LIGHT_END - 40, notifier, 1u << HEAD_1_WINDOW);
    expect_core_method(fw, LIGHT_END, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, LIGHT_END + 8, NVIDIA_NOTIFIER_CONTROL, 0);
    EXPECT_EQ(fw.mode_set_puts[1], LIGHT_END + 16);
    EXPECT_EQ(window_channel_word(HEAD_1_WINDOW, CHANNEL_PUT), WINDOW_IMAGE_BYTES);

    EXPECT_TRUE(engine_log_contains(
        "nvidia: the core channel read the update lighting head 1, its completion notifier reading 0x00000000\r\n"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: after the light, head 1's display id reads 0x100 and serial link 3's control 0x902 in the core "
        "channel's armed state\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("an update of its own"));
    EXPECT_FALSE(engine_log_contains("taken down"));

    g_core_channel_registers[(NVIDIA_ARMED_STATE + HEAD_1_DISPLAY_ID) / sizeof(uint32_t)] = 0;
    g_core_channel_registers[(NVIDIA_ARMED_STATE + LINK_3_CONTROL) / sizeof(uint32_t)] = 0;
}

// The armed owners giving windows 0 to 5 the heads they serve, 0 for windows 0 to 2 and 1 for windows 3 to 5
static constexpr uint32_t SERVED_OWNERS[] = {0, 0, 0, 1, 1, 1};

// Arms windows 0 to 5 with the owners `owners` gives
static void arm_owners(const uint32_t (&owners)[6]) {
    for (uint8_t window = 0; window < 6; window++) {
        set_armed_control(window, owners[window]);
    }
}

// With no head active the light's update is the first, and windows 6 and 7 alone change owner, serving no head, so it
// interlocks no window and window 0's usage bounds and first image follow in an update of their own naming no notifier
TEST(nvidia_display_engine, set_up_lights_window_0_apart_when_the_first_update_changes_an_owner) {
    arm_owners(SERVED_OWNERS);
    engine_firmware fw;
    fw.get_at_stream = FIRST_LIGHT_END;

    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_display_id = 0;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    uint32_t window_6_control = NVIDIA_WINDOW_CONTROL + 6 * NVIDIA_WINDOW_STRIDE;
    expect_core_method(fw, INITIAL_METHODS_END + 6 * 8, window_6_control, WINDOW_OWNER_NONE);

    rm::handle notifier = display_engine_test_access::notifier_context_dma(engine);
    expect_update(fw, FIRST_LIGHT_END - 40, notifier, 0);
    expect_core_method(fw, FIRST_LIGHT_END, NVIDIA_NOTIFIER_CONTEXT_DMA, 0);
    expect_core_method(fw, FIRST_LIGHT_END + 8, NVIDIA_NOTIFIER_CONTROL, 0);

    expect_first_image(fw, engine, 0, SECOND_UPDATE_END - 40);
    expect_update(fw, SECOND_UPDATE_END - 40, 0, 1u << 0);
    EXPECT_EQ(fw.mode_set_puts[1], SECOND_UPDATE_END);
    EXPECT_EQ(window_channel_word(0, CHANNEL_PUT), WINDOW_IMAGE_BYTES);

    EXPECT_TRUE(engine_log_contains("nvidia: the core channel read the update lighting head 0, its completion"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: window 0's first image follows in an update of its own naming no notifier, since the update lighting "
        "head 0 changed a window's owner or a present window serves no head\r\n"
    ));
    EXPECT_FALSE(engine_log_contains("taken down"));
    arm_owners({});
}

// Window 0's image follows in an update of its own when a present window is armed to a head it does not serve, or
// serves none even armed to none, as NVIDIA compares the owner field with the head it gives
TEST(nvidia_display_engine, set_up_lights_window_0_apart_for_a_changed_owner_or_a_window_serving_none) {
    struct change {
        uint32_t present_windows;
        uint8_t  mapped_windows;
        uint8_t  window;
        uint32_t control;
    };

    const change changes[] = {
        {0x3F, 6, 4, 0x0},
        {0x7F, 7, 6, WINDOW_OWNER_NONE},
    };

    for (const change& changed : changes) {
        arm_owners(SERVED_OWNERS);
        set_armed_control(changed.window, changed.control);

        uint32_t first_update_methods = changed.mapped_windows * 2 + HEAD_LIGHT_FIXED_METHODS + 3 + 5;
        uint32_t first_update_end = INITIAL_METHODS_END + first_update_methods * 2 * sizeof(uint32_t);
        engine_firmware fw;
        fw.present_windows = changed.present_windows;
        fw.get_at_stream = first_update_end;

        engine_video_memory memory;
        ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
        fw.boot_display_id = 0;
        display_engine& engine = new_engine(fw, memory);

        ASSERT_EQ(set_up_logged(engine), OK);
        rm::handle notifier = display_engine_test_access::notifier_context_dma(engine);
        expect_update(fw, first_update_end - 40, notifier, 0);
        EXPECT_TRUE(engine_log_contains("an update of its own"));
        EXPECT_FALSE(engine_log_contains("taken down"));
    }

    set_armed_control(6, 0);
    arm_owners({});
}

// The light's first update with windows 0 to 5 present, mapped ahead of head 0's fixed methods, a usage bound for each
// of its three windows, window 0's usage methods and the update's five, and where it ends
static constexpr uint32_t MAPPED_LIGHT_METHODS = 6 * 2 + HEAD_LIGHT_FIXED_METHODS + 3 + WINDOW_USAGE_METHODS + 5;
static constexpr uint32_t MAPPED_LIGHT_END     = INITIAL_METHODS_END + MAPPED_LIGHT_METHODS * 2 * sizeof(uint32_t);

// With no head active and each present window armed to the head it serves, the light's first update changes no owner,
// so it carries window 0's usage bounds and first image, interlocked with window 0's, and nothing follows it
TEST(nvidia_display_engine, set_up_lights_in_one_first_update_when_no_owner_changes) {
    arm_owners(SERVED_OWNERS);
    engine_firmware fw;
    fw.present_windows = 0x3F;
    fw.get_at_stream = MAPPED_LIGHT_END;

    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_display_id = 0;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle notifier = display_engine_test_access::notifier_context_dma(engine);
    expect_update(fw, MAPPED_LIGHT_END - 40, notifier, 1u << 0);
    expect_first_image(fw, engine, 0, MAPPED_LIGHT_END - 40);
    EXPECT_EQ(fw.mode_set_puts[1], MAPPED_LIGHT_END + 16);
    EXPECT_EQ(window_channel_word(0, CHANNEL_PUT), WINDOW_IMAGE_BYTES);

    EXPECT_FALSE(engine_log_contains("an update of its own"));
    EXPECT_FALSE(engine_log_contains("taken down"));
    arm_owners({});
}

// Where a take-down after a failed light ends: head 1's four methods for the active head, four again for the lit head,
// and the update's five after the light's update
static constexpr uint32_t LIGHT_TAKE_DOWN_END = LIGHT_END + (4 + 4 + 5) * 2 * sizeof(uint32_t);

// A light whose notifier wait fails takes the engine down, stopping the active head and the lit head on link 3 ahead of
// an update naming no notifier, and logs neither the light's read nor the armed state
TEST(nvidia_display_engine, set_up_takes_the_lit_head_down_after_a_failed_light) {
    engine_firmware fw;
    fw.get_at_stream = LIGHT_TAKE_DOWN_END;
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_B;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    expect_head_stopped(fw, LIGHT_END, 1, LINK_3_CONTROL);
    expect_head_stopped(fw, LIGHT_END + 32, 1, LINK_3_CONTROL);
    expect_update(fw, LIGHT_END + 64, 0, 0);
    EXPECT_EQ(fw.mode_set_puts[1], LIGHT_TAKE_DOWN_END);

    EXPECT_TRUE(engine_log_contains("nvidia: lighting head 1 failed: -4, so bring-up takes the display engine down"));
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core channel"));
    EXPECT_FALSE(engine_log_contains("the core channel read the update lighting head"));
    EXPECT_FALSE(engine_log_contains("after the light"));
}

// With the crossbar assigning output resource 2, a light whose notifier wait fails detaches link 3 from the active head
// and link 2 from the lit head ahead of an update naming no notifier
TEST(nvidia_display_engine, set_up_detaches_the_lit_heads_own_link_after_a_failed_light) {
    engine_firmware fw;
    fw.get_at_stream = LIGHT_TAKE_DOWN_END;

    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.has_crossbar = true;
    fw.assigned_resource = 2;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    expect_head_stopped(fw, LIGHT_END, 1, LINK_3_CONTROL);
    expect_head_stopped(fw, LIGHT_END + 32, 1, LINK_2_CONTROL);
    expect_update(fw, LIGHT_END + 64, 0, 0);
    EXPECT_EQ(fw.mode_set_puts[1], LIGHT_TAKE_DOWN_END);
}

// A first light whose notifier wait fails with no head active stops the lit head 0 alone, without mapping the windows
// again since its update was kicked, and writes nothing into window 0's channel
TEST(nvidia_display_engine, set_up_takes_down_the_lit_head_alone_after_a_failed_first_light) {
    engine_firmware fw;
    fw.get_at_stream = FIRST_LIGHT_END + (4 + 5) * 2 * sizeof(uint32_t);
    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_display_id = 0;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    expect_head_stopped(fw, FIRST_LIGHT_END, 0, LINK_3_CONTROL);
    expect_update(fw, FIRST_LIGHT_END + 32, 0, 0);
    EXPECT_EQ(fw.mode_set_puts[1], fw.get_at_stream);
    EXPECT_TRUE(is_push_buffer_blank(fw.push_buffer_address));
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core channel"));
}

// A refused stream on output resource 3's link B leaves the link not ready, so nothing is lit and the take-down stops
// the active head alone right after the shutdown update
TEST(nvidia_display_engine, set_up_lights_nothing_after_a_refused_stream) {
    engine_firmware fw;
    fw.get_at_link = LINK_TAKE_DOWN_END;
    fw.refusals[0] = {
        .made   = engine_firmware::kind::control,
        .number = rm::CONTROL_CONFIGURE_STREAM,
        .object = 0,
        .result = ERR_FIRMWARE,
    };

    engine_video_memory memory;
    ASSERT_TRUE(show_output_0_on_link_3(fw, memory));
    fw.boot_link_protocol = rm::PROTOCOL_DISPLAYPORT_B;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    expect_head_stopped(fw, SHUTDOWN_UPDATE_END + 16, 1, LINK_3_CONTROL);
    expect_update(fw, SHUTDOWN_UPDATE_END + 48, 0, 0);
    EXPECT_EQ(fw.mode_set_puts[1], LINK_TAKE_DOWN_END);
    EXPECT_TRUE(is_push_buffer_blank(fw.push_buffer_address));

    EXPECT_FALSE(engine_log_contains("lighting head"));
    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is taken down, its update read by the core channel"));
}

// Once output 0's base buffer is bound, a set-up maps it again and keeps that mapping, logging its BAR1 address and
// size, and the release leaves it mapped and allocated
TEST(nvidia_display_engine, set_up_maps_the_bound_base_buffer_for_userland_and_keeps_the_mapping) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.scanout_maps, 2u);
    EXPECT_EQ(memory.scanout_unmaps, 1u);

    const char* mapped =
        "nvidia: output 0's base buffer is mapped for userland at physical address 0x6002000000 in BAR1, 0x420000 "
        "bytes";
    const char* bound = "nvidia: output 0's base buffer is bound to window 3's channel";
    EXPECT_TRUE(engine_log_contains(mapped));
    EXPECT_LT(engine_log_offset(bound), engine_log_offset(mapped));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(memory.scanout_releases, 0u);
}

// A mapping for userland finding no room leaves the set-up `OK` and one failing otherwise fails it, each logged, with
// the base buffer kept bound, no look-up table allocated and no output lit
TEST(nvidia_display_engine, set_up_keeps_bound_a_base_buffer_it_cannot_map_for_userland) {
    struct failure {
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    const failure failures[] = {
        {ERR_NO_MEMORY,
         OK,
         "nvidia: output 0 cannot light, as mapping its base buffer for userland found no room, so its video memory "
         "stays bound and unused"},
        {ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for userland failed: -4, so its video "
         "memory stays bound and unused"},
        {ERR_BAD_STATE,
         ERR_BAD_STATE,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for userland failed: -5, so its video "
         "memory stays bound and unused"},
        {ERR_INVALID_ARG,
         ERR_INVALID_ARG,
         "nvidia: mapping output 0's base buffer at 0x1000000 in video memory for userland failed: -13, so its video "
         "memory stays bound and unused"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        engine_video_memory memory;
        ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
        memory.failing_scanout_map = 2;
        memory.scanout_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_TRUE(engine_log_contains("nvidia: output 0's base buffer is bound to window 3's channel"));
        EXPECT_FALSE(engine_log_contains("nvidia: output 0's base buffer is mapped for userland"));

        EXPECT_EQ(memory.scanout_maps, 2u);
        EXPECT_EQ(memory.scanout_releases, 0u);
        EXPECT_EQ(memory.general_requests, 1u);

        EXPECT_FALSE(engine.has_lit_output());
        EXPECT_NULL(engine.base_buffer(0));
        EXPECT_EQ(engine.release(), OK);
    }
}

// A mapping for userland at no physical address is taken down and leaves the set-up `OK`, logged, with the base
// buffer kept bound, no look-up table allocated and no output lit
TEST(nvidia_display_engine, set_up_unmaps_and_keeps_bound_a_base_buffer_mapped_at_no_physical_address) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    memory.physless_scanout_map = 2;
    display_engine& engine = new_engine(fw, memory);

    EXPECT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0 cannot light, as the mapping of its base buffer for userland has no physical address, so its "
        "video memory stays bound and unused"
    ));
    EXPECT_FALSE(engine_log_contains("nvidia: output 0's base buffer is mapped for userland"));

    EXPECT_EQ(memory.scanout_maps, 2u);
    EXPECT_EQ(memory.scanout_unmaps, 2u);
    EXPECT_EQ(memory.scanout_releases, 0u);
    EXPECT_EQ(memory.general_requests, 1u);

    EXPECT_FALSE(engine.has_lit_output());
    EXPECT_NULL(engine.base_buffer(0));
    EXPECT_EQ(engine.release(), OK);
}

// An output state relighting at 1 by 1 with a cursor, as a reused one may hold, for `read_output` to clear
static gfxfb::output_state stale_output_state() {
    gfxfb::output_state state = {};
    state.relighting = true;
    state.current = {.width = 1, .height = 1, .refresh_millihertz = 1};
    state.cursor_max_width = 0xFFFF;
    state.cursor_max_height = 0xFFFF;

    return state;
}

// Has the double move GET past the light's update to where the take-down after it ends, so the light's wait fails and
// the engine is taken down with no output lit
static void fail_the_light(engine_firmware& fw) {
    fw.get_at_stream = LIGHT_TAKE_DOWN_END;
}

// After a failed light, gfxfb reads the chosen output 0 as dark with no mode, relight, cursor or base buffer, its
// `boot_output` set since the boot head showed it, and a flush of it returns `gfxfb::ERR_DARK`
TEST(nvidia_display_engine, gfxfb_reads_the_chosen_output_dark_after_a_failed_light) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fail_the_light(fw);
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    gfxfb::output_state state = stale_output_state();
    ASSERT_EQ(engine.read_output(0, state), gfxfb::OK);
    EXPECT_FALSE(state.lit);
    EXPECT_FALSE(state.relighting);
    EXPECT_TRUE(state.boot_output);

    EXPECT_EQ(state.current.width, 0u);
    EXPECT_EQ(state.current.height, 0u);
    EXPECT_EQ(state.current.refresh_millihertz, 0u);
    EXPECT_EQ(state.cursor_max_width, 0u);
    EXPECT_EQ(state.cursor_max_height, 0u);

    EXPECT_FALSE(engine.has_lit_output());
    EXPECT_NULL(engine.base_buffer(0));
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::ERR_DARK);
    EXPECT_EQ(engine.release(), OK);
}

// Once lit, output 0 reads as lit at its 1366 by 768 timing with `boot_output` set and no relight or cursor, its base
// buffer the mapping for userland, write-combining, and a flush of it returns `gfxfb::OK`
TEST(nvidia_display_engine, gfxfb_reads_the_lit_output_at_its_mode_with_its_base_buffer) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_TRUE(engine.has_lit_output());
    EXPECT_EQ(engine.output_count(), 1u);
    EXPECT_FALSE(engine.needs_flush());

    gfxfb::output_state state = stale_output_state();
    ASSERT_EQ(engine.read_output(0, state), gfxfb::OK);
    EXPECT_TRUE(state.lit);
    EXPECT_FALSE(state.relighting);
    EXPECT_TRUE(state.boot_output);

    EXPECT_EQ(state.current.width, 1366u);
    EXPECT_EQ(state.current.height, 768u);
    EXPECT_EQ(state.current.refresh_millihertz, 59789u);
    EXPECT_EQ(state.cursor_max_width, 0u);
    EXPECT_EQ(state.cursor_max_height, 0u);

    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);
    EXPECT_EQ(base->width, 1366u);
    EXPECT_EQ(base->height, 768u);
    EXPECT_EQ(base->pitch, 5632u);

    EXPECT_EQ(base->size, BASE_BUFFER_1366_BY_768_BYTES);
    EXPECT_EQ(base->phys, USERLAND_BAR1_ADDRESS);
    EXPECT_FALSE(base->cacheable);

    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::OK);
    EXPECT_EQ(engine.release(), OK);
}

// A refused mode set end after the light changes nothing, so gfxfb still reads output 0 as lit with its base buffer
TEST(nvidia_display_engine, gfxfb_reads_the_output_lit_after_a_refused_mode_set_end) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fw.mode_set_end_result = ERR_FIRMWARE;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_TRUE(engine_log_contains("nvidia: display common refused the end of the mode set naming display id 0x100"));

    gfxfb::output_state state = {};
    ASSERT_EQ(engine.read_output(0, state), gfxfb::OK);
    EXPECT_TRUE(state.lit);
    EXPECT_NOT_NULL(engine.base_buffer(0));
    EXPECT_EQ(engine.release(), OK);
}

// A frame ending inside a page gets a base buffer of the whole pages its range holds, not of the frame's bytes
TEST(nvidia_display_engine, gfxfb_reads_a_base_buffer_of_the_whole_pages_its_range_holds) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    put_timing_first(TIMING_1366_BY_767);
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);
    EXPECT_EQ(memory.scanout_size, 0x41EA00u);

    EXPECT_EQ(base->height, 767u);
    EXPECT_EQ(base->pitch, 5632u);
    EXPECT_EQ(base->size, 0x41F000u);
    EXPECT_EQ(engine.release(), OK);
}

// With output 1 lit and output 0, which head 1 showed, dark, gfxfb reads `boot_output` set only on output 0 and only
// output 1 as lit with a base buffer, and the engine refuses output 2 with `gfxfb::ERR_INVALID_ARG`
TEST(nvidia_display_engine, gfxfb_reads_boot_output_only_on_an_output_an_active_head_showed) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    fw.boot_head = 1;
    fw.boot_display_id = OUTPUT_0_ID;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(engine.output_count(), 2u);

    gfxfb::output_state lit = {};
    ASSERT_EQ(engine.read_output(1, lit), gfxfb::OK);
    EXPECT_TRUE(lit.lit);
    EXPECT_FALSE(lit.boot_output);
    EXPECT_EQ(lit.current.width, 1366u);

    EXPECT_NOT_NULL(engine.base_buffer(1));
    EXPECT_EQ(engine.flush(1, nullptr, 0), gfxfb::OK);

    gfxfb::output_state dark = {};
    ASSERT_EQ(engine.read_output(0, dark), gfxfb::OK);
    EXPECT_FALSE(dark.lit);
    EXPECT_TRUE(dark.boot_output);
    EXPECT_EQ(dark.current.width, 0u);

    EXPECT_NULL(engine.base_buffer(0));
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::ERR_DARK);

    gfxfb::output_state past = {};
    EXPECT_EQ(engine.read_output(2, past), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.release(), OK);
}

// With head 0 showing output 1 and head 1 output 0, gfxfb reads `boot_output` set on both, output 1 lit and output 0
// dark
TEST(nvidia_display_engine, gfxfb_reads_boot_output_on_every_output_an_active_head_showed) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    fw.boot_display_id = OUTPUT_1_ID;
    fw.other_display_id = OUTPUT_0_ID;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    gfxfb::output_state lit = {};
    ASSERT_EQ(engine.read_output(1, lit), gfxfb::OK);
    EXPECT_TRUE(lit.lit);
    EXPECT_TRUE(lit.boot_output);

    gfxfb::output_state dark = {};
    ASSERT_EQ(engine.read_output(0, dark), gfxfb::OK);
    EXPECT_FALSE(dark.lit);
    EXPECT_TRUE(dark.boot_output);
    EXPECT_EQ(engine.release(), OK);
}

// Of 17 outputs set up, gfxfb reads the first `gfxfb::GFXFB_OUTPUTS_MAX` and the engine refuses the 17th with
// `gfxfb::ERR_INVALID_ARG`
TEST(nvidia_display_engine, gfxfb_reads_at_most_gfxfb_outputs_max_outputs) {
    engine_firmware fw;
    fw.supported_ids = (1u << 17) - 1;
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(engine.output_count(), gfxfb::GFXFB_OUTPUTS_MAX);
    gfxfb::output_state state = {};
    EXPECT_EQ(engine.read_output(gfxfb::GFXFB_OUTPUTS_MAX - 1, state), gfxfb::OK);
    EXPECT_EQ(engine.read_output(gfxfb::GFXFB_OUTPUTS_MAX, state), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.release(), OK);
}

// The engine returns `gfxfb::ERR_UNSUPPORTED` for buffers and the cursor, and for presents while flips are off, even
// once released, and a release of the base buffer frees and unmaps nothing, leaving it the lit output's
TEST(nvidia_display_engine, buffers_cursors_and_flips_off_presents_are_unsupported_and_release_buffer_frees_nothing) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    gfxfb::buffer* created = nullptr;
    gfxfb::cursor_image image = {};
    EXPECT_EQ(engine.create_buffer(64, 64, 0, &created), gfxfb::ERR_UNSUPPORTED);
    EXPECT_NULL(created);

    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_UNSUPPORTED);
    EXPECT_EQ(engine.set_cursor(0, image), gfxfb::ERR_UNSUPPORTED);
    EXPECT_EQ(engine.move_cursor(0, 1, 1), gfxfb::ERR_UNSUPPORTED);

    engine.release_buffer(base);
    EXPECT_EQ(engine.base_buffer(0), base);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(memory.scanout_releases, 0u);
    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_UNSUPPORTED);
}

// Once released, the lit output reads as dark with no base buffer and its flush fails with `gfxfb::ERR_DEVICE`,
// while the base buffer stays mapped and allocated
TEST(nvidia_display_engine, gfxfb_reads_the_lit_output_dark_and_its_flush_fails_once_released) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(engine.release(), OK);

    gfxfb::output_state state = {};
    ASSERT_EQ(engine.read_output(0, state), gfxfb::OK);
    EXPECT_FALSE(state.lit);
    EXPECT_TRUE(state.boot_output);

    EXPECT_FALSE(engine.has_lit_output());
    EXPECT_NULL(engine.base_buffer(0));
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::ERR_DEVICE);

    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(memory.scanout_releases, 0u);
}

// How many calls `count_registration` has taken, the display it was last handed, and the result it answers each with
static uint32_t        s_registration_calls = 0;
static gfxfb::display* s_registered_display = nullptr;
static int32_t         s_registration_result = gfxfb::OK;

// gfxfb's registration call as the registration tests give it, counting its calls, keeping the display it is handed and
// answering each with `s_registration_result`
static int32_t count_registration(gfxfb::display* display) {
    s_registration_calls++;
    s_registered_display = display;
    return s_registration_result;
}

// Runs `register_display_engine` on `engine` with `count_registration` answering `result`, the log kept
static int32_t register_logged(display_engine& engine, bool is_boot_display_shown, int32_t result = gfxfb::OK) {
    s_registration_calls = 0;
    s_registered_display = nullptr;
    s_registration_result = result;
    s_engine_log_bytes = 0;

    log::set_backend(&ENGINE_LOG_BACKEND);
    int32_t rc = register_display_engine(engine, is_boot_display_shown, count_registration);
    log::set_backend(nullptr);

    return rc;
}

// While gfxfb shows the boot framebuffer, an engine that has lit no output is left unregistered, which is logged
TEST(nvidia_display_engine, registration_leaves_the_boot_framebuffer_while_the_engine_has_lit_no_output) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fail_the_light(fw);
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, true), OK);
    EXPECT_EQ(s_registration_calls, 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the display engine has lit no output, so it is not registered and gfxfb keeps the boot framebuffer"
    ));
    EXPECT_FALSE(engine_log_contains("is gfxfb's display"));
    EXPECT_EQ(engine.release(), OK);
}

// With no boot framebuffer, an engine that has lit no output is registered, logging its one output as dark
TEST(nvidia_display_engine, registration_without_a_boot_framebuffer_registers_an_engine_that_has_lit_no_output) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    fail_the_light(fw);
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, false), OK);
    EXPECT_EQ(s_registration_calls, 1u);
    EXPECT_EQ(s_registered_display, &engine);

    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is gfxfb's display, of 1 output"));
    EXPECT_FALSE(engine_log_contains("of 1 outputs"));
    EXPECT_TRUE(engine_log_contains("nvidia: gfxfb reads output 0, an output the boot firmware lit, as dark"));
    EXPECT_EQ(engine.release(), OK);
}

// With output 0 lit, the registration logs it at its mode with its base buffer's BAR1 address and layout
TEST(nvidia_display_engine, registration_logs_the_lit_output_at_its_mode_with_its_base_buffer) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, true), OK);
    EXPECT_EQ(s_registration_calls, 1u);
    EXPECT_EQ(s_registered_display, &engine);

    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is gfxfb's display, of 1 output"));
    EXPECT_FALSE(engine_log_contains("of 1 outputs"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: gfxfb reads output 0, an output the boot firmware lit, as lit at 1366 by 768 at 59.789 Hz with its "
        "base buffer at physical address 0x6002000000 in BAR1, 768 rows of 5632 bytes"
    ));
    EXPECT_EQ(engine.release(), OK);
}

// With output 0 lit at a timing one row high, the registration logs its base buffer's lone row in the singular
TEST(nvidia_display_engine, registration_counts_the_lone_row_of_the_lit_outputs_base_buffer_in_the_singular) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_timing_first(TIMING_1366_BY_1);

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_1_PAGES_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, true), OK);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: gfxfb reads output 0, an output the boot firmware lit, as lit at 1366 by 1 at 1539.098 Hz with its "
        "base buffer at physical address 0x6002000000 in BAR1, 1 row of 5632 bytes"
    ));
    EXPECT_EQ(engine.release(), OK);
}

// With output 1 lit and output 0, which head 1 showed, dark, the registration logs both as gfxfb reads them
TEST(nvidia_display_engine, registration_logs_every_output_as_gfxfb_reads_it) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    fw.boot_head = 1;
    fw.boot_display_id = OUTPUT_0_ID;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, true), OK);
    EXPECT_EQ(s_registration_calls, 1u);
    EXPECT_EQ(s_registered_display, &engine);

    EXPECT_TRUE(engine_log_contains("nvidia: the display engine is gfxfb's display, of 2 outputs"));
    EXPECT_TRUE(engine_log_contains("nvidia: gfxfb reads output 0, an output the boot firmware lit, as dark"));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: gfxfb reads output 1, an output the boot firmware did not light, as lit at 1366 by 768 at 59.789 Hz "
        "with its base buffer at physical address 0x6002000000 in BAR1, 768 rows of 5632 bytes"
    ));
    EXPECT_EQ(engine.release(), OK);
}

// A registration gfxfb refuses fails with `ERR_NOT_REGISTERED`, logged, and logs no output
TEST(nvidia_display_engine, refused_registration_fails_with_err_not_registered) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    EXPECT_EQ(register_logged(engine, true, gfxfb::ERR), ERR_NOT_REGISTERED);
    EXPECT_EQ(s_registration_calls, 1u);
    EXPECT_EQ(s_registered_display, &engine);

    EXPECT_TRUE(engine_log_contains("nvidia: gfxfb refused the display engine as its display"));
    EXPECT_FALSE(engine_log_contains("is gfxfb's display"));
    EXPECT_FALSE(engine_log_contains("nvidia: gfxfb reads output"));
    EXPECT_EQ(engine.release(), OK);
}

// A head's timing event, interrupt status and interrupt enable and its raster position, as NVIDIA's headers give
// them, and LAST_DATA's bit, which written to the event clears it
static constexpr uint32_t NVIDIA_HEAD_TIMING_EVENT  = 0x611800;
static constexpr uint32_t NVIDIA_HEAD_TIMING_STATUS = 0x611C00;
static constexpr uint32_t NVIDIA_HEAD_TIMING_ENABLE = 0x611D80;
static constexpr uint32_t NVIDIA_RG_DPCA            = 0x616330;
static constexpr uint32_t NVIDIA_RG_DPCA_STRIDE     = 0x800;
static constexpr uint32_t NVIDIA_LAST_DATA          = 1u << 1;

// A word the engine never writes, so a write that lands on it fails
static constexpr uint32_t STALE_HEAD_WORD = 0xA5A5A5A5;

// The word of `g_display_registers` standing in for the display register at `offset`
static uint32_t& display_register(uint32_t offset) {
    return g_display_registers[(offset - DISPLAY_REGISTERS_START) / sizeof(uint32_t)];
}

// `head`'s word in the array of per-head words starting at `first`
static uint32_t& head_register(uint32_t first, uint8_t head) {
    return display_register(first + head * sizeof(uint32_t));
}

// `head`'s raster position in `g_display_registers`
static uint32_t& raster_register(uint8_t head) {
    return display_register(NVIDIA_RG_DPCA + head * NVIDIA_RG_DPCA_STRIDE);
}

// A view of BAR0 whose display registers are `g_display_registers`
static registers engine_registers() {
    return registers(reinterpret_cast<uintptr_t>(g_display_registers) - DISPLAY_REGISTERS_START);
}

// Services the vertical blanks of `pending_heads` elevated, as the card's interrupt handler does
static bool service_elevated(display_engine& engine, registers& regs, uint32_t pending_heads) {
    bool completes_first_second = false;
    RUN_ELEVATED(completes_first_second = engine.service_vertical_blanks(regs, pending_heads));
    return completes_first_second;
}

// Has `engine` log its first second with the log kept
static void log_first_second_logged(display_engine& engine) {
    s_engine_log_bytes = 0;
    log::set_backend(&ENGINE_LOG_BACKEND);
    engine.log_first_second();
    log::set_backend(nullptr);
}

// `set_up` disables every head's vertical blank with its event cleared, leaving those past the chip's heads, and the
// light enables its own head's, keeping the enables' other bits, while flips stay off until the driver turns them on
TEST(nvidia_display_engine, set_up_disables_every_heads_vertical_blank_and_the_light_enables_its_heads) {
    for (uint8_t head = 0; head < CHIP_HEADS; head++) {
        head_register(NVIDIA_HEAD_TIMING_ENABLE, head) = 0xFFFFFFFF;
        head_register(NVIDIA_HEAD_TIMING_EVENT, head) = STALE_HEAD_WORD;
    }

    head_register(NVIDIA_HEAD_TIMING_ENABLE, CHIP_HEADS) = STALE_HEAD_WORD;
    head_register(NVIDIA_HEAD_TIMING_EVENT, CHIP_HEADS) = STALE_HEAD_WORD;

    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);
    ASSERT_TRUE(engine.has_lit_output());

    uint8_t lit_head = display_engine_test_access::lit_head(engine);
    for (uint8_t head = 0; head < CHIP_HEADS; head++) {
        uint32_t expected_enable = head == lit_head ? 0xFFFFFFFF : ~NVIDIA_LAST_DATA;
        EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_ENABLE, head), expected_enable);
        EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, head), NVIDIA_LAST_DATA);
    }

    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_ENABLE, CHIP_HEADS), STALE_HEAD_WORD);
    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, CHIP_HEADS), STALE_HEAD_WORD);
    EXPECT_FALSE(engine.are_flips_on());
    engine.turn_on_flips();
    EXPECT_TRUE(engine.are_flips_on());
    EXPECT_EQ(engine.release(), OK);
}

// Each vertical blank on the lit head counts a frame, and the service completing the 59.789 Hz mode's 60 frames
// returns true once, keeping the first second's raster lines and the head's frame counts, which wrap
TEST(nvidia_display_engine, vertical_blanks_on_the_lit_head_complete_the_first_second_once) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    registers regs = engine_registers();
    uint8_t head = display_engine_test_access::lit_head(engine);
    uint32_t frames = display_engine_test_access::frames_per_second(engine);
    ASSERT_EQ(frames, 60u);

    uint32_t completions = 0;
    uint32_t completing_frame = 0;
    for (uint32_t frame = 1; frame <= frames + 2; frame++) {
        head_register(NVIDIA_HEAD_TIMING_STATUS, head) = NVIDIA_LAST_DATA;
        raster_register(head) = ((0xFFF0 + frame) << 16) | (770 + frame % 3);

        if (service_elevated(engine, regs, 1u << head)) {
            completions++;
            completing_frame = frame;
        }
    }

    EXPECT_EQ(completions, 1u);
    EXPECT_EQ(completing_frame, frames);
    EXPECT_EQ(display_engine_test_access::frame_count(engine), frames + 2);

    const first_second_statistics& statistics = display_engine_test_access::first_second(engine);
    EXPECT_EQ(statistics.start_frame, 0xFFF1u);
    EXPECT_EQ(statistics.end_frame, 0x002Cu);
    EXPECT_EQ(statistics.lowest_line, 770u);
    EXPECT_EQ(statistics.highest_line, 772u);
    EXPECT_LE(statistics.start_ns, statistics.end_ns);

    EXPECT_EQ(engine.release(), OK);
}

// A pending head driving no output has its event cleared and counts no frame, as does the lit head once released,
// while a head lacking LAST_DATA or its dispatch bit keeps its event, as does one past the chip's heads
TEST(nvidia_display_engine, vertical_blanks_off_the_lit_output_count_no_frame) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    registers regs = engine_registers();
    uint8_t lit_head = display_engine_test_access::lit_head(engine);
    uint8_t other_head = (lit_head + 1) % CHIP_HEADS;
    uint8_t unpending_head = (lit_head + 2) % CHIP_HEADS;
    uint32_t past_the_heads = ~((1u << CHIP_HEADS) - 1);

    head_register(NVIDIA_HEAD_TIMING_STATUS, other_head) = NVIDIA_LAST_DATA;
    head_register(NVIDIA_HEAD_TIMING_EVENT, other_head) = STALE_HEAD_WORD;
    head_register(NVIDIA_HEAD_TIMING_STATUS, lit_head) = ~NVIDIA_LAST_DATA;
    head_register(NVIDIA_HEAD_TIMING_EVENT, lit_head) = STALE_HEAD_WORD;

    head_register(NVIDIA_HEAD_TIMING_STATUS, unpending_head) = NVIDIA_LAST_DATA;
    head_register(NVIDIA_HEAD_TIMING_EVENT, unpending_head) = STALE_HEAD_WORD;
    head_register(NVIDIA_HEAD_TIMING_STATUS, CHIP_HEADS) = NVIDIA_LAST_DATA;
    head_register(NVIDIA_HEAD_TIMING_EVENT, CHIP_HEADS) = STALE_HEAD_WORD;

    uint32_t pending_heads = (1u << other_head) | (1u << lit_head) | past_the_heads;
    EXPECT_FALSE(service_elevated(engine, regs, pending_heads));
    EXPECT_EQ(display_engine_test_access::frame_count(engine), 0u);

    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, other_head), NVIDIA_LAST_DATA);
    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, lit_head), STALE_HEAD_WORD);
    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, unpending_head), STALE_HEAD_WORD);
    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, CHIP_HEADS), STALE_HEAD_WORD);

    EXPECT_EQ(engine.release(), OK);
    head_register(NVIDIA_HEAD_TIMING_STATUS, lit_head) = NVIDIA_LAST_DATA;
    EXPECT_FALSE(service_elevated(engine, regs, 1u << lit_head));
    EXPECT_EQ(head_register(NVIDIA_HEAD_TIMING_EVENT, lit_head), NVIDIA_LAST_DATA);
    EXPECT_EQ(display_engine_test_access::frame_count(engine), 0u);
}

// The first second is logged once it is serviced and only once, with its head, frames, mode, lines and the head's
// own frame count's advance, and nothing is logged before it
TEST(nvidia_display_engine, first_second_is_logged_once_after_its_last_frame) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    registers regs = engine_registers();
    uint8_t head = display_engine_test_access::lit_head(engine);
    uint32_t frames = display_engine_test_access::frames_per_second(engine);
    ASSERT_EQ(head, 1u);
    for (uint32_t frame = 1; frame < frames; frame++) {
        head_register(NVIDIA_HEAD_TIMING_STATUS, head) = NVIDIA_LAST_DATA;
        raster_register(head) = (frame << 16) | 770;
        service_elevated(engine, regs, 1u << head);
    }

    log_first_second_logged(engine);
    EXPECT_FALSE(engine_log_contains("vertical blanks"));

    head_register(NVIDIA_HEAD_TIMING_STATUS, head) = NVIDIA_LAST_DATA;
    raster_register(head) = (frames << 16) | 771;
    EXPECT_TRUE(service_elevated(engine, regs, 1u << head));

    log_first_second_logged(engine);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the handler serviced head 1's vertical blanks 1 to 60 since the light over "
    ));
    EXPECT_TRUE(engine_log_contains(
        " us, at its mode's 59.789 Hz, at raster lines 770 to 771, while the head's own frame count advanced 59"
    ));

    log_first_second_logged(engine);
    EXPECT_FALSE(engine_log_contains("vertical blanks"));
    EXPECT_EQ(engine.release(), OK);
}

// The light gives its head the chosen output, here output 1 on head 0, which showed it, and a vertical blank there
// counts a frame
TEST(nvidia_display_engine, vertical_blanks_count_on_the_head_lighting_output_1) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    fw.boot_display_id = OUTPUT_1_ID;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);
    ASSERT_TRUE(engine.has_lit_output());

    registers regs = engine_registers();
    uint8_t head = display_engine_test_access::lit_head(engine);
    ASSERT_EQ(head, 0u);
    EXPECT_EQ(display_engine_test_access::head_output(engine, head), 1u);
    EXPECT_EQ(display_engine_test_access::head_output(engine, 1), NO_OUTPUT);

    head_register(NVIDIA_HEAD_TIMING_STATUS, head) = NVIDIA_LAST_DATA;
    EXPECT_FALSE(service_elevated(engine, regs, 1u << head));
    EXPECT_EQ(display_engine_test_access::frame_count(engine), 1u);
    EXPECT_EQ(engine.release(), OK);
}

// A window notifier slot's begun and finished statuses in bits 31:30 of its first word, NVIDIA's
// NV_DISP_NOTIFIER__0_STATUS values
static constexpr uint32_t NVIDIA_SLOT_BEGUN    = 0x40000000;
static constexpr uint32_t NVIDIA_SLOT_FINISHED = 0x80000000;

// Mode times whose latch check falls 10 seconds after a vertical blank, within half of a 100-second period, so no
// check a test schedules fires while it runs
static constexpr mode_times SLOW_MODE_TIMES = {
    .period_ns            = 100000000000,
    .latch_check_delay_ns = 10000000000,
    .has_latch_check      = true,
};

// The presents an engine ended since `start_recording_present_ends`, and the display, output and result of the last
static uint32_t              g_present_end_count   = 0;
static gfxfb::display*       g_present_end_display = nullptr;
static uint32_t              g_present_end_output  = 0;
static gfxfb::present_result g_present_end         = {};

// Records a present's end where gfxfb's `present_done` would take it
static void record_present_end(gfxfb::display* display, uint32_t output, const gfxfb::present_result& result) {
    g_present_end_count++;
    g_present_end_display = display;
    g_present_end_output = output;
    g_present_end = result;
}

// Has `engine` report the presents it ends to `record_present_end`, counting from 0
static void start_recording_present_ends(display_engine& engine) {
    g_present_end_count = 0;
    display_engine_test_access::set_present_done(engine, record_present_end);
}

// Expects `count` ends recorded, the last from `g_built_engine` on `output` with `status`, `frame` and `timestamp_ns`
static void expect_last_present_end(
    uint32_t count,
    uint32_t output,
    uint32_t status,
    uint64_t frame,
    uint64_t timestamp_ns
) {
    EXPECT_EQ(g_present_end_count, count);
    EXPECT_EQ(g_present_end_display, static_cast<gfxfb::display*>(g_built_engine));
    EXPECT_EQ(g_present_end_output, output);
    EXPECT_EQ(g_present_end.status, status);
    EXPECT_EQ(g_present_end.frame, frame);
    EXPECT_EQ(g_present_end.timestamp_ns, timestamp_ns);
}

// The first word of the lit window's notifier slot `slot`, through the kernel's mapping of all physical memory
static uint32_t& notifier_slot_word(const display_engine& engine, uint32_t slot) {
    uint64_t address = display_engine_test_access::window_notifier_address(engine);
    uint32_t* words = static_cast<uint32_t*>(paging::phys_to_virt(address));
    return words[slot * 4];
}

// Presents `base` on output 0 and holds the present's deadline out of reach, so only the test ends it
static int32_t present_base_buffer(display_engine& engine, gfxfb::buffer* base) {
    int32_t rc = engine.present(0, base, nullptr, 0);
    if (rc == gfxfb::OK) {
        EXPECT_TRUE(display_engine_test_access::hold_deadline(engine));
    }

    return rc;
}

// Services one vertical blank of the lit head, as its interrupt does
static void service_lit_head(display_engine& engine) {
    registers regs = engine_registers();
    uint8_t head = display_engine_test_access::lit_head(engine);
    head_register(NVIDIA_HEAD_TIMING_STATUS, head) = NVIDIA_LAST_DATA;
    service_elevated(engine, regs, 1u << head);
}

// Starts keeping the log from an empty text, for a test to read the engine's lines
static void start_keeping_engine_log() {
    s_engine_log_bytes = 0;
    log::set_backend(&ENGINE_LOG_BACKEND);
}

// Ends what `start_keeping_engine_log` began, leaving the kept text for the test to read
static void stop_keeping_engine_log() {
    log::set_backend(nullptr);
}

// A present refuses with nothing written: flips off, an output not lit, a size off the mode and a buffer not bound to
// the window's channel in gfxfb's order, then a released engine
TEST(nvidia_display_engine, a_present_refuses_in_gfxfbs_order_with_nothing_written) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    gfxfb::buffer narrower = *base;
    narrower.width--;
    gfxfb::buffer wider = *base;
    wider.width++;

    gfxfb::buffer shorter = *base;
    shorter.height--;
    gfxfb::buffer taller = *base;
    taller.height++;
    gfxfb::buffer unbound = *base;

    uint8_t window = display_engine_test_access::lit_window(engine);
    uint32_t put = window_channel_word(window, CHANNEL_PUT);
    EXPECT_EQ(engine.present(1, &narrower, nullptr, 0), gfxfb::ERR_UNSUPPORTED);

    engine.turn_on_flips();
    EXPECT_EQ(engine.present(1, &narrower, nullptr, 0), gfxfb::ERR_DARK);

    EXPECT_EQ(engine.present(0, &narrower, nullptr, 0), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.present(0, &wider, nullptr, 0), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.present(0, &shorter, nullptr, 0), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.present(0, &taller, nullptr, 0), gfxfb::ERR_INVALID_ARG);
    EXPECT_EQ(engine.present(0, &unbound, nullptr, 0), gfxfb::ERR_REFUSED);

    EXPECT_EQ(window_channel_word(window, CHANNEL_PUT), put);
    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_FALSE(display_engine_test_access::is_deadline_scheduled(engine));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(engine.present(1, &narrower, nullptr, 0), gfxfb::ERR_DEVICE);
}

// Expects the window image a present wrote before `end` to show `surface` with the window's own interlock, naming
// `slot` of the window notifier whose context DMA is `notifier`
static void expect_present_image(
    const engine_firmware& fw,
    uint32_t end,
    uint8_t window,
    rm::handle surface,
    rm::handle notifier,
    uint32_t slot
) {
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_SURFACE_CONTEXT_DMA), surface);
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_INTERLOCK_FLAGS), 0u);
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_WINDOW_INTERLOCK_FLAGS), 1u << window);

    uint32_t notifier_control = slot << rm::NOTIFIER_CONTROL_OFFSET_SHIFT;
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_NOTIFIER_CONTEXT_DMA), notifier);
    EXPECT_EQ(window_method_value(fw, end, rm::WINDOW_SET_NOTIFIER_CONTROL), notifier_control);
}

// Presents take the notifier's slots in turn, each kicked to the end of its image and ended latched at the vertical
// blank finding its slot written, until the ninth needs GET: refused at offset 0, failing the display at GET all ones
TEST(nvidia_display_engine, presents_take_the_notifier_slots_in_turn_until_the_push_buffer_needs_get) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);
    uint8_t window = display_engine_test_access::lit_window(engine);
    window_channel_word(window, CHANNEL_GET) = 0;

    rm::handle notifier = display_engine_test_access::window_notifier_context_dma(engine);
    rm::handle surface = display_engine_test_access::base_buffer_context_dma(engine);
    ASSERT_NE(notifier, 0u);

    // The first image and eight presents leave 31 of the push buffer's 1012 writable words, too few for a ninth
    for (uint32_t present = 0; present < 8; present++) {
        uint32_t slot = present % NOTIFIER_SLOTS;
        notifier_slot_word(engine, slot) = NVIDIA_SLOT_FINISHED;
        uint64_t before_ns = clock::now_ns();
        ASSERT_EQ(engine.present(0, base, nullptr, 0), gfxfb::OK);
        uint64_t after_ns = clock::now_ns();
        EXPECT_EQ(notifier_slot_word(engine, slot), 0u);

        uint64_t deadline_ns = display_engine_test_access::queued_deadline_ns(engine);
        EXPECT_TRUE(display_engine_test_access::is_deadline_scheduled(engine));
        EXPECT_EQ(display_engine_test_access::deadline_timer_ns(engine), deadline_ns);
        EXPECT_GE(deadline_ns, before_ns + gfxfb::GFXFB_PRESENT_TIMEOUT_NS);
        EXPECT_LE(deadline_ns, after_ns + gfxfb::GFXFB_PRESENT_TIMEOUT_NS);
        EXPECT_TRUE(display_engine_test_access::hold_deadline(engine));

        uint32_t end = (present + 2) * WINDOW_IMAGE_BYTES;
        EXPECT_EQ(window_channel_word(window, CHANNEL_PUT), end);
        expect_present_image(fw, end, window, surface, notifier, slot);

        notifier_slot_word(engine, slot) = NVIDIA_SLOT_BEGUN;
        service_lit_head(engine);
        EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
        EXPECT_FALSE(display_engine_test_access::is_deadline_scheduled(engine));
        uint64_t blank_ns = display_engine_test_access::vertical_blank_ns(engine);
        expect_last_present_end(present + 1, 0, gfxfb::GFXFB_PRESENT_LATCHED, present + 1, blank_ns);
    }

    uint32_t put = window_channel_word(window, CHANNEL_PUT);
    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_REFUSED);
    EXPECT_EQ(window_channel_word(window, CHANNEL_PUT), put);
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::OK);

    window_channel_word(window, CHANNEL_GET) = 0xFFFFFFFF;
    start_keeping_engine_log();
    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_DEVICE);
    stop_keeping_engine_log();

    EXPECT_TRUE(engine_log_contains(
        "nvidia: writing a present into window 3's channel failed: -5, so the display failed"
    ));
    EXPECT_EQ(window_channel_word(window, CHANNEL_PUT), put);
    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::ERR_DEVICE);

    window_channel_word(window, CHANNEL_GET) = 0;
    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_DEVICE);
    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(g_present_end_count, 8u);
}

// A vertical blank finding the slot unwritten schedules the latch check, which leaves the present queued while the slot
// stays unwritten and then ends it latched at that blank's frame and time, cancelling the deadline
TEST(nvidia_display_engine, a_latch_check_ends_the_present_its_vertical_blank_found_unwritten) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    display_engine_test_access::set_mode_times(engine, SLOW_MODE_TIMES);
    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    ASSERT_EQ(present_base_buffer(engine, base), gfxfb::OK);
    service_lit_head(engine);
    uint64_t blank_ns = display_engine_test_access::vertical_blank_ns(engine);
    EXPECT_TRUE(display_engine_test_access::has_queued_present(engine));
    EXPECT_TRUE(display_engine_test_access::is_latch_check_scheduled(engine));
    EXPECT_EQ(
        display_engine_test_access::latch_check_timer_ns(engine),
        blank_ns + SLOW_MODE_TIMES.latch_check_delay_ns
    );

    display_engine_test_access::run_latch_check(engine);
    EXPECT_TRUE(display_engine_test_access::has_queued_present(engine));
    EXPECT_EQ(g_present_end_count, 0u);

    notifier_slot_word(engine, 0) = NVIDIA_SLOT_FINISHED;
    display_engine_test_access::run_latch_check(engine);
    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_FALSE(display_engine_test_access::is_deadline_scheduled(engine));
    expect_last_present_end(1, 0, gfxfb::GFXFB_PRESENT_LATCHED, 1, blank_ns);

    // A vertical blank that reports cancels the check an earlier one scheduled
    ASSERT_EQ(present_base_buffer(engine, base), gfxfb::OK);
    service_lit_head(engine);
    EXPECT_TRUE(display_engine_test_access::is_latch_check_scheduled(engine));

    notifier_slot_word(engine, 1) = NVIDIA_SLOT_BEGUN;
    service_lit_head(engine);
    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_FALSE(display_engine_test_access::is_latch_check_scheduled(engine));

    blank_ns = display_engine_test_access::vertical_blank_ns(engine);
    expect_last_present_end(2, 0, gfxfb::GFXFB_PRESENT_LATCHED, 3, blank_ns);
    EXPECT_EQ(engine.release(), OK);
}

// Before its deadline the timer leaves a present queued. At it, a present whose slot reads begun ends latched one past
// the last report at the time the timer runs, with no vertical blank serviced, logging so at most once a second
TEST(nvidia_display_engine, a_deadline_ends_a_present_its_head_took_latched_at_an_estimated_frame) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    // The second estimate falls within a second of the last log, and the third two seconds after it
    const uint64_t last_log_ago_ns[] = {0, 0, 2000 * NS_PER_MS};

    // The frame each estimate's log names, one past the report before it
    const char* const estimated_frames[] = {
        "ended latched at an estimated frame 1 and time ",
        "ended latched at an estimated frame 2 and time ",
        "ended latched at an estimated frame 3 and time ",
    };

    for (uint32_t present = 0; present < 3; present++) {
        ASSERT_EQ(present_base_buffer(engine, base), gfxfb::OK);
        display_engine_test_access::run_deadline(engine);
        EXPECT_TRUE(display_engine_test_access::has_queued_present(engine));

        if (present > 0) {
            uint64_t now_ns = clock::now_ns();
            ASSERT_TRUE(now_ns > last_log_ago_ns[present]);
            display_engine_test_access::set_last_estimate_log_ns(engine, now_ns - last_log_ago_ns[present]);
        }

        notifier_slot_word(engine, present) = NVIDIA_SLOT_BEGUN;
        display_engine_test_access::move_deadline(engine, 0);

        start_keeping_engine_log();
        uint64_t before_ns = clock::now_ns();
        display_engine_test_access::run_deadline(engine);
        uint64_t after_ns = clock::now_ns();
        stop_keeping_engine_log();

        EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
        EXPECT_EQ(g_present_end_count, present + 1);
        EXPECT_EQ(g_present_end_display, static_cast<gfxfb::display*>(&engine));
        EXPECT_EQ(g_present_end_output, 0u);

        EXPECT_EQ(g_present_end.status, gfxfb::GFXFB_PRESENT_LATCHED);
        EXPECT_EQ(g_present_end.frame, present + 1);
        EXPECT_GE(g_present_end.timestamp_ns, before_ns);
        EXPECT_LE(g_present_end.timestamp_ns, after_ns);

        bool must_log = present != 1;
        EXPECT_EQ(engine_log_contains("output 0's present reached its deadline with its head holding"), must_log);
        EXPECT_EQ(engine_log_contains(estimated_frames[present]), must_log);
    }

    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::OK);
    EXPECT_EQ(engine.release(), OK);
}

// At its deadline a present whose slot reads unwritten ends failed, cancelling its latch check, and fails the display,
// so presents and flushes return `ERR_DEVICE`
TEST(nvidia_display_engine, a_deadline_fails_the_display_for_a_present_its_head_never_took) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    display_engine_test_access::set_mode_times(engine, SLOW_MODE_TIMES);
    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    ASSERT_EQ(present_base_buffer(engine, base), gfxfb::OK);
    service_lit_head(engine);
    EXPECT_TRUE(display_engine_test_access::is_latch_check_scheduled(engine));

    display_engine_test_access::move_deadline(engine, 0);
    start_keeping_engine_log();
    display_engine_test_access::run_deadline(engine);
    stop_keeping_engine_log();

    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_FALSE(display_engine_test_access::is_latch_check_scheduled(engine));
    expect_last_present_end(1, 0, gfxfb::GFXFB_PRESENT_FAILED, 0, 0);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's present reached its deadline before its head took the buffer, so the display failed"
    ));

    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_DEVICE);
    EXPECT_EQ(engine.present(1, base, nullptr, 0), gfxfb::ERR_DEVICE);
    EXPECT_EQ(engine.flush(0, nullptr, 0), gfxfb::ERR_DEVICE);
    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(g_present_end_count, 1u);
}

// A release ends the queued present failed and cancels both timers
TEST(nvidia_display_engine, release_ends_the_queued_present_and_cancels_its_timers) {
    engine_firmware fw;
    engine_video_memory memory;
    ASSERT_TRUE(give_output_0_a_base_buffer(fw, memory));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    display_engine_test_access::set_mode_times(engine, SLOW_MODE_TIMES);
    gfxfb::buffer* base = engine.base_buffer(0);
    ASSERT_NOT_NULL(base);

    ASSERT_EQ(present_base_buffer(engine, base), gfxfb::OK);
    service_lit_head(engine);
    EXPECT_TRUE(display_engine_test_access::is_latch_check_scheduled(engine));
    EXPECT_TRUE(display_engine_test_access::is_deadline_scheduled(engine));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    EXPECT_FALSE(display_engine_test_access::is_latch_check_scheduled(engine));
    EXPECT_FALSE(display_engine_test_access::is_deadline_scheduled(engine));
    expect_last_present_end(1, 0, gfxfb::GFXFB_PRESENT_FAILED, 0, 0);
}

// Presents follow the lit output, here output 1 on head 0: one on output 0 is refused as dark, and one on output 1
// ends there, latched at its head's vertical blank
TEST(nvidia_display_engine, a_present_on_lit_output_1_ends_on_output_1) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    fw.boot_display_id = OUTPUT_1_ID;

    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BY_768_BYTES));
    display_engine& engine = new_engine(fw, memory);
    ASSERT_EQ(set_up(engine), OK);
    ASSERT_TRUE(engine.has_lit_output());

    engine.turn_on_flips();
    start_recording_present_ends(engine);
    gfxfb::buffer* base = engine.base_buffer(1);
    ASSERT_NOT_NULL(base);
    EXPECT_EQ(engine.present(0, base, nullptr, 0), gfxfb::ERR_DARK);

    ASSERT_EQ(engine.present(1, base, nullptr, 0), gfxfb::OK);
    EXPECT_TRUE(display_engine_test_access::hold_deadline(engine));
    notifier_slot_word(engine, 0) = NVIDIA_SLOT_BEGUN;
    service_lit_head(engine);

    EXPECT_FALSE(display_engine_test_access::has_queued_present(engine));
    uint64_t blank_ns = display_engine_test_access::vertical_blank_ns(engine);
    expect_last_present_end(1, 1, gfxfb::GFXFB_PRESENT_LATCHED, 1, blank_ns);
    EXPECT_EQ(engine.release(), OK);
}
