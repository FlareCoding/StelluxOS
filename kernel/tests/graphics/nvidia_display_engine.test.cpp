// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "common/logging.h"
#include "common/string.h"
#include "drivers/graphics/edid.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/core_channel.h"
#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/rm.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/vram_allocator.h"
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
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::firmware;
using drivers::nvidia::INSTANCE_MEMORY_BYTES;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
using drivers::nvidia::release_core_channel;
using drivers::nvidia::video_memory;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;
namespace displayport = drivers::nvidia::displayport;
namespace rm = drivers::nvidia::rm;

// The driver's client and device and the first handle the double gives, made up
static constexpr rm::handle TEST_CLIENT  = 0xC1D0000B;
static constexpr rm::handle TEST_DEVICE  = 0x0005000A;
static constexpr rm::handle FIRST_HANDLE = 0x0005000D;

// A display class and a core channel class no chip has, so each allocation's class names its object
static constexpr uint32_t TEST_DISPLAY_CLASS      = 0xC67F;
static constexpr uint32_t TEST_CORE_CHANNEL_CLASS = 0xC67E;

// A card's full address width, and a limit `dma::alloc_pages` refuses, so a set-up for a card limited to it fails at
// the push buffer
static constexpr uint8_t FULL_ADDRESS_BITS    = 64;
static constexpr uint8_t REFUSED_ADDRESS_BITS = 16;

// The heads of the chip `set_up` describes, whose initial methods outgrow the push buffer
static constexpr uint8_t CHIP_HEADS = 4;

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
static constexpr uint32_t NVIDIA_FIRST_RECORD_FIELD  = 0x101;
static constexpr uint64_t NVIDIA_FIRST_RECORD_OFFSET = 0x2020;

// A record's first word for snooped pages, read and write and pitch, the bytes of a record's address unit and NVIDIA's
// 4 KiB notifier in those units
static constexpr uint32_t NVIDIA_SNOOPED_PAGES_WORD_0 = 7;
static constexpr uint64_t NVIDIA_RECORD_UNIT_BYTES    = 256;
static constexpr uint64_t NVIDIA_NOTIFIER_UNITS       = 16;

// The memory the video memory double maps, and the words standing in for the core channel's PUT and GET
static uint8_t  g_video_memory[INSTANCE_MEMORY_BYTES];
static uint32_t g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t) + 1];

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

// A firmware that records the kind, object and number of each request in order, answers a request a refusal matches
// with its result, the head, display common's capability, active head and output reads from its fields, others as sent
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
    rm::handle reported_client = TEST_CLIENT; // the client `client()` reports, 0 making `bind` refuse
    uint64_t   push_buffer_address = 0;       // the address `CONTROL_SET_DISPLAY_PUSH_BUFFER` last carried
    uint32_t   boot_display_id = 0;           // the display id head 0 shows, every other head showing none
    uint32_t   boot_link_index = 0;           // the link index every link request's reply names
    uint32_t   boot_link_type = 0;            // the link type every link request's reply names
    bool       has_crossbar = false;          // whether display common's capabilities report a crossbar
    uint32_t   supported_ids = 0;             // the display ids reported supported, each a DisplayPort output
    uint32_t   digital_facts = DISPLAYPORT_FACTS; // the digital facts of every supported display id
    uint32_t   connected_ids = 0;             // those reported connected, the only ones with a monitor, giving `g_edid`
    const uint8_t* dpcd_receiver = DPCD_RECEIVER; // the receiver capability field of every connected monitor
    uint32_t   put_at_first_active_read = 0;  // the core channel's PUT when a head's display id was first asked

    int32_t allocate(rm::handle, rm::handle object, uint32_t class_id, void*, size_t) override {
        return answer(kind::allocation, object, class_id);
    }

    int32_t control(rm::handle object, uint32_t command, void* params, size_t) override {
        bool is_first_active_read = command == rm::CONTROL_GET_ACTIVE_DISPLAY && control_count(command) == 0;
        if (is_first_active_read) {
            put_at_first_active_read = g_core_channel_registers[CHANNEL_PUT / sizeof(uint32_t)];
        }

        int32_t rc = answer(kind::control, object, command);
        if (rc != OK || params == nullptr) {
            return rc;
        }

        if (reads_unconnected_monitor(command, params)) {
            return ERR_FIRMWARE;
        }

        fill_reply(command, params);
        fill_output_reply(command, params);

        return OK;
    }

    int32_t release(rm::handle object) override {
        return answer(kind::release, object, 0);
    }

    int32_t internal_control(uint32_t command, void* params, size_t) override {
        if (command == rm::CONTROL_SET_DISPLAY_PUSH_BUFFER) {
            push_buffer_address = static_cast<rm::display_push_buffer_params*>(params)->address;
        }

        return answer(kind::internal_control, 0, command);
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

    // Gives `heads` heads the display engine can use, from head 0, each served by the window of its number, head 0
    // showing `boot_display_id`, link replies `boot_link_index` and `boot_link_type`, and a crossbar if `has_crossbar`
    void fill_reply(uint32_t command, void* params) const {
        if (command == rm::CONTROL_GET_HEAD_COUNT) {
            static_cast<rm::head_count_params*>(params)->head_count = heads;
        } else if (command == rm::CONTROL_GET_HEAD_MASK) {
            static_cast<rm::head_mask_params*>(params)->head_mask = (1u << heads) - 1;
        } else if (command == rm::CONTROL_GET_WINDOW_HEADS) {
            rm::window_heads_params* reply = static_cast<rm::window_heads_params*>(params);
            for (uint8_t head = 0; head < heads; head++) {
                reply->window_heads[head] = static_cast<uint8_t>(1u << head);
            }
        } else if (command == rm::CONTROL_GET_ACTIVE_DISPLAY) {
            rm::active_display_params* reply = static_cast<rm::active_display_params*>(params);
            reply->display_id = reply->head == 0 ? boot_display_id : 0;
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
};

// A video memory that hands out offset 0, refusing more bytes than `g_video_memory` holds, and maps it over that array,
// so a release or an unmap has nothing to undo
class engine_video_memory : public video_memory {
public:
    int32_t allocate(uint64_t size, uint64_t, vram_use, vram_range& out) override {
        if (size > sizeof(g_video_memory)) {
            return ERR_NO_MEMORY;
        }

        out = {.offset = 0, .size = size};
        return OK;
    }

    void release(const vram_range&) override {}

    int32_t map_for_cpu(const vram_range&, cpu_mapping& out) override {
        out.virt = reinterpret_cast<uintptr_t>(g_video_memory);
        out.phys = 0;
        return OK;
    }

    void unmap_for_cpu(const vram_range&) override {}
};

// An engine over `fw` and `memory`, built in `g_engine_bytes`
static display_engine& new_engine(engine_firmware& fw, engine_video_memory& memory) {
    return *new (g_engine_bytes) display_engine(fw, memory);
}

// Sets up `engine` under `TEST_DEVICE` for a card driving `address_bits` physical address bits, with four heads, eight
// windows and links doing `links`. PUT and GET are in `g_core_channel_registers`, GET reading `get`, no DMA page freed
static int32_t set_up(
    display_engine& engine,
    uint8_t address_bits = FULL_ADDRESS_BITS,
    uint32_t get = INITIAL_METHODS_END,
    const chip_displayport& links = CHIP_LINKS
) {
    g_core_channel_registers[CHANNEL_GET / sizeof(uint32_t)] = get;
    registers regs(reinterpret_cast<uintptr_t>(g_core_channel_registers) - CORE_CHANNEL_AREA);
    dma::device_profile card = dma::device_profile_test_access::build(address_bits, true);

    chip_info chip = {};
    chip.display = {
        .head_count         = CHIP_HEADS,
        .window_count       = 8,
        .display_class      = TEST_DISPLAY_CLASS,
        .core_channel_class = TEST_CORE_CHANNEL_CLASS,
        .displayport        = links,
    };

    return engine.set_up(regs, card, chip, TEST_DEVICE);
}

// The 32-bit word at `offset` in the instance memory, which the video memory double maps over `g_video_memory`
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

// After a full set-up the release first sends what `release_core_channel` sends for the channel and the display it was
// allocated under, then releases each of the display's objects once, and a second release sends nothing
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
    EXPECT_EQ(release_core_channel(channel_release, display, channel), OK);

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
static char     s_engine_log[8192] = {};
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

// Sets up `engine` with the log kept, for a chip whose links do `links` and a card driving `address_bits` physical
// address bits
static int32_t set_up_logged(
    display_engine& engine,
    const chip_displayport& links = CHIP_LINKS,
    uint8_t address_bits = FULL_ADDRESS_BITS
) {
    s_engine_log_bytes = 0;
    log::set_backend(&ENGINE_LOG_BACKEND);
    int32_t rc = set_up(engine, address_bits, INITIAL_METHODS_END, links);
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
// and a set-up logs that reason and that no output is chosen to light
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
        "window 0"
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
        "window 0"
    ));
}

// With both outputs given modes and head 0 showing output 1, a set-up chooses output 1 alone, the boot output being
// tried before output 0
TEST(nvidia_display_engine, set_up_chooses_the_boot_output_alone_of_two_with_modes) {
    engine_firmware fw;
    fw.supported_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.connected_ids = OUTPUT_0_ID | OUTPUT_1_ID;
    fw.boot_display_id = OUTPUT_1_ID;

    copy_aoc_edid(true);
    engine_video_memory memory;
    display_engine& engine = new_engine(fw, memory);

    ASSERT_EQ(set_up_logged(engine), OK);
    EXPECT_EQ(engine_log_count("is chosen to light first"), 1u);
    EXPECT_TRUE(engine_log_contains(
        "nvidia: output 1 is chosen to light first, at its EDID's timing 0, 2560 by 1440 at 59.950 Hz, on head 0 with "
        "window 0"
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
