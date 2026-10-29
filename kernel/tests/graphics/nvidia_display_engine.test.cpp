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
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/rm.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_display_engine);

using drivers::nvidia::CHANNEL_GET;
using drivers::nvidia::CHANNEL_PUT;
using drivers::nvidia::chip_displayport;
using drivers::nvidia::chip_info;
using drivers::nvidia::CORE_CHANNEL_AREA;
using drivers::nvidia::CORE_INITIAL_METHODS_PER_HEAD;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::display_engine;
using drivers::nvidia::DPCD_RECEIVER_CAPABILITY_BYTES;
using drivers::nvidia::ERR_BAD_MESSAGE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_FIRMWARE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::INSTANCE_MEMORY_BYTES;
using drivers::nvidia::LOOK_UP_TABLE_SURFACE_BYTES;
using drivers::nvidia::look_up_table_word;
using drivers::nvidia::NS_PER_MS;
using drivers::nvidia::OK;
using drivers::nvidia::PUSH_BUFFER_BYTES;
using drivers::nvidia::registers;
using drivers::nvidia::release_display_channel;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
using drivers::nvidia::window_channel_area;
using drivers::nvidia::WINDOW_OWNER_NONE;
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

// The heads of the chip `set_up` describes, whose initial methods outgrow the push buffer, and its windows
static constexpr uint8_t CHIP_HEADS   = 4;
static constexpr uint8_t CHIP_WINDOWS = 8;

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

// The words standing in for BAR0 from the core channel's area to the end of the last window channel's
static uint32_t g_channel_areas[(window_channel_area(CHIP_WINDOWS) - CORE_CHANNEL_AREA) / sizeof(uint32_t)];

// Where the video memory double hands out scanout memory, the bytes past it the double backs too, so a write running
// past the range shows, and the byte the double fills them with, as it fills each general range and guard
static constexpr uint64_t SCANOUT_OFFSET      = 0x1000000;
static constexpr uint64_t SCANOUT_GUARD_BYTES = 0x1000;
static constexpr uint8_t  SCANOUT_FILL        = 0xA5;

// `SCANOUT_FILL` in each byte of a 64-bit word, as the video memory double finds it in scanout words left unwritten
static constexpr uint64_t SCANOUT_FILL_WORD = 0xA5A5A5A5A5A5A5A5;

// The pages the video memory double hands general memory out in, as the allocator rounds each request to them
static constexpr uint64_t DOUBLE_PAGE_BYTES = 0x1000;

// The core channel's area, where `g_channel_areas` begins, holding its PUT, GET and armed state
static uint32_t* const g_core_channel_registers = g_channel_areas;

// The object under test, which its instance memory's copy of the hash table makes too large for a test's stack
alignas(display_engine) static uint8_t g_engine_bytes[sizeof(display_engine)];

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

// A byte of the header every EDID starts with, where the AOC's two 18-byte timing descriptors start and their bytes,
// and where its third descriptor, its range limits, starts
static constexpr uint32_t HEADER_FF_BYTE           = 1;
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

// A firmware that records each request in order, answers one a refusal matches with its result, the push buffer, head,
// window, crossbar, active head, output, bandwidth check and channel state replies from its fields, others as sent
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
    uint8_t    boot_head = 0;                 // the head showing `boot_display_id`, every other head showing none
    uint32_t   boot_display_id = 0;
    uint32_t   boot_link_index = 0;           // the link index every link request's reply names
    uint32_t   boot_link_type = 0;            // the link type every link request's reply names
    bool       has_crossbar = false;          // whether display common's capabilities report a crossbar
    uint32_t   supported_ids = 0;             // the display ids reported supported, each a DisplayPort output
    uint32_t   digital_facts = DISPLAYPORT_FACTS; // the digital facts of every supported display id
    uint32_t   connected_ids = 0;             // those reported connected, the only ones with a monitor, giving `g_edid`
    const uint8_t* dpcd_receiver = DPCD_RECEIVER; // the receiver capability field of every connected monitor
    uint32_t   refused_modes = 0;             // how many of the first bandwidth check requests the check refuses
    uint32_t   put_at_first_active_read = 0;  // the core channel's PUT when a head's display id was first asked
    bool       is_channel_busy = false;       // whether each channel state reply names the busy state, else idle
    bool       has_state_request = false;     // whether a channel state request has come
    uint64_t   first_state_request_ns = 0;    // when the first channel state request came
    uint32_t   armed_control_at_reply = 0;    // window 0's armed control after each state request, 0 leaving it alone

    int32_t allocate(rm::handle, rm::handle object, uint32_t class_id, void*, size_t) override {
        return answer(kind::allocation, object, class_id);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t) override {
        bool is_first_active_read = command == rm::CONTROL_GET_ACTIVE_DISPLAY && control_count(command) == 0;
        if (is_first_active_read) {
            put_at_first_active_read = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
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

        if (reads_unconnected_monitor(command, params)) {
            return ERR_FIRMWARE;
        }

        fill_reply(command, params);
        fill_output_reply(command, params);
        fill_check_reply(command, params);
        fill_state_reply(command, params);

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

    // Acknowledges a DPCD read with the bytes of `dpcd_receiver` from its address, every other byte reading 0
    void fill_aux_reply(rm::aux_params& reply) const {
        uint32_t bytes = reply.size + 1;
        for (uint32_t i = 0; i < bytes && reply.address + i < DPCD_RECEIVER_CAPABILITY_BYTES; i++) {
            reply.data[i] = dpcd_receiver[reply.address + i];
        }

        reply.size = bytes;
        reply.reply = rm::AUX_REPLY_ACK;
    }

    // Gives `heads` heads the display engine can use, from head 0, each with `windows_per_head` windows, `boot_head`
    // showing `boot_display_id`, link replies `boot_link_index` and `boot_link_type`, and a crossbar if `has_crossbar`
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
            reply->display_id = reply->head == boot_head ? boot_display_id : 0;
        } else if (command == rm::CONTROL_GET_OUTPUT_RESOURCE) {
            rm::output_resource_params* reply = static_cast<rm::output_resource_params*>(params);
            reply->index = boot_link_index;
            reply->type = boot_link_type;
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
};

// A video memory giving general memory in whole pages from offset 0 of `g_video_memory`, mapped one range at a time
// through its copy, and scanout memory at `SCANOUT_OFFSET` over `scanout`, counting calls and failing them as set
class engine_video_memory : public video_memory {
public:
    int32_t  general_allocate_result = OK;
    uint32_t failing_general_request = 0;       // the number, from 1, of the general request failing, 0 for none
    int32_t  general_map_result      = OK;
    uint32_t failing_general_map     = 0;       // the number, from 1, of the general mapping failing, 0 for none
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

            if (size > scanout_bytes) {
                return ERR_NO_MEMORY;
            }

            out = {.offset = SCANOUT_OFFSET, .size = size};
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

            uint8_t* view = g_mapped_video_memory + range.offset;
            string::memcpy(view, g_video_memory + range.offset, range.size);
            string::memset(view + range.size, SCANOUT_FILL, GENERAL_GUARD_BYTES);
            general_mapped = range;

            out.virt = reinterpret_cast<uintptr_t>(view);
            out.phys = 0;
            return OK;
        }

        scanout_maps++;
        if (scanout_map_result != OK) {
            return scanout_map_result;
        }

        out.virt = reinterpret_cast<uintptr_t>(scanout);
        out.phys = 0;

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

// An engine over `fw` and `memory`, built in `g_engine_bytes`
static display_engine& new_engine(engine_firmware& fw, engine_video_memory& memory) {
    return *new (g_engine_bytes) display_engine(fw, memory);
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

// Copies `AOC_BASE_BLOCK` into `g_edid` with its timing descriptors zeroed into display descriptors of a kind the
// parser passes over, so the EDID parses with no timing
static void copy_aoc_edid_without_timings() {
    copy_aoc_edid(true);

    uint8_t zeroed_sum = 0;
    for (uint32_t i = 0; i < TIMING_DESCRIPTORS_BYTES; i++) {
        zeroed_sum = static_cast<uint8_t>(zeroed_sum + g_edid[TIMING_DESCRIPTORS_BYTE + i]);
    }

    string::memset(g_edid + TIMING_DESCRIPTORS_BYTE, 0, TIMING_DESCRIPTORS_BYTES);
    g_edid[CHECKSUM_BYTE] = static_cast<uint8_t>(g_edid[CHECKSUM_BYTE] + zeroed_sum);
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
    EXPECT_EQ(release_display_channel(channel_release, display, channel), OK);

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
        "nvidia: display id 0x100's EDID of 128 bytes does not start with the EDID header"
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
    EXPECT_TRUE(engine_log_contains("nvidia: output 0 has no mode, as its link carries none of its EDID's 2 timings"));
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
        "nvidia: output 0 has no mode, as none of its EDID's 2 timings both refreshes below 60.600 Hz and is carried "
        "by its link"
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

// The present windows lacking the chosen window leave it with no channel, logged, the set-up `OK`, neither its push
// buffer given nor its channel allocated, and no base buffer asked for
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
        engine_firmware channel_releases;
        EXPECT_EQ(release_display_channel(channel_releases, display, fw.allocated(TEST_WINDOW_CHANNEL_CLASS)), OK);
        EXPECT_EQ(release_display_channel(channel_releases, display, fw.allocated(TEST_CORE_CHANNEL_CLASS)), OK);
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
static constexpr uint64_t BASE_BUFFER_1366_BYTES = 0x420000;

// Replaces the AOC's first timing in `g_edid`, the preferred one, with `TIMING_1366_BY_768`, which stays preferred
static void put_1366_pixel_timing_first() {
    uint8_t replaced_sum = 0;
    for (uint32_t i = 0; i < sizeof(TIMING_1366_BY_768); i++) {
        replaced_sum = static_cast<uint8_t>(replaced_sum + g_edid[TIMING_DESCRIPTORS_BYTE + i] - TIMING_1366_BY_768[i]);
    }

    string::memcpy(g_edid + TIMING_DESCRIPTORS_BYTE, TIMING_1366_BY_768, sizeof(TIMING_1366_BY_768));
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

// Output 0's 1366 by 768 timing gets 768 rows 5632 bytes apart, rounded up from 5464, as scanout memory at NVIDIA's
// alignment, cleared through one mapping taken down after, the bytes past it untouched, logged, and kept by the release
TEST(nvidia_display_engine, set_up_allocates_and_clears_the_chosen_outputs_base_buffer) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_1366_pixel_timing_first();
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.scanout_requests, 1u);
    EXPECT_EQ(memory.scanout_size, BASE_BUFFER_1366_BYTES);
    EXPECT_EQ(memory.scanout_alignment, NVIDIA_SURFACE_ALIGNMENT);

    EXPECT_EQ(memory.scanout_maps, 1u);
    EXPECT_EQ(memory.scanout_unmaps, 1u);
    EXPECT_EQ(memory.scanout_fill_words, 0u);
    EXPECT_TRUE(scanout_holds(memory, 0, BASE_BUFFER_1366_BYTES, 0));
    EXPECT_TRUE(scanout_holds(memory, BASE_BUFFER_1366_BYTES, SCANOUT_GUARD_BYTES, SCANOUT_FILL));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's base buffer lies at 0x1000000 in video memory, its 768 rows 5632 bytes apart, and is "
        "cleared to black"
    ));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(memory.scanout_releases, 0u);
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

// The numbers of the channels of window 0, which a set-up chooses for output 1 as the only connected output, and of
// window 3, which it chooses for output 0 on head 1, NVIDIA's NV_PDISP_CHN_NUM_WIN(0) and NV_PDISP_CHN_NUM_WIN(3)
static constexpr uint32_t OUTPUT_1_WINDOW_CHANNEL = 1;
static constexpr uint32_t HEAD_1_WINDOW_CHANNEL   = 4;

// A set-up's general mappings by number when it chooses an output, after the instance memory's clear and the
// notifier's binding, and the table's general request, after the instance memory's
static constexpr uint32_t BASE_BUFFER_BINDING_MAP = 3;
static constexpr uint32_t TABLE_FILL_MAP          = 4;
static constexpr uint32_t TABLE_BINDING_MAP       = 5;
static constexpr uint32_t TABLE_REQUEST           = 2;

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
    put_1366_pixel_timing_first();
}

// Whether the hash table entry naming `object` binds it under the double's client to `hardware_channel` with the
// record `record`, from 0 the lowest, whose words are `words`
static bool binds(rm::handle object, uint32_t hardware_channel, uint32_t record, const uint32_t (&words)[5]) {
    uint32_t entry = entry_of(object);
    if (entry == NVIDIA_HASH_ENTRIES) {
        return false;
    }

    uint32_t client_bits = TEST_CLIENT & NVIDIA_CLIENT_MASK;
    uint32_t record_bits = (NVIDIA_FIRST_RECORD_FIELD + record) << NVIDIA_RECORD_FIELD_SHIFT;
    uint32_t channel_bits = hardware_channel << NVIDIA_CHANNEL_SHIFT;
    if (instance_word(entry * NVIDIA_ENTRY_BYTES + sizeof(uint32_t)) != (client_bits | record_bits | channel_bits)) {
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

// Output 1 on window 0 has its base buffer, then the look-up table surface, bound to the window's channel, number 1,
// each under a new handle with the next record naming its memory and logged, and the release keeps both bound
TEST(nvidia_display_engine, set_up_binds_the_base_buffer_and_the_look_up_table_surface_to_the_windows_channel) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle base_buffer_context_dma = FIRST_HANDLE + fw.handle_count - 2;
    rm::handle table_context_dma = FIRST_HANDLE + fw.handle_count - 1;
    EXPECT_EQ(used_entry_count(), 3u);
    EXPECT_TRUE(binds(base_buffer_context_dma, OUTPUT_1_WINDOW_CHANNEL, 1, BASE_BUFFER_RECORD));
    EXPECT_TRUE(binds(table_context_dma, OUTPUT_1_WINDOW_CHANNEL, 2, TABLE_RECORD));

    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1's base buffer is bound to window 0's channel as context DMA 0x"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface is bound to window 0's channel as context DMA 0x"
    ));

    EXPECT_EQ(engine.release(), OK);
    EXPECT_EQ(used_entry_count(), 3u);
    EXPECT_EQ(memory.scanout_releases, 0u);
    EXPECT_EQ(memory.general_releases, 0u);
}

// Output 0 on head 1 has both surfaces bound to its window's channel, window 3's, number 4, rather than head 1's or
// window 0's
TEST(nvidia_display_engine, set_up_binds_head_1s_surfaces_to_window_3s_channel) {
    engine_firmware fw;
    show_output_0_on_head_1(fw);
    put_1366_pixel_timing_first();
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    rm::handle base_buffer_context_dma = FIRST_HANDLE + fw.handle_count - 2;
    rm::handle table_context_dma = FIRST_HANDLE + fw.handle_count - 1;
    EXPECT_TRUE(binds(base_buffer_context_dma, HEAD_1_WINDOW_CHANNEL, 1, BASE_BUFFER_RECORD));
    EXPECT_TRUE(binds(table_context_dma, HEAD_1_WINDOW_CHANNEL, 2, TABLE_RECORD));

    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 0's base buffer is bound to window 3's channel as context DMA 0x"
    ));
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface is bound to window 3's channel as context DMA 0x"
    ));
}

// The table takes `LOOK_UP_TABLE_SURFACE_BYTES` of general memory at NVIDIA's alignment, its whole pages and nothing
// past them written as `look_up_table_word` gives through one mapping taken down after, and where it lies logged
TEST(nvidia_display_engine, set_up_fills_the_look_up_table_surface_with_nvidias_identity) {
    engine_firmware fw;
    connect_output_1_alone(fw);
    engine_video_memory memory;
    ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(memory.general_requests, TABLE_REQUEST);
    EXPECT_EQ(memory.general_size, LOOK_UP_TABLE_SURFACE_BYTES);
    EXPECT_EQ(memory.general_alignment, NVIDIA_SURFACE_ALIGNMENT);
    EXPECT_EQ(memory.general_maps, TABLE_BINDING_MAP);
    EXPECT_EQ(memory.general_unmaps, TABLE_BINDING_MAP);

    EXPECT_EQ(memory.general_overruns, 0u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: the look-up table surface lies at 0x10000 in video memory, its input table passing each color through "
        "unchanged"
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

// A base buffer binding finding no room leaves output 1 with no base buffer and the set-up `OK`, and one failing
// otherwise fails the set-up with its error, each logged, not as bound, the buffer released and no table allocated
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
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
        memory.failing_general_map = BASE_BUFFER_BINDING_MAP;
        memory.general_map_result = failed.error;
        display_engine& engine = new_engine(fw, memory);

        EXPECT_EQ(set_up_logged(engine), failed.result);
        EXPECT_TRUE(engine_log_contains(failed.line));
        EXPECT_FALSE(engine_log_contains("nvidia: output 1's base buffer is bound"));

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
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
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

// A table mapping or binding finding no room leaves the engine with none and the set-up `OK`, and one failing otherwise
// fails the set-up with its error, each logged, not as bound, the table released and the base buffer kept bound
TEST(nvidia_display_engine, set_up_releases_a_look_up_table_surface_it_cannot_fill_or_bind) {
    struct failure {
        uint32_t    map;
        int32_t     error;
        int32_t     result;
        const char* line;
    };

    const failure failures[] = {
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
        {TABLE_BINDING_MAP,
         ERR_NO_MEMORY,
         OK,
         "nvidia: the display engine has no look-up table surface, as binding it to window 0's channel found no room, "
         "so its video memory is released"},
        {TABLE_BINDING_MAP,
         ERR_TIMEOUT,
         ERR_TIMEOUT,
         "nvidia: binding the look-up table surface to window 0's channel failed: -4, so its video memory is released"},
    };

    for (const failure& failed : failures) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));
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
        {0, TABLE_BINDING_MAP},
    };

    for (const no_room& missing : cases) {
        engine_firmware fw;
        connect_output_1_alone(fw);

        engine_video_memory memory;
        memory.failing_general_request = missing.request;
        memory.general_allocate_result = ERR_NO_MEMORY;
        memory.failing_general_map = missing.map;
        memory.general_map_result = ERR_NO_MEMORY;
        ASSERT_TRUE(memory.give_scanout(BASE_BUFFER_1366_BYTES));

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
    put_1366_pixel_timing_first();
    return memory.give_scanout(BASE_BUFFER_1366_BYTES);
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
