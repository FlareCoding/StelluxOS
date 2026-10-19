#include "drivers/graphics/edid.h"
#include "common/string.h"

namespace edid {

static constexpr uint8_t HEADER_PATTERN[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

static constexpr size_t  MAKER_OFFSET            = 8;
static constexpr size_t  VERSION_OFFSET          = 18;
static constexpr uint8_t BASE_BLOCK_VERSION      = 1;
static constexpr size_t  FEATURES_OFFSET         = 24;
static constexpr uint8_t FEATURE_FIRST_PREFERRED = 0x02;
static constexpr size_t  DESCRIPTORS_OFFSET      = 54;
static constexpr size_t  DESCRIPTOR_BYTES        = 18;
static constexpr size_t  BASE_DESCRIPTORS        = 4;

// The maker's id is a big-endian word of three 5-bit letters in bits 14 to 0, the first highest, 1 meaning A
static constexpr uint32_t MAKER_LETTERS     = 3;
static constexpr uint32_t MAKER_LETTER_BITS = 5;
static constexpr uint16_t MAKER_LETTER_MASK = 0x1F;
static constexpr uint16_t MAKER_LETTER_Z    = 26;

// A descriptor with a pixel clock of 0 is a display descriptor, whose kind is its byte 3, and the EDID code in NVIDIA's
// open kernel modules parses none whose reserved byte 2 is set
static constexpr size_t  DISPLAY_RESERVED_OFFSET  = 2;
static constexpr size_t  DISPLAY_TAG_OFFSET       = 3;
static constexpr uint8_t DISPLAY_TAG_PRODUCT_NAME = 0xFC;
static constexpr size_t  NAME_OFFSET              = 5;
static constexpr size_t  NAME_BYTES               = 13;

// The EDID code in NVIDIA's open kernel modules parses a descriptor starting with five 0x01 bytes as unused
static constexpr uint8_t UNUSED_FILL       = 0x01;
static constexpr size_t  UNUSED_FILL_BYTES = 5;

// Past the clock, a count wider than its byte or nibble keeps its high bits in a byte with other counts' high bits
static constexpr size_t TIMING_CLOCK_LOW         = 0;  // the pixel clock in 10 kHz units, low byte first
static constexpr size_t TIMING_CLOCK_HIGH        = 1;
static constexpr size_t TIMING_WIDTH_LOW         = 2;
static constexpr size_t TIMING_H_BLANKING_LOW    = 3;
static constexpr size_t TIMING_H_HIGH_BITS       = 4;  // the width's high 4 bits above the blanking's
static constexpr size_t TIMING_HEIGHT_LOW        = 5;
static constexpr size_t TIMING_V_BLANKING_LOW    = 6;
static constexpr size_t TIMING_V_HIGH_BITS       = 7;  // the height's high 4 bits above the blanking's
static constexpr size_t TIMING_H_FRONT_PORCH_LOW = 8;
static constexpr size_t TIMING_H_SYNC_WIDTH_LOW  = 9;
static constexpr size_t TIMING_V_PORCH_SYNC_LOW  = 10; // the vertical front porch's low 4 bits above the sync's
static constexpr size_t TIMING_PORCH_SYNC_HIGH   = 11; // 2 high bits each: h front porch, h sync, v front porch, v sync
static constexpr size_t TIMING_H_BORDER          = 15;
static constexpr size_t TIMING_V_BORDER          = 16;
static constexpr size_t TIMING_FLAGS             = 17;

static constexpr uint8_t FLAG_INTERLACED        = 0x80;
static constexpr uint8_t SYNC_KIND_MASK         = 0x18;
static constexpr uint8_t SYNC_DIGITAL_SEPARATE  = 0x18;
static constexpr uint8_t SYNC_DIGITAL_COMPOSITE = 0x10;
static constexpr uint8_t FLAG_H_SYNC_POSITIVE   = 0x02;
static constexpr uint8_t FLAG_V_SYNC_POSITIVE   = 0x04;

static constexpr uint32_t KHZ_PER_CLOCK_UNIT = 10;
static constexpr uint64_t MILLIHERTZ_PER_KHZ = 1000000;

static_assert(BASE_DESCRIPTORS <= TIMINGS_MAX, "every timing the base block gives fits in `monitor::timings`");

uint32_t timing::h_total() const {
    return width + h_front_porch + h_sync_width + h_back_porch;
}

uint32_t timing::v_total() const {
    return height + v_front_porch + v_sync_width + v_back_porch;
}

uint64_t timing::refresh_millihertz() const {
    uint64_t frame_pixels = static_cast<uint64_t>(h_total()) * v_total();
    if (frame_pixels == 0) {
        return 0;
    }

    return pixel_clock_khz * MILLIHERTZ_PER_KHZ / frame_pixels;
}

static int32_t check_base_block(const uint8_t* bytes, size_t size) {
    if (size < BLOCK_BYTES) {
        return ERR_TOO_SHORT;
    }

    if (string::memcmp(bytes, HEADER_PATTERN, sizeof(HEADER_PATTERN)) != 0) {
        return ERR_BAD_HEADER;
    }

    uint8_t sum = 0;
    for (size_t i = 0; i < BLOCK_BYTES; i++) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }

    if (sum != 0) {
        return ERR_BAD_CHECKSUM;
    }

    if (bytes[VERSION_OFFSET] != BASE_BLOCK_VERSION) {
        return ERR_BAD_VERSION;
    }

    return OK;
}

static void read_maker(const uint8_t* block, char* maker) {
    uint16_t word = static_cast<uint16_t>((block[MAKER_OFFSET] << 8) | block[MAKER_OFFSET + 1]);

    for (uint32_t i = 0; i < MAKER_LETTERS; i++) {
        uint32_t shift = MAKER_LETTER_BITS * (MAKER_LETTERS - 1 - i);
        uint16_t letter = (word >> shift) & MAKER_LETTER_MASK;
        bool is_letter = letter >= 1 && letter <= MAKER_LETTER_Z;

        maker[i] = is_letter ? static_cast<char>('A' + letter - 1) : '?';
    }
}

// The name is at most 13 characters, stopping before a newline or a NUL, without its trailing spaces
static void read_name(const uint8_t* descriptor, char* name) {
    size_t length = 0;
    while (length < NAME_BYTES) {
        uint8_t c = descriptor[NAME_OFFSET + length];
        if (c == '\n' || c == '\0') {
            break;
        }

        bool is_printable = c >= ' ' && c <= '~';
        name[length] = is_printable ? static_cast<char>(c) : '?';
        length++;
    }

    while (length > 0 && name[length - 1] == ' ') {
        length--;
        name[length] = '\0';
    }
}

static bool is_unused_descriptor(const uint8_t* descriptor) {
    for (size_t i = 0; i < UNUSED_FILL_BYTES; i++) {
        if (descriptor[i] != UNUSED_FILL) {
            return false;
        }
    }

    return true;
}

// Reads a timing descriptor into `out`, or returns false when it has no visible area or `timing` cannot describe it
static bool read_timing_descriptor(const uint8_t* descriptor, timing& out) {
    uint32_t clock_units = descriptor[TIMING_CLOCK_LOW] | (descriptor[TIMING_CLOCK_HIGH] << 8);
    uint8_t porch_sync_high = descriptor[TIMING_PORCH_SYNC_HIGH];
    uint8_t flags = descriptor[TIMING_FLAGS];

    uint32_t width = descriptor[TIMING_WIDTH_LOW] | ((descriptor[TIMING_H_HIGH_BITS] & 0xF0) << 4);
    uint32_t h_blanking = descriptor[TIMING_H_BLANKING_LOW] | ((descriptor[TIMING_H_HIGH_BITS] & 0x0F) << 8);
    uint32_t h_front_porch = descriptor[TIMING_H_FRONT_PORCH_LOW] | ((porch_sync_high & 0xC0) << 2);
    uint32_t h_sync_width = descriptor[TIMING_H_SYNC_WIDTH_LOW] | ((porch_sync_high & 0x30) << 4);

    uint32_t height = descriptor[TIMING_HEIGHT_LOW] | ((descriptor[TIMING_V_HIGH_BITS] & 0xF0) << 4);
    uint32_t v_blanking = descriptor[TIMING_V_BLANKING_LOW] | ((descriptor[TIMING_V_HIGH_BITS] & 0x0F) << 8);
    uint32_t v_front_porch = (descriptor[TIMING_V_PORCH_SYNC_LOW] >> 4) | ((porch_sync_high & 0x0C) << 2);
    uint32_t v_sync_width = (descriptor[TIMING_V_PORCH_SYNC_LOW] & 0x0F) | ((porch_sync_high & 0x03) << 4);

    bool is_interlaced = (flags & FLAG_INTERLACED) != 0;
    bool has_border = descriptor[TIMING_H_BORDER] != 0 || descriptor[TIMING_V_BORDER] != 0;
    bool has_no_visible_area = width == 0 || height == 0;
    bool overruns_h_blanking = h_front_porch + h_sync_width > h_blanking;
    bool overruns_v_blanking = v_front_porch + v_sync_width > v_blanking;
    if (is_interlaced || has_border || has_no_visible_area || overruns_h_blanking || overruns_v_blanking) {
        return false;
    }

    // Digital composite sync gives no vertical polarity and analog sync gives neither, so a missing polarity reads as
    // the EDID code in NVIDIA's open kernel modules assigns it, horizontal negative and vertical positive
    uint8_t sync_kind = flags & SYNC_KIND_MASK;
    bool h_sync_positive = false;
    bool v_sync_positive = true;
    if (sync_kind == SYNC_DIGITAL_SEPARATE) {
        h_sync_positive = (flags & FLAG_H_SYNC_POSITIVE) != 0;
        v_sync_positive = (flags & FLAG_V_SYNC_POSITIVE) != 0;
    } else if (sync_kind == SYNC_DIGITAL_COMPOSITE) {
        h_sync_positive = (flags & FLAG_H_SYNC_POSITIVE) != 0;
    }

    out = {
        .pixel_clock_khz = clock_units * KHZ_PER_CLOCK_UNIT,
        .width           = width,
        .h_front_porch   = h_front_porch,
        .h_sync_width    = h_sync_width,
        .h_back_porch    = h_blanking - h_front_porch - h_sync_width,
        .height          = height,
        .v_front_porch   = v_front_porch,
        .v_sync_width    = v_sync_width,
        .v_back_porch    = v_blanking - v_front_porch - v_sync_width,
        .h_sync_positive = h_sync_positive,
        .v_sync_positive = v_sync_positive,
    };

    return true;
}

// Lists `t` after the listed timings unless one equals it. The static_assert above keeps the write inside `timings`
static void list_timing(const timing& t, monitor& out) {
    for (uint32_t i = 0; i < out.timing_count; i++) {
        if (out.timings[i] == t) {
            return;
        }
    }

    out.timings[out.timing_count] = t;
    out.timing_count++;
}

static void read_descriptors(const uint8_t* block, monitor& out) {
    // The EDID code in NVIDIA's open kernel modules reads bit 1 as the preferred mark at every revision
    bool marks_first_preferred = (block[FEATURES_OFFSET] & FEATURE_FIRST_PREFERRED) != 0;
    bool has_name = false;

    for (size_t i = 0; i < BASE_DESCRIPTORS; i++) {
        const uint8_t* descriptor = block + DESCRIPTORS_OFFSET + i * DESCRIPTOR_BYTES;
        bool is_display_descriptor = descriptor[TIMING_CLOCK_LOW] == 0 && descriptor[TIMING_CLOCK_HIGH] == 0;

        if (is_display_descriptor) {
            bool is_reserved_clear = descriptor[DISPLAY_RESERVED_OFFSET] == 0;
            bool is_name = descriptor[DISPLAY_TAG_OFFSET] == DISPLAY_TAG_PRODUCT_NAME;
            if (!has_name && is_reserved_clear && is_name) {
                read_name(descriptor, out.name);
                has_name = true;
            }

            continue;
        }

        if (is_unused_descriptor(descriptor)) {
            continue;
        }

        timing t = {};
        if (!read_timing_descriptor(descriptor, t)) {
            continue;
        }

        list_timing(t, out);
        if (i == 0) {
            out.first_timing_is_preferred = marks_first_preferred;
        }
    }
}

int32_t parse(const uint8_t* bytes, size_t size, monitor& out) {
    string::memset(&out, 0, sizeof(out));

    int32_t result = check_base_block(bytes, size);
    if (result != OK) {
        return result;
    }

    read_maker(bytes, out.maker);
    read_descriptors(bytes, out);

    return OK;
}

} // namespace edid
