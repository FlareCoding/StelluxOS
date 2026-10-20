// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/bar0_window.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// The room NVIDIA gives each string of the guest system information, the arguments of `FUNCTION_SET_GUEST_SYSTEM_INFO`
constexpr uint32_t GUEST_STRING_BYTES = 256;

namespace {

// The guest system information in NVIDIA's layout, which the reply carries back with the version the firmware accepts
struct guest_system_info {
    uint32_t vgx_version_major;
    uint32_t vgx_version_minor;
    uint32_t driver_version_bytes;
    uint32_t branch_bytes;
    uint32_t title_bytes;
    uint32_t changelist;
    char     driver_version[GUEST_STRING_BYTES];
    char     branch[GUEST_STRING_BYTES];
    char     title[GUEST_STRING_BYTES];
};

// One entry of the table of video memory regions in NVIDIA's static configuration
struct region_entry {
    uint64_t base;
    uint64_t limit;
    uint64_t reserved;
    uint32_t performance;
    uint8_t  supports_compression;
    uint8_t  supports_scanout;
    uint8_t  is_protected;
    uint8_t  type_blacklist[17];
};

} // namespace

static_assert(sizeof(guest_system_info) == 792, "NVIDIA's layout of the guest system information");
static_assert(sizeof(FIRMWARE_VERSION) <= GUEST_STRING_BYTES, "the driver's version fits its room");
static_assert(sizeof(region_entry) == 48, "NVIDIA's layout of a video memory region");

// The bytes of the reply's copy of the arguments that give the VGX version, the first two fields
constexpr uint32_t VGX_VERSION_BYTES = 2 * sizeof(uint32_t);

// The bytes of NVIDIA's static configuration for this firmware, and where in it the table of video memory regions lies:
// a 32-bit count of regions, then the entries from the next 8-byte boundary
constexpr uint32_t STATIC_INFO_BYTES     = 2168;
constexpr uint32_t REGION_TABLE_OFFSET   = 840;
constexpr uint32_t REGION_ENTRIES_OFFSET = REGION_TABLE_OFFSET + 8;
constexpr uint32_t REGION_TABLE_END      = REGION_ENTRIES_OFFSET + VIDEO_MEMORY_REGIONS_MAX * sizeof(region_entry);

// Where the field holding the 64-bit offset of BAR1's root page directory and those holding the internal client's and
// subdevice's handles lie in the static configuration, and where the last field read, the subdevice's, ends
constexpr uint32_t BAR1_PAGE_DIRECTORY_FIELD_OFFSET = 2088;
constexpr uint32_t INTERNAL_CLIENT_FIELD_OFFSET     = 2152;
constexpr uint32_t INTERNAL_SUBDEVICE_FIELD_OFFSET  = 2160;
constexpr uint32_t READ_FIELDS_END                  = INTERNAL_SUBDEVICE_FIELD_OFFSET + sizeof(rm::handle);

static_assert(REGION_TABLE_END <= BAR1_PAGE_DIRECTORY_FIELD_OFFSET, "the table of regions ends before the field");
static_assert(READ_FIELDS_END <= STATIC_INFO_BYTES, "the fields read lie inside the static configuration");

int32_t confirm_message_version(gsp& conversation, uint64_t timeout_ns) {
    guest_system_info info = {};
    info.vgx_version_major = VGX_VERSION_MAJOR;
    info.vgx_version_minor = VGX_VERSION_MINOR;
    info.driver_version_bytes = GUEST_STRING_BYTES;
    info.branch_bytes = GUEST_STRING_BYTES;
    info.title_bytes = GUEST_STRING_BYTES;
    string::memcpy(info.driver_version, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION));

    received_message reply = {};
    int32_t rc = conversation.request(
        FUNCTION_SET_GUEST_SYSTEM_INFO,
        &info,
        sizeof(info),
        sizeof(info),
        reply,
        timeout_ns
    );
    if (rc != OK) {
        return rc;
    }

    if (gsp::to_error(reply.private_result) != OK) {
        log::error(
            "nvidia: the GSP's firmware refuses VGX version 0x%X.%02X for the driver's messages, with private result "
            "0x%X, in a reply of %u payload bytes, after which the version reads 0x%X.%02X",
            VGX_VERSION_MAJOR,
            VGX_VERSION_MINOR,
            reply.private_result,
            reply.payload_bytes,
            info.vgx_version_major,
            info.vgx_version_minor
        );
        return ERR_FIRMWARE;
    }

    if (reply.payload_bytes < VGX_VERSION_BYTES) {
        log::error(
            "nvidia: the GSP's firmware replies to the driver's VGX version with %u payload bytes, too few to give the "
            "version it accepts",
            reply.payload_bytes
        );
        return ERR_BAD_MESSAGE;
    }

    bool is_same_version = info.vgx_version_major == VGX_VERSION_MAJOR && info.vgx_version_minor == VGX_VERSION_MINOR;
    if (!is_same_version) {
        log::error(
            "nvidia: the GSP's firmware replies with VGX version 0x%X.%02X, while the driver's messages follow "
            "0x%X.%02X",
            info.vgx_version_major,
            info.vgx_version_minor,
            VGX_VERSION_MAJOR,
            VGX_VERSION_MINOR
        );
        return ERR_FIRMWARE;
    }

    return OK;
}

static region_entry region_entry_at(const uint8_t* info, uint32_t index) {
    region_entry entry = {};
    string::memcpy(&entry, info + REGION_ENTRIES_OFFSET + index * sizeof(region_entry), sizeof(entry));
    return entry;
}

// Reads the table of regions from the static configuration copied to `info`
static int32_t read_region_table(const uint8_t* info, static_configuration& configuration) {
    uint32_t count = read_u32(info + REGION_TABLE_OFFSET);
    bool is_count_in_range = count >= 1 && count <= VIDEO_MEMORY_REGIONS_MAX;
    if (!is_count_in_range) {
        log::error(
            "nvidia: the GSP firmware's static configuration gives %u video memory regions, outside the range of 1 "
            "to %u",
            count,
            VIDEO_MEMORY_REGIONS_MAX
        );
        return ERR_BAD_MESSAGE;
    }

    // Every entry is checked before any is handed back, so `configuration` stays unchanged on every error
    for (uint32_t i = 0; i < count; i++) {
        region_entry entry = region_entry_at(info, i);
        if (entry.limit < entry.base) {
            log::error(
                "nvidia: the GSP firmware's video memory region %u ends at 0x%lX, below its first byte 0x%lX",
                i,
                entry.limit,
                entry.base
            );
            return ERR_BAD_MESSAGE;
        }

        bool spans_all_bytes = entry.limit - entry.base + 1 == 0;
        if (spans_all_bytes) {
            log::error(
                "nvidia: the GSP firmware's video memory region %u spans all 2^64 bytes, more than a 64-bit size holds",
                i
            );
            return ERR_BAD_MESSAGE;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        region_entry entry = region_entry_at(info, i);
        video_memory_region& region = configuration.regions[i];

        region.offset = entry.base;
        region.size = entry.limit - entry.base + 1;
        region.reserved_bytes = entry.reserved;
        region.is_reserved = entry.reserved != 0;
        region.is_protected = entry.is_protected != 0;
        region.supports_scanout = entry.supports_scanout != 0;
        region.supports_compression = entry.supports_compression != 0;
    }

    configuration.region_count = count;

    return OK;
}

// Reads the members of `configuration` from the static configuration copied to `info`, of which the reply's payload
// filled the first `info_bytes` bytes
static int32_t read_configuration(
    const uint8_t* info,
    uint32_t info_bytes,
    uint64_t video_memory_size,
    static_configuration& configuration
) {
    if (info_bytes < READ_FIELDS_END) {
        log::error(
            "nvidia: the GSP firmware's static configuration has %u bytes, ending before the field holding the "
            "internal subdevice's handle ends at byte %u",
            info_bytes,
            READ_FIELDS_END
        );
        return ERR_BAD_MESSAGE;
    }

    uint64_t page_directory_offset = 0;
    string::memcpy(&page_directory_offset, info + BAR1_PAGE_DIRECTORY_FIELD_OFFSET, sizeof(page_directory_offset));

    bool is_entry_aligned = page_directory_offset % sizeof(uint64_t) == 0;
    bool is_below_window_reach = page_directory_offset < BAR0_WINDOW_REACH;
    bool is_entry_in_video_memory = lies_inside(page_directory_offset, sizeof(uint64_t), video_memory_size);
    if (!is_entry_aligned || !is_below_window_reach || !is_entry_in_video_memory) {
        log::error(
            "nvidia: the GSP firmware's static configuration places BAR1's root page directory at 0x%lX in video "
            "memory, refused since that offset is not a multiple of 8 below 0x%lX (the reach of the base of BAR0's "
            "window) or the directory's first entry ends past the 0x%lX bytes of video memory the card's boot "
            "firmware records",
            page_directory_offset,
            BAR0_WINDOW_REACH,
            video_memory_size
        );
        return ERR_BAD_MESSAGE;
    }

    int32_t rc = read_region_table(info, configuration);
    if (rc != OK) {
        return rc;
    }

    configuration.bar1_page_directory_offset = page_directory_offset;
    configuration.internal_client = read_u32(info + INTERNAL_CLIENT_FIELD_OFFSET);
    configuration.internal_subdevice = read_u32(info + INTERNAL_SUBDEVICE_FIELD_OFFSET);

    return OK;
}

int32_t read_static_configuration(
    gsp& conversation,
    uint64_t timeout_ns,
    uint64_t video_memory_size,
    static_configuration& configuration
) {
    uint8_t* info = static_cast<uint8_t*>(heap::uzalloc(STATIC_INFO_BYTES));
    if (info == nullptr) {
        log::error("nvidia: allocating %u bytes for the GSP firmware's static configuration failed", STATIC_INFO_BYTES);
        return ERR_NO_MEMORY;
    }

    received_message reply = {};
    int32_t rc = conversation.request(
        FUNCTION_GET_GSP_STATIC_INFO,
        info,
        STATIC_INFO_BYTES,
        STATIC_INFO_BYTES,
        reply,
        timeout_ns
    );
    if (rc == OK) {
        log::info(
            "nvidia: the GSP's firmware sends its static configuration in %u payload bytes, where NVIDIA's layout "
            "for this firmware takes %u",
            reply.payload_bytes,
            STATIC_INFO_BYTES
        );

        uint32_t info_bytes = reply.payload_bytes < STATIC_INFO_BYTES ? reply.payload_bytes : STATIC_INFO_BYTES;
        rc = read_configuration(info, info_bytes, video_memory_size, configuration);
    }

    heap::ufree(info);

    return rc;
}

} // namespace drivers::nvidia
