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
static constexpr size_t  EXTENSION_COUNT_OFFSET  = 126;
static constexpr size_t  CHECKSUM_OFFSET         = 127;

// An extension block's first byte is its tag, naming its kind
static constexpr size_t  EXTENSION_TAG_OFFSET    = 0;
static constexpr uint8_t EXTENSION_TAG_CTA_861   = 0x02;
static constexpr uint8_t EXTENSION_TAG_DISPLAYID = 0x70;

// A CTA-861 block's header is its first four bytes: its tag, its revision, the offset of its first timing descriptor
// and a byte the parser does not read
static constexpr size_t CTA_861_REVISION_OFFSET         = 1;
static constexpr size_t CTA_861_DESCRIPTOR_START_OFFSET = 2;
static constexpr size_t CTA_861_HEADER_BYTES            = 4;

// A DisplayID block holds one section from byte 1: its version byte, the byte count of its data blocks, two bytes the
// parser does not read, the data blocks, then its checksum byte, which must come before the block's own
static constexpr size_t DISPLAYID_VERSION_OFFSET    = 1;
static constexpr size_t DISPLAYID_DATA_BYTES_OFFSET = 2;
static constexpr size_t DISPLAYID_DATA_OFFSET       = 5;
static constexpr size_t DISPLAYID_DATA_BYTES_MAX    = CHECKSUM_OFFSET - 1 - DISPLAYID_DATA_OFFSET;

// The high nibble of a section's version byte is its version and the low nibble its revision
static constexpr uint8_t DISPLAYID_VERSION_1 = 0x1;
static constexpr uint8_t DISPLAYID_VERSION_2 = 0x2;

// A DisplayID data block is a tag, a revision, its length and that many bytes
static constexpr size_t  DISPLAYID_DATA_BLOCK_HEADER_BYTES    = 3;
static constexpr size_t  DISPLAYID_DATA_BLOCK_TAG_OFFSET      = 0;
static constexpr size_t  DISPLAYID_DATA_BLOCK_REVISION_OFFSET = 1;
static constexpr size_t  DISPLAYID_DATA_BLOCK_LENGTH_OFFSET   = 2;
static constexpr uint8_t DISPLAYID_TAG_TYPE_I_TIMINGS         = 0x03;

// A Type VII block holds descriptors laid out as Type I's with the clock in kHz. The parser passes over one whose
// revision byte sets bit 3 (from revision 1, timings only a compressed stream can carry) or any of bits 6 to 4
static constexpr uint8_t  DISPLAYID_TAG_TYPE_VII_TIMINGS     = 0x22;
static constexpr uint8_t  TYPE_VII_UNSUPPORTED_REVISION_BITS = 0x78;
static constexpr uint32_t KHZ_PER_TYPE_VII_CLOCK_UNIT        = 1;

// A tag 0 block is zero padding unless it comes first and holds bytes, as a version 1 section's product identification
// block does. In each section the EDID parse in NVIDIA's open kernel modules walks, such padding ends its walk
static constexpr uint8_t DISPLAYID_TAG_PRODUCT_ID = 0x00;

// A Type I timing block holds 20-byte descriptors: the pixel clock in 10 kHz units in 3 bytes, an options byte, then 8
// bytes for each axis, every count and the clock stored minus 1 with its low byte first
static constexpr size_t  TYPE_I_DESCRIPTOR_BYTES  = 20;
static constexpr size_t  TYPE_I_CLOCK_OFFSET      = 0;
static constexpr size_t  TYPE_I_CLOCK_BYTES       = 3;
static constexpr size_t  TYPE_I_OPTIONS_OFFSET    = 3;
static constexpr size_t  TYPE_I_HORIZONTAL_OFFSET = 4;
static constexpr size_t  TYPE_I_VERTICAL_OFFSET   = 12;
static constexpr uint8_t TYPE_I_OPTION_INTERLACED = 0x10;

// Each axis of a Type I descriptor holds its visible count, blanking, front porch and sync width in turn, 2 bytes each,
// and the top bit of the front porch's 2 bytes is the sync pulse's polarity, 1 meaning positive
static constexpr size_t   AXIS_FIELD_BYTES        = 2;
static constexpr size_t   AXIS_VISIBLE_OFFSET     = 0;
static constexpr size_t   AXIS_BLANKING_OFFSET    = 2;
static constexpr size_t   AXIS_FRONT_PORCH_OFFSET = 4;
static constexpr size_t   AXIS_SYNC_WIDTH_OFFSET  = 6;
static constexpr uint32_t AXIS_FRONT_PORCH_MASK   = 0x7FFF;
static constexpr uint32_t AXIS_SYNC_POSITIVE      = 0x8000;

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

namespace {

// One axis of a timing as a descriptor gives it, the blanking in place of the back porch
struct timing_axis {
    uint32_t visible;
    uint32_t blanking;
    uint32_t front_porch;
    uint32_t sync_width;
    bool     sync_positive;
};

// What the walk reads in a section of one DisplayID version: the timing blocks' tag, the revision bits that make it
// pass one over, and the kHz in a unit of their descriptors' pixel clock
struct displayid_timing_blocks {
    uint8_t  tag;
    uint8_t  unsupported_revision_bits;
    uint32_t khz_per_clock_unit;
};

} // namespace

// A version 1 section's Type I blocks, read at any revision, and a version 2 section's Type VII blocks
static constexpr displayid_timing_blocks TYPE_I_BLOCKS = {DISPLAYID_TAG_TYPE_I_TIMINGS, 0, KHZ_PER_CLOCK_UNIT};
static constexpr displayid_timing_blocks TYPE_VII_BLOCKS = {
    DISPLAYID_TAG_TYPE_VII_TIMINGS, TYPE_VII_UNSUPPORTED_REVISION_BITS, KHZ_PER_TYPE_VII_CLOCK_UNIT
};

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

static bool has_valid_checksum(const uint8_t* bytes, size_t count) {
    uint8_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }

    return sum == 0;
}

static int32_t check_base_block(const uint8_t* bytes, size_t size) {
    if (size < BLOCK_BYTES) {
        return ERR_TOO_SHORT;
    }

    if (string::memcmp(bytes, HEADER_PATTERN, sizeof(HEADER_PATTERN)) != 0) {
        return ERR_BAD_HEADER;
    }

    if (!has_valid_checksum(bytes, BLOCK_BYTES)) {
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

// Fills `out` from a pixel clock and two axes, or returns false when an axis's front porch and sync pulse are longer
// than its blanking
static bool fill_timing(uint32_t pixel_clock_khz, const timing_axis& h, const timing_axis& v, timing& out) {
    bool overruns_h_blanking = h.front_porch + h.sync_width > h.blanking;
    bool overruns_v_blanking = v.front_porch + v.sync_width > v.blanking;
    if (overruns_h_blanking || overruns_v_blanking) {
        return false;
    }

    out = {
        .pixel_clock_khz = pixel_clock_khz,
        .width           = h.visible,
        .h_front_porch   = h.front_porch,
        .h_sync_width    = h.sync_width,
        .h_back_porch    = h.blanking - h.front_porch - h.sync_width,
        .height          = v.visible,
        .v_front_porch   = v.front_porch,
        .v_sync_width    = v.sync_width,
        .v_back_porch    = v.blanking - v.front_porch - v.sync_width,
        .h_sync_positive = h.sync_positive,
        .v_sync_positive = v.sync_positive,
    };

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
    if (is_interlaced || has_border || has_no_visible_area) {
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

    timing_axis h = {width, h_blanking, h_front_porch, h_sync_width, h_sync_positive};
    timing_axis v = {height, v_blanking, v_front_porch, v_sync_width, v_sync_positive};

    return fill_timing(clock_units * KHZ_PER_CLOCK_UNIT, h, v, out);
}

// Lists `t` after the listed timings unless one equals it, counting it in `dropped_timings` when `timings` is full
static void list_timing(const timing& t, monitor& out) {
    for (uint32_t i = 0; i < out.timing_count; i++) {
        if (out.timings[i] == t) {
            return;
        }
    }

    if (out.timing_count == TIMINGS_MAX) {
        out.dropped_timings++;
        return;
    }

    out.timings[out.timing_count] = t;
    out.timing_count++;
}

// Lists the timing of a descriptor whose pixel clock is not 0, returning false when the descriptor gives none
static bool list_descriptor_timing(const uint8_t* descriptor, monitor& out) {
    if (is_unused_descriptor(descriptor)) {
        return false;
    }

    timing t = {};
    if (!read_timing_descriptor(descriptor, t)) {
        return false;
    }

    list_timing(t, out);

    return true;
}

static bool has_zero_clock(const uint8_t* descriptor) {
    return descriptor[TIMING_CLOCK_LOW] == 0 && descriptor[TIMING_CLOCK_HIGH] == 0;
}

static void read_descriptors(const uint8_t* block, monitor& out) {
    // The EDID code in NVIDIA's open kernel modules reads bit 1 as the preferred mark at every revision
    bool marks_first_preferred = (block[FEATURES_OFFSET] & FEATURE_FIRST_PREFERRED) != 0;
    bool has_name = false;

    for (size_t i = 0; i < BASE_DESCRIPTORS; i++) {
        const uint8_t* descriptor = block + DESCRIPTORS_OFFSET + i * DESCRIPTOR_BYTES;

        if (has_zero_clock(descriptor)) {
            bool is_reserved_clear = descriptor[DISPLAY_RESERVED_OFFSET] == 0;
            bool is_name = descriptor[DISPLAY_TAG_OFFSET] == DISPLAY_TAG_PRODUCT_NAME;
            if (!has_name && is_reserved_clear && is_name) {
                read_name(descriptor, out.name);
                has_name = true;
            }

            continue;
        }

        bool gives_timing = list_descriptor_timing(descriptor, out);
        if (i == 0 && gives_timing) {
            out.first_timing_is_preferred = marks_first_preferred;
        }
    }
}

// Reads no descriptor at a byte 2 of 1 to 3. Like the EDID parse in NVIDIA's open kernel modules, it reads none at
// revision 0 or a byte 2 of 0 and stops at the first whose pixel clock is 0 or that would not end before byte 127
static void read_cta_861_descriptors(const uint8_t* block, monitor& out) {
    bool has_revision = block[CTA_861_REVISION_OFFSET] != 0;
    size_t offset = block[CTA_861_DESCRIPTOR_START_OFFSET];
    bool starts_past_header = offset >= CTA_861_HEADER_BYTES;
    if (!has_revision || !starts_past_header) {
        return;
    }

    for (; offset + DESCRIPTOR_BYTES <= CHECKSUM_OFFSET; offset += DESCRIPTOR_BYTES) {
        const uint8_t* descriptor = block + offset;
        if (has_zero_clock(descriptor)) {
            return;
        }

        list_descriptor_timing(descriptor, out);
    }
}

// A section is valid when its checksum byte comes before the block's and its bytes sum to 0 modulo 256
static bool has_valid_displayid_section(const uint8_t* block) {
    size_t data_bytes = block[DISPLAYID_DATA_BYTES_OFFSET];
    if (data_bytes > DISPLAYID_DATA_BYTES_MAX) {
        return false;
    }

    size_t section_checksum_offset = DISPLAYID_DATA_OFFSET + data_bytes;
    size_t section_bytes = section_checksum_offset + 1 - DISPLAYID_VERSION_OFFSET;

    return has_valid_checksum(block + DISPLAYID_VERSION_OFFSET, section_bytes);
}

static uint32_t read_little_endian(const uint8_t* bytes, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; i++) {
        value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    }

    return value;
}

static timing_axis read_displayid_axis(const uint8_t* axis) {
    uint32_t front_porch_field = read_little_endian(axis + AXIS_FRONT_PORCH_OFFSET, AXIS_FIELD_BYTES);

    return {
        .visible       = read_little_endian(axis + AXIS_VISIBLE_OFFSET, AXIS_FIELD_BYTES) + 1,
        .blanking      = read_little_endian(axis + AXIS_BLANKING_OFFSET, AXIS_FIELD_BYTES) + 1,
        .front_porch   = (front_porch_field & AXIS_FRONT_PORCH_MASK) + 1,
        .sync_width    = read_little_endian(axis + AXIS_SYNC_WIDTH_OFFSET, AXIS_FIELD_BYTES) + 1,
        .sync_positive = (front_porch_field & AXIS_SYNC_POSITIVE) != 0,
    };
}

// Lists the timing of a Type I or Type VII descriptor unless it is interlaced or `fill_timing` refuses its axes
static void list_displayid_timing(const uint8_t* descriptor, uint32_t khz_per_clock_unit, monitor& out) {
    bool is_interlaced = (descriptor[TYPE_I_OPTIONS_OFFSET] & TYPE_I_OPTION_INTERLACED) != 0;
    if (is_interlaced) {
        return;
    }

    uint32_t clock_units = read_little_endian(descriptor + TYPE_I_CLOCK_OFFSET, TYPE_I_CLOCK_BYTES) + 1;
    timing_axis h = read_displayid_axis(descriptor + TYPE_I_HORIZONTAL_OFFSET);
    timing_axis v = read_displayid_axis(descriptor + TYPE_I_VERTICAL_OFFSET);

    timing t = {};
    if (fill_timing(clock_units * khz_per_clock_unit, h, v, t)) {
        list_timing(t, out);
    }
}

// Lists a section's timings. In each section the EDID parse in NVIDIA's open kernel modules walks, both stop at a data
// block not ending before the section's checksum byte and read no timing block whose length is not a multiple of 20
static void read_displayid_data_blocks(
    const uint8_t* data,
    size_t data_bytes,
    const displayid_timing_blocks& blocks,
    monitor& out
) {
    size_t offset = 0;
    while (offset + DISPLAYID_DATA_BLOCK_HEADER_BYTES <= data_bytes) {
        const uint8_t* data_block = data + offset;
        uint8_t tag = data_block[DISPLAYID_DATA_BLOCK_TAG_OFFSET];
        uint8_t revision = data_block[DISPLAYID_DATA_BLOCK_REVISION_OFFSET];
        size_t length = data_block[DISPLAYID_DATA_BLOCK_LENGTH_OFFSET];
        size_t end = offset + DISPLAYID_DATA_BLOCK_HEADER_BYTES + length;

        bool is_first_holding_bytes = offset == 0 && tag == DISPLAYID_TAG_PRODUCT_ID && length != 0;
        bool is_padding = tag == DISPLAYID_TAG_PRODUCT_ID && !is_first_holding_bytes;
        if (is_padding || end > data_bytes) {
            return;
        }

        bool is_timing_block = tag == blocks.tag;
        bool has_unsupported_bits = (revision & blocks.unsupported_revision_bits) != 0;
        bool is_whole = length % TYPE_I_DESCRIPTOR_BYTES == 0;
        if (is_timing_block && !has_unsupported_bits && is_whole) {
            const uint8_t* descriptors = data_block + DISPLAYID_DATA_BLOCK_HEADER_BYTES;
            for (size_t i = 0; i < length; i += TYPE_I_DESCRIPTOR_BYTES) {
                list_displayid_timing(descriptors + i, blocks.khz_per_clock_unit, out);
            }
        }

        offset = end;
    }
}

// Lists the timings of a DisplayID block's section when its version is 1 or 2. The section must have passed
// `has_valid_displayid_section`, whose length check keeps the walk inside the block
static void read_displayid_section(const uint8_t* block, monitor& out) {
    const uint8_t* data = block + DISPLAYID_DATA_OFFSET;
    size_t data_bytes = block[DISPLAYID_DATA_BYTES_OFFSET];

    uint8_t version = block[DISPLAYID_VERSION_OFFSET] >> 4;
    if (version == DISPLAYID_VERSION_1) {
        read_displayid_data_blocks(data, data_bytes, TYPE_I_BLOCKS, out);
    } else if (version == DISPLAYID_VERSION_2) {
        read_displayid_data_blocks(data, data_bytes, TYPE_VII_BLOCKS, out);
    }
}

// Lists the timings of the blocks byte 126 counts, skipping and counting each extension block `bytes` does not hold
// whole, each CTA-861 or DisplayID one failing its checksum and each DisplayID one whose section fails
static void read_extensions(const uint8_t* bytes, size_t size, monitor& out) {
    size_t extension_count = bytes[EXTENSION_COUNT_OFFSET];
    size_t whole_blocks = size / BLOCK_BYTES;

    for (size_t i = 1; i <= extension_count; i++) {
        if (i >= whole_blocks) {
            out.skipped_extensions++;
            continue;
        }

        const uint8_t* block = bytes + i * BLOCK_BYTES;
        uint8_t tag = block[EXTENSION_TAG_OFFSET];
        bool is_checked_kind = tag == EXTENSION_TAG_CTA_861 || tag == EXTENSION_TAG_DISPLAYID;
        if (is_checked_kind && !has_valid_checksum(block, BLOCK_BYTES)) {
            out.skipped_extensions++;
            continue;
        }

        if (tag == EXTENSION_TAG_DISPLAYID && !has_valid_displayid_section(block)) {
            out.skipped_extensions++;
            continue;
        }

        if (tag == EXTENSION_TAG_CTA_861) {
            read_cta_861_descriptors(block, out);
        } else if (tag == EXTENSION_TAG_DISPLAYID) {
            read_displayid_section(block, out);
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
    read_extensions(bytes, size, out);

    return OK;
}

} // namespace edid
