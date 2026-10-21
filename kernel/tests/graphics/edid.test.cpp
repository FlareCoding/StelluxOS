#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/edid.h"

TEST_SUITE(edid);

// The AOC AG241QG4's EDID as it arrives, 384 bytes with byte 126 counting one extension: the base block with its serial
// number (bytes 12 to 15) zeroed and its checksum recomputed, then a CTA-861 block, then a third block all zero
static constexpr uint8_t AOC_AG241QG4_EDID[3 * edid::BLOCK_BYTES] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x05, 0xE3, 0x10, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x26, 0x1C, 0x01, 0x04, 0xA5, 0x35, 0x28, 0x78, 0x3B, 0xE1, 0x61, 0xA4, 0x57, 0x53, 0xA0, 0x27,
    0x0D, 0x50, 0x54, 0xBF, 0xEF, 0x00, 0xD1, 0xC0, 0x81, 0x80, 0x31, 0x68, 0x31, 0x7C, 0x45, 0x68,
    0x45, 0x7C, 0x61, 0x68, 0x61, 0x7C, 0x56, 0x5E, 0x00, 0xA0, 0xA0, 0xA0, 0x29, 0x50, 0x30, 0x20,
    0x35, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E, 0x40, 0xE7, 0x00, 0x6A, 0xA0, 0xA0, 0x67, 0x50,
    0x08, 0x20, 0x98, 0x04, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x1E,
    0x92, 0xE6, 0xE6, 0x3C, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,
    0x00, 0x41, 0x47, 0x32, 0x34, 0x31, 0x51, 0x47, 0x34, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x01, 0xCC,
    0x02, 0x03, 0x1E, 0xF1, 0x4B, 0x01, 0x03, 0x05, 0x14, 0x04, 0x13, 0x1F, 0x12, 0x02, 0x11, 0x90,
    0x23, 0x09, 0x07, 0x07, 0x83, 0x01, 0x00, 0x00, 0x65, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x93, 0xBE,
    0x00, 0x6A, 0xA0, 0xA0, 0x55, 0x50, 0x08, 0x20, 0x98, 0x04, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1E,
    0x40, 0x9D, 0x00, 0x6A, 0xA0, 0xA0, 0x46, 0x50, 0x08, 0x20, 0x98, 0x04, 0x0F, 0x8C, 0x21, 0x00,
    0x00, 0x1E, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x0F, 0x28,
    0x21, 0x00, 0x00, 0x1E, 0xAB, 0x22, 0xA0, 0xA0, 0x50, 0x84, 0x1A, 0x30, 0x30, 0x20, 0x36, 0x00,
    0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1A, 0xF0, 0x3C, 0x00, 0xD0, 0x51, 0xA0, 0x35, 0x50, 0x60, 0x88,
    0x3A, 0x00, 0x0F, 0x8C, 0x21, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42,
};

// The two timings of the AOC's base block, 2560 by 1440 at about 60 Hz and at about 144 Hz
static constexpr edid::timing AOC_60_HZ  = {241500, 2560, 48, 32, 80, 1440, 3, 5, 33, true, true};
static constexpr edid::timing AOC_144_HZ = {592000, 2560, 8, 32, 66, 1440, 25, 8, 70, true, false};

// Four of the five timings of the AOC's CTA-861 block: 2560 by 1440 at about 120 Hz and 100 Hz, then 1440 by 900 and
// 1280 by 1440 at about 60 Hz. The block's third timing is `FULL_HD_TIMING`
static constexpr edid::timing AOC_120_HZ       = {487870, 2560, 8, 32, 66, 1440, 25, 8, 52, true, true};
static constexpr edid::timing AOC_100_HZ       = {402560, 2560, 8, 32, 66, 1440, 25, 8, 37, true, true};
static constexpr edid::timing AOC_1440_BY_900  = {88750, 1440, 48, 32, 80, 900, 3, 6, 17, true, false};
static constexpr edid::timing AOC_1280_BY_1440 = {156000, 1280, 96, 136, 232, 1440, 3, 10, 40, false, true};

static constexpr size_t MAKER_OFFSET           = 8;
static constexpr size_t VERSION_OFFSET         = 18;
static constexpr size_t REVISION_OFFSET        = 19;
static constexpr size_t FEATURES_OFFSET        = 24;
static constexpr size_t DESCRIPTORS_OFFSET     = 54;
static constexpr size_t DESCRIPTOR_BYTES       = 18;
static constexpr size_t BASE_DESCRIPTORS       = 4;
static constexpr size_t EXTENSION_COUNT_OFFSET = 126;
static constexpr size_t CHECKSUM_OFFSET        = 127;

static constexpr size_t  DISPLAY_RESERVED_OFFSET  = 2;
static constexpr size_t  DISPLAY_TAG_OFFSET       = 3;
static constexpr uint8_t DISPLAY_TAG_PRODUCT_NAME = 0xFC;
static constexpr size_t  NAME_OFFSET              = 5;
static constexpr size_t  NAME_BYTES               = 13;

static constexpr size_t  EXTENSION_TAG_OFFSET    = 0;
static constexpr uint8_t EXTENSION_TAG_CTA_861   = 0x02;
static constexpr uint8_t EXTENSION_TAG_DISPLAYID = 0x70;

static constexpr size_t  CTA_861_REVISION_OFFSET         = 1;
static constexpr size_t  CTA_861_DESCRIPTOR_START_OFFSET = 2;
static constexpr size_t  CTA_861_UNREAD_OFFSET           = 3;
static constexpr uint8_t CTA_861_HEADER_BYTES            = 4;
static constexpr size_t  CTA_861_DESCRIPTORS_MAX         = 6;
static constexpr uint8_t CTA_861_REVISION_3              = 3;

static constexpr size_t  DISPLAYID_VERSION_OFFSET            = 1;
static constexpr size_t  DISPLAYID_DATA_BYTES_OFFSET         = 2;
static constexpr size_t  DISPLAYID_PRODUCT_TYPE_OFFSET       = 3;
static constexpr size_t  DISPLAYID_EXTENSION_SECTIONS_OFFSET = 4;
static constexpr size_t  DISPLAYID_DATA_OFFSET               = 5;
static constexpr uint8_t DISPLAYID_DATA_BYTES_MAX            = 121;

// The AOC's features byte, and the same with its preferred bit (bit 1) clear
static constexpr uint8_t AOC_FEATURES             = 0x3B;
static constexpr uint8_t AOC_FEATURES_BIT_1_CLEAR = 0x39;

// A timing descriptor's fields as the EDID stores them, the blanking in place of the back porch
struct timing_descriptor_fields {
    uint32_t clock_units; // 10 kHz each
    uint32_t width;
    uint32_t h_blanking;
    uint32_t h_front_porch;
    uint32_t h_sync_width;
    uint32_t height;
    uint32_t v_blanking;
    uint32_t v_front_porch;
    uint32_t v_sync_width;
    uint8_t  h_border;
    uint8_t  v_border;
    uint8_t  flags;
};

// A sync flags byte and the polarities the parser gives for it
struct sync_polarity_case {
    uint8_t flags;
    bool    h_sync_positive;
    bool    v_sync_positive;
};

// A product name descriptor's 13 text bytes and the name the parser reads from them
struct name_case {
    char text[NAME_BYTES + 1];
    char name[NAME_BYTES + 1];
};

// One byte of an unused descriptor's fill and the value it changes to
struct fill_change {
    size_t  offset;
    uint8_t value;
};

// 1920 by 1080 at 60 Hz with separate digital sync, both pulses positive
static constexpr timing_descriptor_fields FULL_HD        = {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E};
static constexpr edid::timing             FULL_HD_TIMING = {148500, 1920, 88, 44, 148, 1080, 4, 5, 36, true, true};

static uint8_t       g_block[edid::BLOCK_BYTES];
static edid::monitor g_monitor;

// A base block, then room for the 255 extension blocks byte 126 can give, so every byte read past `size` is in `g_edid`
static uint8_t g_edid[256 * edid::BLOCK_BYTES];

// Sets the last of `count` bytes so the bytes sum to 0 modulo 256
static void write_checksum(uint8_t* bytes, size_t count) {
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }

    bytes[count - 1] = static_cast<uint8_t>(0x100 - sum);
}

// Sets the checksum byte so the block's bytes sum to 0 modulo 256
static void seal(uint8_t* block) {
    write_checksum(block, edid::BLOCK_BYTES);
}

static void seal_block() {
    seal(g_block);
}

// The AOC's base block with `features` as its features byte, no extensions and every descriptor a display descriptor of
// no kind the parser reads
static void make_base_block(uint8_t features) {
    string::memcpy(g_block, AOC_AG241QG4_EDID, sizeof(g_block));
    string::memset(g_block + DESCRIPTORS_OFFSET, 0, BASE_DESCRIPTORS * DESCRIPTOR_BYTES);
    g_block[FEATURES_OFFSET] = features;
    g_block[EXTENSION_COUNT_OFFSET] = 0;
    seal_block();
}

static uint8_t* descriptor_at(size_t index) {
    return g_block + DESCRIPTORS_OFFSET + index * DESCRIPTOR_BYTES;
}

// Writes `f` as a timing descriptor at `d`, leaving the checksum of the block holding it to the caller
static void encode_timing_descriptor(uint8_t* d, const timing_descriptor_fields& f) {
    string::memset(d, 0, DESCRIPTOR_BYTES);

    d[0] = static_cast<uint8_t>(f.clock_units);
    d[1] = static_cast<uint8_t>(f.clock_units >> 8);
    d[2] = static_cast<uint8_t>(f.width);
    d[3] = static_cast<uint8_t>(f.h_blanking);
    d[4] = static_cast<uint8_t>((f.width >> 8) << 4 | f.h_blanking >> 8);
    d[5] = static_cast<uint8_t>(f.height);
    d[6] = static_cast<uint8_t>(f.v_blanking);
    d[7] = static_cast<uint8_t>((f.height >> 8) << 4 | f.v_blanking >> 8);
    d[8] = static_cast<uint8_t>(f.h_front_porch);
    d[9] = static_cast<uint8_t>(f.h_sync_width);
    d[10] = static_cast<uint8_t>((f.v_front_porch & 0x0F) << 4 | (f.v_sync_width & 0x0F));
    d[11] = static_cast<uint8_t>(
        (f.h_front_porch >> 8) << 6 | (f.h_sync_width >> 8) << 4 | (f.v_front_porch >> 4) << 2 | f.v_sync_width >> 4
    );
    d[15] = f.h_border;
    d[16] = f.v_border;
    d[17] = f.flags;
}

static void write_timing_descriptor(size_t index, const timing_descriptor_fields& f) {
    encode_timing_descriptor(descriptor_at(index), f);
    seal_block();
}

static void write_name_descriptor(size_t index, const char (&text)[NAME_BYTES + 1]) {
    uint8_t* d = descriptor_at(index);
    string::memset(d, 0, DESCRIPTOR_BYTES);

    d[DISPLAY_TAG_OFFSET] = DISPLAY_TAG_PRODUCT_NAME;
    string::memcpy(d + NAME_OFFSET, text, NAME_BYTES);
    seal_block();
}

static int32_t parse_block() {
    return edid::parse(g_block, sizeof(g_block), g_monitor);
}

// Copies `g_block` into `g_edid` as its base block and zeroes every byte after it
static void copy_block_into_edid() {
    string::memset(g_edid, 0, sizeof(g_edid));
    string::memcpy(g_edid, g_block, edid::BLOCK_BYTES);
}

static uint8_t* block_at(size_t index) {
    return g_edid + index * edid::BLOCK_BYTES;
}

// Sets byte 126 of `g_edid`'s base block to `count`, keeping the block's checksum right
static void set_extension_count(uint8_t count) {
    g_edid[EXTENSION_COUNT_OFFSET] = count;
    seal(g_edid);
}

// Writes block `index` of `g_edid` as an extension block of kind `tag` holding nothing else, its checksum right
static void write_extension(size_t index, uint8_t tag) {
    uint8_t* block = block_at(index);
    string::memset(block, 0, edid::BLOCK_BYTES);

    block[EXTENSION_TAG_OFFSET] = tag;
    seal(block);
}

// The offset of descriptor `index` in a CTA-861 block whose descriptors start at byte 4
static size_t cta_861_descriptor_offset(size_t index) {
    return CTA_861_HEADER_BYTES + index * DESCRIPTOR_BYTES;
}

// Writes block `index` of `g_edid` as an empty CTA-861 block at revision 3 whose descriptors would start at byte 4
static void write_cta_861_block(size_t index) {
    write_extension(index, EXTENSION_TAG_CTA_861);
    block_at(index)[CTA_861_REVISION_OFFSET] = CTA_861_REVISION_3;
    block_at(index)[CTA_861_DESCRIPTOR_START_OFFSET] = CTA_861_HEADER_BYTES;
    seal(block_at(index));
}

// Writes `f` as a timing descriptor at byte `offset` of block `index` of `g_edid`, keeping the block's checksum right
static void write_extension_descriptor(size_t index, size_t offset, const timing_descriptor_fields& f) {
    encode_timing_descriptor(block_at(index) + offset, f);
    seal(block_at(index));
}

// Lays out `g_edid` as a base block holding no timing, then one CTA-861 block at `revision` whose descriptors start at
// byte `start` with `FULL_HD`, then writes the block's tag, revision and byte 2 over any descriptor bytes they share
static void write_cta_861_edid(uint8_t revision, uint8_t start) {
    make_base_block(AOC_FEATURES);
    copy_block_into_edid();
    set_extension_count(1);

    uint8_t* block = block_at(1);
    encode_timing_descriptor(block + start, FULL_HD);
    block[EXTENSION_TAG_OFFSET] = EXTENSION_TAG_CTA_861;
    block[CTA_861_REVISION_OFFSET] = revision;
    block[CTA_861_DESCRIPTOR_START_OFFSET] = start;
    seal(block);
}

// Writes the checksum byte after the data blocks of DisplayID block `block` so the section's bytes sum to 0 modulo 256,
// past the block when its section's data blocks take more than 122 bytes
static void write_section_checksum(uint8_t* block) {
    size_t section_bytes = DISPLAYID_DATA_OFFSET + block[DISPLAYID_DATA_BYTES_OFFSET] + 1 - DISPLAYID_VERSION_OFFSET;
    write_checksum(block + DISPLAYID_VERSION_OFFSET, section_bytes);
}

// Sets both checksums of block `index` of `g_edid`, a DisplayID block whose section ends before byte 127
static void seal_displayid_block(size_t index) {
    write_section_checksum(block_at(index));
    seal(block_at(index));
}

// Writes block `index` of `g_edid` as a DisplayID block whose section, at `version`, holds no data block
static void write_displayid_block(size_t index, uint8_t version) {
    write_extension(index, EXTENSION_TAG_DISPLAYID);
    block_at(index)[DISPLAYID_VERSION_OFFSET] = version;
    seal_displayid_block(index);
}

static int32_t parse_edid(size_t size) {
    return edid::parse(g_edid, size, g_monitor);
}

static void expect_timing(const edid::timing& actual, const edid::timing& expected) {
    EXPECT_EQ(actual.pixel_clock_khz, expected.pixel_clock_khz);
    EXPECT_EQ(actual.width, expected.width);
    EXPECT_EQ(actual.h_front_porch, expected.h_front_porch);
    EXPECT_EQ(actual.h_sync_width, expected.h_sync_width);
    EXPECT_EQ(actual.h_back_porch, expected.h_back_porch);
    EXPECT_EQ(actual.height, expected.height);
    EXPECT_EQ(actual.v_front_porch, expected.v_front_porch);
    EXPECT_EQ(actual.v_sync_width, expected.v_sync_width);
    EXPECT_EQ(actual.v_back_porch, expected.v_back_porch);
    EXPECT_EQ(actual.h_sync_positive, expected.h_sync_positive);
    EXPECT_EQ(actual.v_sync_positive, expected.v_sync_positive);
}

static bool is_zeroed(const edid::monitor& m) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&m);
    for (size_t i = 0; i < sizeof(m); i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }

    return true;
}

// Parses `g_block`'s first `size` bytes over a good parse in `g_monitor`, expecting `error` and `g_monitor` cleared
static void expect_refused(size_t size, int32_t error) {
    ASSERT_EQ(edid::parse(AOC_AG241QG4_EDID, sizeof(AOC_AG241QG4_EDID), g_monitor), edid::OK);

    EXPECT_EQ(edid::parse(g_block, size, g_monitor), error);
    EXPECT_TRUE(is_zeroed(g_monitor));
}

TEST(edid, reads_the_aoc_ag241qg4_edid) {
    ASSERT_EQ(edid::parse(AOC_AG241QG4_EDID, sizeof(AOC_AG241QG4_EDID), g_monitor), edid::OK);

    EXPECT_STREQ(g_monitor.maker, "AOC");
    EXPECT_STREQ(g_monitor.name, "AG241QG4");
    ASSERT_EQ(g_monitor.timing_count, 7u);
    expect_timing(g_monitor.timings[0], AOC_60_HZ);
    expect_timing(g_monitor.timings[1], AOC_144_HZ);
    expect_timing(g_monitor.timings[2], AOC_120_HZ);
    expect_timing(g_monitor.timings[3], AOC_100_HZ);
    expect_timing(g_monitor.timings[4], FULL_HD_TIMING);
    expect_timing(g_monitor.timings[5], AOC_1440_BY_900);
    expect_timing(g_monitor.timings[6], AOC_1280_BY_1440);
    EXPECT_EQ(g_monitor.timings[0].h_total(), 2720u);
    EXPECT_EQ(g_monitor.timings[0].v_total(), 1481u);
    EXPECT_EQ(g_monitor.timings[1].h_total(), 2666u);
    EXPECT_EQ(g_monitor.timings[1].v_total(), 1543u);
    EXPECT_EQ(g_monitor.timings[0].refresh_millihertz(), 59950ull);
    EXPECT_EQ(g_monitor.timings[1].refresh_millihertz(), 143911ull);
    EXPECT_EQ(g_monitor.timings[2].refresh_millihertz(), 119998ull);
    EXPECT_EQ(g_monitor.timings[3].refresh_millihertz(), 99998ull);
    EXPECT_EQ(g_monitor.timings[4].refresh_millihertz(), 60000ull);
    EXPECT_EQ(g_monitor.timings[5].refresh_millihertz(), 59901ull);
    EXPECT_EQ(g_monitor.timings[6].refresh_millihertz(), 59912ull);
    EXPECT_TRUE(g_monitor.first_timing_is_preferred);
    EXPECT_EQ(g_monitor.dropped_timings, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);

    // A parse of the base block alone lists its two timings and counts the CTA-861 block it is not given as skipped
    ASSERT_EQ(edid::parse(AOC_AG241QG4_EDID, edid::BLOCK_BYTES, g_monitor), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 2u);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);
}

TEST(edid, refuses_fewer_bytes_than_one_block) {
    make_base_block(AOC_FEATURES);

    expect_refused(edid::BLOCK_BYTES - 1, edid::ERR_TOO_SHORT);
    expect_refused(0, edid::ERR_TOO_SHORT);
}

// A base block, then the bytes a fifth descriptor at byte 126 would read as a timing, then 0xFF bytes
static uint8_t g_two_blocks[2 * edid::BLOCK_BYTES];

TEST(edid, ignores_bytes_past_a_base_block_whose_byte_126_is_0) {
    size_t fifth_descriptor_offset = DESCRIPTORS_OFFSET + BASE_DESCRIPTORS * DESCRIPTOR_BYTES;
    size_t bytes_inside = edid::BLOCK_BYTES - fifth_descriptor_offset;

    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, FULL_HD);

    // The checksum would be the fifth descriptor's clock high byte, nonzero so the descriptor reads as a timing
    ASSERT_NE(g_block[CHECKSUM_OFFSET], 0);

    string::memcpy(g_two_blocks, g_block, edid::BLOCK_BYTES);
    string::memset(g_two_blocks + edid::BLOCK_BYTES, 0xFF, edid::BLOCK_BYTES);
    string::memcpy(g_two_blocks + edid::BLOCK_BYTES, descriptor_at(0) + bytes_inside, DESCRIPTOR_BYTES - bytes_inside);

    ASSERT_EQ(edid::parse(g_two_blocks, sizeof(g_two_blocks), g_monitor), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
    EXPECT_TRUE(g_monitor.first_timing_is_preferred);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);
}

TEST(edid, refuses_a_wrong_header) {
    static constexpr size_t CHANGED_HEADER_OFFSETS[] = {0, 3, 7};
    static constexpr uint8_t HEADER_BIT_FLIPS[] = {0x01, 0x80};
    static constexpr uint8_t DISPLAYID_2_FIRST_BYTES[] = {0x20, 0x2F};

    for (size_t offset : CHANGED_HEADER_OFFSETS) {
        for (uint8_t flip : HEADER_BIT_FLIPS) {
            make_base_block(AOC_FEATURES);
            g_block[offset] ^= flip;
            seal_block();

            expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_HEADER);
        }
    }

    // These first bytes are refused as a wrong header, where the EDID code in NVIDIA's open kernel modules takes them
    // as a version of DisplayID 2 (a format other than EDID) and checks no other header byte
    for (uint8_t first_byte : DISPLAYID_2_FIRST_BYTES) {
        make_base_block(AOC_FEATURES);
        g_block[0] = first_byte;
        seal_block();

        expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_HEADER);
    }
}

TEST(edid, refuses_a_bad_checksum) {
    make_base_block(AOC_FEATURES);
    g_block[CHECKSUM_OFFSET]++;
    expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_CHECKSUM);

    make_base_block(AOC_FEATURES);
    g_block[CHECKSUM_OFFSET] ^= 0x80;
    expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_CHECKSUM);

    make_base_block(AOC_FEATURES);
    descriptor_at(2)[DISPLAY_TAG_OFFSET] = DISPLAY_TAG_PRODUCT_NAME;
    expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_CHECKSUM);
}

TEST(edid, reads_only_version_1_at_any_revision) {
    static constexpr uint8_t REFUSED_VERSIONS[] = {0, 2, 0x81};
    static constexpr uint8_t ACCEPTED_REVISIONS[] = {0, 0xFF};

    for (uint8_t version : REFUSED_VERSIONS) {
        make_base_block(AOC_FEATURES);
        g_block[VERSION_OFFSET] = version;
        seal_block();

        expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_VERSION);
    }

    for (uint8_t revision : ACCEPTED_REVISIONS) {
        make_base_block(AOC_FEATURES);
        g_block[REVISION_OFFSET] = revision;
        seal_block();

        EXPECT_EQ(parse_block(), edid::OK);
    }
}

TEST(edid, reports_the_first_check_that_fails) {
    make_base_block(AOC_FEATURES);
    g_block[0] = 0x01;
    g_block[VERSION_OFFSET] = 2;
    expect_refused(edid::BLOCK_BYTES - 1, edid::ERR_TOO_SHORT);
    expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_HEADER);

    make_base_block(AOC_FEATURES);
    g_block[VERSION_OFFSET] = 2;
    expect_refused(edid::BLOCK_BYTES, edid::ERR_BAD_CHECKSUM);
}

TEST(edid, reads_display_descriptors_alone_as_no_timings) {
    // A pixel clock of 0 makes a display descriptor even when the other bytes form a timing
    timing_descriptor_fields no_clock = FULL_HD;
    no_clock.clock_units = 0;

    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, no_clock);

    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.maker, "AOC");
    EXPECT_STREQ(g_monitor.name, "");
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);
}

// Every count's high bits set, so a mask that drops one of them changes a count
static constexpr uint8_t ALL_HIGH_BITS_DESCRIPTOR[DESCRIPTOR_BYTES] = {
    0xFF, 0xFF, 0x00, 0x01, 0xFF, 0x00, 0x02, 0xFF, 0x00, 0x01, 0x0F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1E,
};

// The high-bit fields sharing a byte hold different patterns, so a field read from a neighbor's place changes a count
static constexpr uint8_t HIGH_BITS_DESCRIPTOR[DESCRIPTOR_BYTES] = {
    0x34, 0x12, 0xBC, 0xDE, 0xA5, 0xF1, 0xE2, 0x93, 0xC4, 0xA7, 0xBD, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1A,
};

TEST(edid, reads_the_high_bits_of_every_count) {
    make_base_block(AOC_FEATURES);
    string::memcpy(descriptor_at(2), ALL_HIGH_BITS_DESCRIPTOR, DESCRIPTOR_BYTES);
    string::memcpy(descriptor_at(3), HIGH_BITS_DESCRIPTOR, DESCRIPTOR_BYTES);
    seal_block();

    ASSERT_EQ(parse_block(), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 2u);
    expect_timing(g_monitor.timings[0], {655350, 3840, 768, 769, 2304, 3840, 48, 63, 3731, true, true});
    expect_timing(g_monitor.timings[1], {46600, 2748, 196, 423, 883, 2545, 43, 61, 890, true, false});
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);
}

// Separate digital sync gives both polarities and digital composite sync the horizontal one, and a missing polarity
// reads as the EDID code in NVIDIA's open kernel modules assigns it, horizontal negative and vertical positive
static constexpr sync_polarity_case SYNC_POLARITY_CASES[] = {
    {0x18, false, false}, {0x1A, true, false}, {0x1C, false, true}, {0x1E, true, true},
    {0x10, false, true},  {0x12, true, true},  {0x14, false, true}, {0x16, true, true},
    {0x00, false, true},  {0x06, false, true}, {0x08, false, true}, {0x0E, false, true},
    {0x7B, true, false},  {0x7D, false, true}, {0x75, false, true}, {0x6F, false, true},
};

TEST(edid, reads_sync_polarity_as_the_sync_kind_allows) {
    for (const sync_polarity_case& c : SYNC_POLARITY_CASES) {
        timing_descriptor_fields fields = FULL_HD;
        fields.flags = c.flags;
        make_base_block(AOC_FEATURES);
        write_timing_descriptor(0, fields);

        ASSERT_EQ(parse_block(), edid::OK);
        ASSERT_EQ(g_monitor.timing_count, 1u);
        EXPECT_EQ(g_monitor.timings[0].h_sync_positive, c.h_sync_positive);
        EXPECT_EQ(g_monitor.timings[0].v_sync_positive, c.v_sync_positive);
    }
}

// `FULL_HD` changed into a timing the parser leaves out
static constexpr timing_descriptor_fields LEFT_OUT_TIMINGS[] = {
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x80},    // interlaced, no other flag set
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x9E},    // interlaced
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0xFF},    // interlaced, every other flag set too
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 1, 0, 0x1E},    // a horizontal border
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0x80, 0, 0x1E}, // a horizontal border with only its high bit set
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 1, 0x1E},    // a vertical border
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0x80, 0x1E}, // a vertical border with only its high bit set
    {14850, 0, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E},       // no width
    {14850, 1920, 280, 88, 44, 0, 45, 4, 5, 0, 0, 0x1E},       // no height
    {14850, 1920, 131, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E},    // front porch and sync one pixel past the blanking
    {14850, 1920, 280, 88, 44, 1080, 8, 4, 5, 0, 0, 0x1E},     // front porch and sync one line past the blanking
};

TEST(edid, leaves_out_empty_and_undescribable_timings) {
    for (const timing_descriptor_fields& fields : LEFT_OUT_TIMINGS) {
        make_base_block(AOC_FEATURES);
        write_timing_descriptor(0, fields);
        write_timing_descriptor(1, FULL_HD);

        ASSERT_EQ(parse_block(), edid::OK);
        ASSERT_EQ(g_monitor.timing_count, 1u);
        expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
        EXPECT_FALSE(g_monitor.first_timing_is_preferred);
    }
}

TEST(edid, keeps_timings_with_no_back_porch_or_every_flag_but_interlaced) {
    timing_descriptor_fields no_h_back_porch = FULL_HD;
    no_h_back_porch.h_blanking = FULL_HD.h_front_porch + FULL_HD.h_sync_width;
    timing_descriptor_fields no_v_back_porch = FULL_HD;
    no_v_back_porch.v_blanking = FULL_HD.v_front_porch + FULL_HD.v_sync_width;
    timing_descriptor_fields every_flag_but_interlaced = FULL_HD;
    every_flag_but_interlaced.flags = 0x7F;

    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, no_h_back_porch);
    write_timing_descriptor(1, no_v_back_porch);
    write_timing_descriptor(2, every_flag_but_interlaced);

    ASSERT_EQ(parse_block(), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 3u);
    EXPECT_EQ(g_monitor.timings[0].h_back_porch, 0u);
    EXPECT_EQ(g_monitor.timings[0].v_back_porch, FULL_HD_TIMING.v_back_porch);
    EXPECT_EQ(g_monitor.timings[1].h_back_porch, FULL_HD_TIMING.h_back_porch);
    EXPECT_EQ(g_monitor.timings[1].v_back_porch, 0u);
    expect_timing(g_monitor.timings[2], FULL_HD_TIMING);
}

// At revisions 3 and 4 alike, bit 1 is the preferred mark, as in the EDID code in NVIDIA's open kernel modules
TEST(edid, marks_the_first_descriptor_preferred_only_as_the_features_byte_says) {
    static constexpr uint8_t REVISIONS[] = {3, 4};
    static constexpr uint8_t FIRST_PREFERRED_FEATURES[] = {AOC_FEATURES, 0x02, 0xFF};
    static constexpr uint8_t NONE_PREFERRED_FEATURES[] = {AOC_FEATURES_BIT_1_CLEAR, 0xFD};

    for (uint8_t revision : REVISIONS) {
        for (uint8_t features : FIRST_PREFERRED_FEATURES) {
            make_base_block(features);
            g_block[REVISION_OFFSET] = revision;
            write_timing_descriptor(0, FULL_HD);

            ASSERT_EQ(parse_block(), edid::OK);
            EXPECT_TRUE(g_monitor.first_timing_is_preferred);
        }

        for (uint8_t features : NONE_PREFERRED_FEATURES) {
            make_base_block(features);
            g_block[REVISION_OFFSET] = revision;
            write_timing_descriptor(0, FULL_HD);

            ASSERT_EQ(parse_block(), edid::OK);
            EXPECT_FALSE(g_monitor.first_timing_is_preferred);
        }
    }
}

TEST(edid, marks_no_timing_preferred_when_the_first_descriptor_is_a_display_descriptor) {
    make_base_block(AOC_FEATURES);
    write_timing_descriptor(1, FULL_HD);
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 1u);
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);

    write_name_descriptor(0, "NAMED FIRST\n ");
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.name, "NAMED FIRST");
    EXPECT_EQ(g_monitor.timing_count, 1u);
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);
}

static constexpr name_case NAME_CASES[] = {
    {"ABCDEFGHIJKLM", "ABCDEFGHIJKLM"},
    {"TRAILING  \n  ", "TRAILING"},
    {"INNER SPACE  ", "INNER SPACE"},
    {"NUL\0PADDED\n  ", "NUL"},
    {"\n            ", ""},
    {"A\x01" "B\x7F" "C\x80" "D\xFF" "E\t   ", "A?B?C?D?E?"},
    {" ~\x1F" "\n         ", " ~?"},
    {"\x8A" "\x0B" "STOP! \n    ", "??STOP!"},
    {"             ", ""},
};

// The timing after each name puts a nonzero byte just past the name's 13 bytes
TEST(edid, reads_the_name_to_a_newline_or_nul_trimmed_and_printable) {
    for (const name_case& c : NAME_CASES) {
        make_base_block(AOC_FEATURES);
        write_name_descriptor(1, c.text);
        write_timing_descriptor(2, FULL_HD);

        ASSERT_EQ(parse_block(), edid::OK);
        EXPECT_STREQ(g_monitor.name, c.name);
    }
}

TEST(edid, reads_the_first_name_descriptor_alone) {
    make_base_block(AOC_FEATURES);
    write_name_descriptor(0, "HIGH BIT TAG\n");
    descriptor_at(0)[DISPLAY_TAG_OFFSET] = DISPLAY_TAG_PRODUCT_NAME & 0x7F; // a tag differing in its high bit alone
    write_name_descriptor(1, "\n            ");
    write_name_descriptor(2, "SECOND\n      ");

    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.name, "");
}

// A name descriptor setting its reserved byte 2 gives no name, as the EDID code in NVIDIA's open kernel modules parses
// no display descriptor whose byte 2 is set
TEST(edid, reads_no_name_from_a_display_descriptor_with_byte_2_set) {
    static constexpr uint8_t SET_RESERVED_BYTES[] = {0x01, 0x80};

    for (uint8_t reserved : SET_RESERVED_BYTES) {
        make_base_block(AOC_FEATURES);
        write_name_descriptor(1, "BYTE 2 SET\n  ");
        descriptor_at(1)[DISPLAY_RESERVED_OFFSET] = reserved;
        write_name_descriptor(2, "BYTE 2 CLEAR\n");

        ASSERT_EQ(parse_block(), edid::OK);
        EXPECT_STREQ(g_monitor.name, "BYTE 2 CLEAR");
    }
}

TEST(edid, reads_maker_codes_as_a_to_z_or_question_marks) {
    make_base_block(AOC_FEATURES);
    g_block[MAKER_OFFSET] = 0x00;
    g_block[MAKER_OFFSET + 1] = 0x3A; // letters 0, 1 and 26
    seal_block();
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.maker, "?AZ");

    g_block[MAKER_OFFSET] = 0xEF;
    g_block[MAKER_OFFSET + 1] = 0xE1; // bit 15 set, then letters 27, 31 and 1
    seal_block();
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.maker, "??A");

    g_block[MAKER_OFFSET] = 0x84;
    g_block[MAKER_OFFSET + 1] = 0x21; // bit 15 set, then letters 1, 1 and 1
    seal_block();
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.maker, "AAA");
}

TEST(edid, reads_a_pixel_clock_with_either_byte_zero) {
    timing_descriptor_fields low_byte_zero = FULL_HD;
    low_byte_zero.clock_units = 0x3A00;
    timing_descriptor_fields high_byte_zero = FULL_HD;
    high_byte_zero.clock_units = 0x00FF;
    timing_descriptor_fields only_bit_15 = FULL_HD;
    only_bit_15.clock_units = 0x8000;
    timing_descriptor_fields only_bit_7 = FULL_HD;
    only_bit_7.clock_units = 0x0080;

    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, low_byte_zero);
    write_timing_descriptor(1, high_byte_zero);
    write_timing_descriptor(2, only_bit_15);
    write_timing_descriptor(3, only_bit_7);

    ASSERT_EQ(parse_block(), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 4u);
    EXPECT_EQ(g_monitor.timings[0].pixel_clock_khz, 148480u);
    EXPECT_EQ(g_monitor.timings[1].pixel_clock_khz, 2550u);
    EXPECT_EQ(g_monitor.timings[2].pixel_clock_khz, 327680u);
    EXPECT_EQ(g_monitor.timings[3].pixel_clock_khz, 1280u);
}

TEST(edid, reads_a_timing_with_the_name_tag_in_byte_3_as_a_timing) {
    // Bytes 2 and 3 hold a product name descriptor's 0 and 0xFC, so only the nonzero clock makes the bytes a timing
    timing_descriptor_fields tag_in_byte_3 = FULL_HD;
    tag_in_byte_3.width = 0x700;
    tag_in_byte_3.h_blanking = 0x100 | DISPLAY_TAG_PRODUCT_NAME;

    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, tag_in_byte_3);

    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_STREQ(g_monitor.name, "");
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], {148500, 1792, 88, 44, 376, 1080, 4, 5, 36, true, true});
}

// Five 0x01 bytes at a descriptor's start mark it unused, as the EDID code in NVIDIA's open kernel modules parses it
static constexpr uint8_t UNUSED_FILL       = 0x01;
static constexpr size_t  UNUSED_FILL_BYTES = 5;

// One fill byte changed to 0x02, in its high bit alone (0x81) or in its low bit alone (0x00), but not to 0x00 at byte 2
// or 4, which would leave the timing out with a width of 0 or too short a blanking
static constexpr fill_change FILL_CHANGES[] = {
    {0, 0x02}, {1, 0x02}, {2, 0x02}, {3, 0x02}, {4, 0x02},
    {0, 0x81}, {1, 0x81}, {2, 0x81}, {3, 0x81}, {4, 0x81},
    {0, 0x00}, {1, 0x00}, {3, 0x00},
};

TEST(edid, reads_a_descriptor_starting_with_five_0x01_bytes_as_unused) {
    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, FULL_HD);
    write_timing_descriptor(1, FULL_HD);
    string::memset(descriptor_at(0), UNUSED_FILL, UNUSED_FILL_BYTES);
    seal_block();

    ASSERT_EQ(parse_block(), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);

    // A fill with any one of these changes is no longer unused, so the descriptor's bytes give a timing
    for (const fill_change& change : FILL_CHANGES) {
        string::memset(descriptor_at(0), UNUSED_FILL, UNUSED_FILL_BYTES);
        descriptor_at(0)[change.offset] = change.value;
        seal_block();

        ASSERT_EQ(parse_block(), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 2u);
        EXPECT_TRUE(g_monitor.first_timing_is_preferred);
    }
}

// `FULL_HD` with one field of its timing changed in each row
static constexpr timing_descriptor_fields FULL_HD_NEIGHBORS[] = {
    {14851, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E}, // pixel clock
    {14850, 1921, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E}, // width
    {14850, 1920, 281, 89, 44, 1080, 45, 4, 5, 0, 0, 0x1E}, // horizontal front porch
    {14850, 1920, 281, 88, 45, 1080, 45, 4, 5, 0, 0, 0x1E}, // horizontal sync width
    {14850, 1920, 281, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1E}, // horizontal back porch
    {14850, 1920, 280, 88, 44, 1081, 45, 4, 5, 0, 0, 0x1E}, // height
    {14850, 1920, 280, 88, 44, 1080, 46, 5, 5, 0, 0, 0x1E}, // vertical front porch
    {14850, 1920, 280, 88, 44, 1080, 46, 4, 6, 0, 0, 0x1E}, // vertical sync width
    {14850, 1920, 280, 88, 44, 1080, 46, 4, 5, 0, 0, 0x1E}, // vertical back porch
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1C}, // horizontal sync polarity
    {14850, 1920, 280, 88, 44, 1080, 45, 4, 5, 0, 0, 0x1A}, // vertical sync polarity
};

TEST(edid, lists_a_repeated_timing_once) {
    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, FULL_HD);
    write_timing_descriptor(2, FULL_HD);
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 1u);

    for (const timing_descriptor_fields& neighbor : FULL_HD_NEIGHBORS) {
        write_timing_descriptor(2, neighbor);
        ASSERT_EQ(parse_block(), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 2u);
    }

    // A repeat of the first timing and a repeat of a later one, each after another timing, are both listed once
    write_timing_descriptor(1, FULL_HD_NEIGHBORS[0]);
    write_timing_descriptor(2, FULL_HD);
    write_timing_descriptor(3, FULL_HD_NEIGHBORS[0]);
    ASSERT_EQ(parse_block(), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 2u);
}

TEST(edid, computes_refresh_in_64_bits) {
    edid::timing fastest_clock = {655350, 1, 1, 1, 5, 1, 1, 1, 5, true, true};
    EXPECT_EQ(fastest_clock.refresh_millihertz(), 10239843750ull);

    edid::timing frame_past_32_bits = {1000000, 65534, 1, 1, 1, 65534, 1, 1, 1, true, true};
    EXPECT_EQ(frame_past_32_bits.refresh_millihertz(), 232ull);

    edid::timing no_lines = {148500, 5, 0, 0, 0, 0, 0, 0, 0, true, true};
    edid::timing no_pixels = {148500, 0, 0, 0, 0, 5, 0, 0, 0, true, true};
    EXPECT_EQ(no_lines.refresh_millihertz(), 0ull);
    EXPECT_EQ(no_pixels.refresh_millihertz(), 0ull);
}

TEST(edid, rewrites_the_monitor_on_every_parse) {
    ASSERT_EQ(edid::parse(AOC_AG241QG4_EDID, sizeof(AOC_AG241QG4_EDID), g_monitor), edid::OK);

    make_base_block(AOC_FEATURES_BIT_1_CLEAR);
    write_timing_descriptor(0, FULL_HD);
    ASSERT_EQ(parse_block(), edid::OK);

    EXPECT_STREQ(g_monitor.name, "");
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
    expect_timing(g_monitor.timings[1], {});
    EXPECT_FALSE(g_monitor.first_timing_is_preferred);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);
}

TEST(edid, counts_the_extension_blocks_the_bytes_do_not_hold_whole) {
    make_base_block(AOC_FEATURES);
    copy_block_into_edid();
    set_extension_count(1);
    write_extension(1, EXTENSION_TAG_CTA_861);

    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES - 1), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);

    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);

    // Block 2 lies past the bytes given, so it is skipped whether its checksum passes or fails
    set_extension_count(2);
    write_extension(2, EXTENSION_TAG_CTA_861);
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);

    block_at(2)[CHECKSUM_OFFSET]++;
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);

    set_extension_count(0xFF);
    ASSERT_EQ(parse_edid(edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 255u);
}

TEST(edid, counts_cta_861_and_displayid_extension_blocks_failing_their_checksum) {
    static constexpr uint8_t CHECKED_TAGS[] = {EXTENSION_TAG_CTA_861, EXTENSION_TAG_DISPLAYID};

    for (uint8_t tag : CHECKED_TAGS) {
        make_base_block(AOC_FEATURES);
        write_timing_descriptor(0, FULL_HD);
        copy_block_into_edid();
        set_extension_count(1);

        write_extension(1, tag);
        ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);

        // The base block's timing stays listed when the extension block is skipped
        block_at(1)[CHECKSUM_OFFSET]++;
        ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 1u);
        ASSERT_EQ(g_monitor.timing_count, 1u);
        expect_timing(g_monitor.timings[0], FULL_HD_TIMING);

        write_extension(1, tag);
        block_at(1)[CHECKSUM_OFFSET] ^= 0x80;
        ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 1u);
    }
}

// 0x10, which marks a Video Timing Block (an extension that lists timings), the tags either side of 0x02 and 0x70, and
// those two with only the high bit flipped
static constexpr uint8_t UNCHECKED_TAGS[] = {0x01, 0x03, 0x82, 0x6F, 0x71, 0xF0, 0x10};

TEST(edid, counts_no_extension_block_of_another_kind_failing_its_checksum) {
    for (uint8_t tag : UNCHECKED_TAGS) {
        make_base_block(AOC_FEATURES);
        copy_block_into_edid();
        set_extension_count(1);
        write_extension(1, tag);
        block_at(1)[CHECKSUM_OFFSET]++;

        ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);
    }
}

// Blocks 1 and 3 fail their checksum around a good block 2, and a failing block 4 lies just past the last block while
// byte 126 says 3, then is the last when it says 4
TEST(edid, counts_failing_blocks_up_to_the_extension_count_and_none_past_it) {
    make_base_block(AOC_FEATURES);
    copy_block_into_edid();
    write_extension(1, EXTENSION_TAG_CTA_861);
    write_extension(2, EXTENSION_TAG_DISPLAYID);
    write_extension(3, EXTENSION_TAG_DISPLAYID);
    write_extension(4, EXTENSION_TAG_CTA_861);
    block_at(1)[CHECKSUM_OFFSET]++;
    block_at(3)[CHECKSUM_OFFSET]++;
    block_at(4)[CHECKSUM_OFFSET]++;

    set_extension_count(3);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 2u);

    set_extension_count(4);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 3u);
}

// The start of a CTA-861 block whose one data block (a tagged field before the block's descriptors) is the HDMI Forum's
// override of the extension count (HF-EEODB), giving 2
static constexpr uint8_t HF_EEODB_BLOCK_START[] = {EXTENSION_TAG_CTA_861, 0x03, 0x07, 0x00, 0xE2, 0x78, 0x02};

// A failing CTA-861 block 2 stays unchecked where byte 126 of an EDID 1.3 (version 1, revision 3) gives 1 and block 1
// holds the override
TEST(edid, takes_the_extension_count_from_byte_126_alone) {
    make_base_block(AOC_FEATURES);
    g_block[REVISION_OFFSET] = 3;
    seal_block();
    copy_block_into_edid();
    set_extension_count(1);

    string::memcpy(block_at(1), HF_EEODB_BLOCK_START, sizeof(HF_EEODB_BLOCK_START));
    seal(block_at(1));
    write_extension(2, EXTENSION_TAG_CTA_861);
    block_at(2)[CHECKSUM_OFFSET]++;

    ASSERT_EQ(parse_edid(3 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);
}

// Starts of a first descriptor ending before the checksum byte 127, the last ending at byte 126
static constexpr uint8_t READ_STARTS[] = {4, 5, 109};

// Starts of a first descriptor that ends past the block, its bytes past the block written into the uncounted blocks
// after it
static constexpr uint8_t PAST_BLOCK_STARTS[] = {127, 128, 0xEE, 0xFF};

TEST(edid, reads_cta_861_descriptors_from_the_offset_in_byte_2) {
    for (uint8_t start : READ_STARTS) {
        write_cta_861_edid(CTA_861_REVISION_3, start);

        ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
        ASSERT_EQ(g_monitor.timing_count, 1u);
        expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
        EXPECT_FALSE(g_monitor.first_timing_is_preferred);
    }

    for (uint8_t start : PAST_BLOCK_STARTS) {
        write_cta_861_edid(CTA_861_REVISION_3, start);

        ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 0u);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);
    }
}

// Sets block 1's checksum byte 127 to `value` and adds the difference to byte 3, which the parser does not read, so the
// block's bytes keep summing to 0 modulo 256
static void set_checksum_byte(uint8_t value) {
    uint8_t* block = block_at(1);
    block[CTA_861_UNREAD_OFFSET] = static_cast<uint8_t>(block[CTA_861_UNREAD_OFFSET] + block[CHECKSUM_OFFSET] - value);
    block[CHECKSUM_OFFSET] = value;
}

// With the checksum byte set to a timing's flags, a descriptor at byte 110 holds that timing whole and would give it if
// the walk read a descriptor reaching byte 127
TEST(edid, reads_no_cta_861_descriptor_reaching_the_checksum_byte) {
    write_cta_861_edid(CTA_861_REVISION_3, 110);
    set_checksum_byte(FULL_HD.flags);

    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);

    // Each descriptor must end before byte 127, so the walk reads the one at byte 92 and not the one at byte 110
    write_cta_861_edid(CTA_861_REVISION_3, 92);
    write_extension_descriptor(1, 110, FULL_HD_NEIGHBORS[9]);
    set_checksum_byte(FULL_HD_NEIGHBORS[9].flags);

    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
}

// The CTA-861 header bytes written over a descriptor starting at byte 0 to 3 leave it a timing, so only byte 2's rule
// leaves the block giving none
TEST(edid, reads_no_cta_861_descriptor_at_revision_0_or_inside_the_header) {
    static constexpr uint8_t READ_REVISIONS[] = {1, 2, CTA_861_REVISION_3, 0x80, 0xFF};
    static constexpr uint8_t HEADER_STARTS[] = {0, 1, 2, 3};

    for (uint8_t revision : READ_REVISIONS) {
        write_cta_861_edid(revision, CTA_861_HEADER_BYTES);
        ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 1u);
    }

    write_cta_861_edid(0, CTA_861_HEADER_BYTES);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);

    for (uint8_t start : HEADER_STARTS) {
        write_cta_861_edid(CTA_861_REVISION_3, start);
        ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 0u);
    }
}

// Six descriptors fill the block from byte 4: a timing the parser leaves out, `FULL_HD` with its first five bytes set
// to the fill of an unused descriptor, then four timings, two of them with one byte of the pixel clock zero
TEST(edid, reads_cta_861_descriptors_up_to_the_first_zero_clock) {
    timing_descriptor_fields low_byte_zero = FULL_HD;
    low_byte_zero.clock_units = 0x3A00;
    timing_descriptor_fields high_byte_zero = FULL_HD;
    high_byte_zero.clock_units = 0x00FF;
    timing_descriptor_fields no_clock = FULL_HD;
    no_clock.clock_units = 0;

    write_cta_861_edid(CTA_861_REVISION_3, CTA_861_HEADER_BYTES);
    write_extension_descriptor(1, cta_861_descriptor_offset(0), LEFT_OUT_TIMINGS[0]);
    write_extension_descriptor(1, cta_861_descriptor_offset(1), FULL_HD);
    string::memset(block_at(1) + cta_861_descriptor_offset(1), UNUSED_FILL, UNUSED_FILL_BYTES);
    write_extension_descriptor(1, cta_861_descriptor_offset(2), FULL_HD);
    write_extension_descriptor(1, cta_861_descriptor_offset(3), low_byte_zero);
    write_extension_descriptor(1, cta_861_descriptor_offset(4), high_byte_zero);
    write_extension_descriptor(1, cta_861_descriptor_offset(5), FULL_HD_NEIGHBORS[1]);

    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 4u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
    EXPECT_EQ(g_monitor.timings[1].pixel_clock_khz, 148480u);
    EXPECT_EQ(g_monitor.timings[2].pixel_clock_khz, 2550u);
    EXPECT_EQ(g_monitor.timings[3].width, 1921u);

    // A pixel clock of 0 ends the walk, so the descriptor after it gives nothing
    write_extension_descriptor(1, cta_861_descriptor_offset(4), no_clock);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 2u);
}

TEST(edid, lists_no_timing_from_a_skipped_or_uncounted_cta_861_block) {
    write_cta_861_edid(CTA_861_REVISION_3, CTA_861_HEADER_BYTES);
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 1u);

    // Cut short at its last byte, the block still holds its descriptor whole
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES - 1), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);

    block_at(1)[CHECKSUM_OFFSET]++;
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 1u);

    // Block 2 holds a timing descriptor of its own, but byte 126 counts block 1 alone
    block_at(1)[CHECKSUM_OFFSET]--;
    write_cta_861_block(2);
    write_extension_descriptor(2, CTA_861_HEADER_BYTES, FULL_HD_NEIGHBORS[0]);
    ASSERT_EQ(parse_edid(3 * edid::BLOCK_BYTES), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 1u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
}

// Lays out block 1 as a CTA-861 block holding `FULL_HD` but with `tag` in its byte 0, and expects no timing from it
static void expect_no_timing_with_tag(uint8_t tag) {
    write_cta_861_edid(CTA_861_REVISION_3, CTA_861_HEADER_BYTES);
    block_at(1)[EXTENSION_TAG_OFFSET] = tag;
    seal(block_at(1));

    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);
}

// A Video Timing Block at revision 1 with one timing descriptor, whose timing the EDID parse in NVIDIA's open kernel
// modules lists. Its tag and revision come first, then its counts of timing descriptors and of two kinds of signal code
static constexpr uint8_t VIDEO_TIMING_BLOCK_START[] = {0x10, 0x01, 0x01, 0x00, 0x00};

TEST(edid, lists_timings_from_cta_861_blocks_alone) {
    for (uint8_t tag : UNCHECKED_TAGS) {
        expect_no_timing_with_tag(tag);
    }

    // The same bytes give no timing from a DisplayID block whose section passes, its checksum byte written over the low
    // byte of the descriptor's height
    write_cta_861_edid(CTA_861_REVISION_3, CTA_861_HEADER_BYTES);
    block_at(1)[EXTENSION_TAG_OFFSET] = EXTENSION_TAG_DISPLAYID;
    seal_displayid_block(1);
    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);

    make_base_block(AOC_FEATURES);
    copy_block_into_edid();
    set_extension_count(1);
    string::memcpy(block_at(1), VIDEO_TIMING_BLOCK_START, sizeof(VIDEO_TIMING_BLOCK_START));
    write_extension_descriptor(1, sizeof(VIDEO_TIMING_BLOCK_START), FULL_HD);

    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, 0u);
}

TEST(edid, lists_cta_861_timings_after_the_base_block_and_a_repeat_once) {
    make_base_block(AOC_FEATURES);
    write_timing_descriptor(0, FULL_HD);
    copy_block_into_edid();
    set_extension_count(1);
    write_cta_861_block(1);
    write_extension_descriptor(1, cta_861_descriptor_offset(0), FULL_HD_NEIGHBORS[0]);
    write_extension_descriptor(1, cta_861_descriptor_offset(1), FULL_HD);

    ASSERT_EQ(parse_edid(2 * edid::BLOCK_BYTES), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, 2u);
    expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
    EXPECT_EQ(g_monitor.timings[1].pixel_clock_khz, 148510u);
    EXPECT_TRUE(g_monitor.first_timing_is_preferred);
}

// `FULL_HD` with its pixel clock raised by `n` units, a timing distinct for every `n` that keeps the clock in 16 bits
static timing_descriptor_fields full_hd_raised(uint32_t n) {
    timing_descriptor_fields fields = FULL_HD;
    fields.clock_units += n;
    return fields;
}

// Lays out `g_edid` with `count` distinct timings, at least the base block's four, from `full_hd_raised(0)` on: four in
// the base block, then six in each CTA-861 block but the last, which holds the rest
static void write_distinct_timings(uint32_t count) {
    make_base_block(AOC_FEATURES);
    for (uint32_t i = 0; i < BASE_DESCRIPTORS; i++) {
        write_timing_descriptor(i, full_hd_raised(i));
    }

    copy_block_into_edid();
    size_t block_count = 0;
    for (uint32_t n = BASE_DESCRIPTORS; n < count; n++) {
        size_t index = 1 + (n - BASE_DESCRIPTORS) / CTA_861_DESCRIPTORS_MAX;
        size_t slot = (n - BASE_DESCRIPTORS) % CTA_861_DESCRIPTORS_MAX;
        if (slot == 0) {
            write_cta_861_block(index);
            block_count = index;
        }

        write_extension_descriptor(index, cta_861_descriptor_offset(slot), full_hd_raised(n));
    }

    set_extension_count(static_cast<uint8_t>(block_count));
}

TEST(edid, lists_32_timings_and_counts_a_33rd_as_dropped) {
    write_distinct_timings(edid::TIMINGS_MAX);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, edid::TIMINGS_MAX);
    EXPECT_EQ(g_monitor.timings[edid::TIMINGS_MAX - 1].pixel_clock_khz, 148810u);
    EXPECT_EQ(g_monitor.dropped_timings, 0u);

    write_distinct_timings(edid::TIMINGS_MAX + 1);
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    ASSERT_EQ(g_monitor.timing_count, edid::TIMINGS_MAX);
    EXPECT_EQ(g_monitor.timings[edid::TIMINGS_MAX - 1].pixel_clock_khz, 148810u);
    EXPECT_EQ(g_monitor.dropped_timings, 1u);
    EXPECT_EQ(g_monitor.skipped_extensions, 0u);
}

// Thirty-two timings leave block 5 holding the last four and two descriptors free for the timings found with the array
// full
TEST(edid, counts_a_timing_found_with_the_array_full_unless_it_is_listed) {
    write_distinct_timings(edid::TIMINGS_MAX);
    write_extension_descriptor(5, cta_861_descriptor_offset(4), full_hd_raised(0));
    write_extension_descriptor(5, cta_861_descriptor_offset(5), full_hd_raised(edid::TIMINGS_MAX - 1));
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.timing_count, edid::TIMINGS_MAX);
    EXPECT_EQ(g_monitor.dropped_timings, 0u);

    // A timing equal to no listed one counts each time it is found
    write_extension_descriptor(5, cta_861_descriptor_offset(4), full_hd_raised(edid::TIMINGS_MAX));
    write_extension_descriptor(5, cta_861_descriptor_offset(5), full_hd_raised(edid::TIMINGS_MAX));
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.dropped_timings, 2u);

    // Descriptors giving no timing do not count: a timing the parser leaves out, then the fill of an unused descriptor
    write_extension_descriptor(5, cta_861_descriptor_offset(4), LEFT_OUT_TIMINGS[0]);
    string::memset(block_at(5) + cta_861_descriptor_offset(5), UNUSED_FILL, UNUSED_FILL_BYTES);
    seal(block_at(5));
    ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
    EXPECT_EQ(g_monitor.dropped_timings, 0u);
}

// Version bytes of DisplayID 1.3 and 2.1 and of two versions DisplayID does not name, each section checked the same way
static constexpr uint8_t SECTION_VERSION_BYTES[] = {0x13, 0x21, 0x00, 0x92};

TEST(edid, skips_a_displayid_block_whose_section_fails_its_checksum) {
    for (uint8_t version : SECTION_VERSION_BYTES) {
        make_base_block(AOC_FEATURES);
        write_timing_descriptor(0, FULL_HD);
        copy_block_into_edid();
        set_extension_count(3);
        write_cta_861_block(1);
        write_extension_descriptor(1, cta_861_descriptor_offset(0), full_hd_raised(1));
        write_displayid_block(2, version);
        write_cta_861_block(3);
        write_extension_descriptor(3, cta_861_descriptor_offset(0), full_hd_raised(2));
        ASSERT_EQ(parse_edid(4 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.timing_count, 3u);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);

        // The section's checksum byte off by one makes both checksums fail, and the block is counted once
        uint8_t* block = block_at(2);
        size_t section_checksum_offset = DISPLAYID_DATA_OFFSET + block[DISPLAYID_DATA_BYTES_OFFSET];
        block[section_checksum_offset]++;
        ASSERT_EQ(parse_edid(4 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 1u);

        // With the block's checksum right, the section check alone skips the DisplayID block, and the CTA-861 blocks on
        // either side keep their timings listed
        seal(block);
        ASSERT_EQ(parse_edid(4 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 1u);
        ASSERT_EQ(g_monitor.timing_count, 3u);
        expect_timing(g_monitor.timings[0], FULL_HD_TIMING);
        EXPECT_EQ(g_monitor.timings[1].pixel_clock_khz, 148510u);
        EXPECT_EQ(g_monitor.timings[2].pixel_clock_khz, 148520u);

        // The byte after the section's checksum byte is not part of the section
        block[section_checksum_offset]--;
        block[section_checksum_offset + 1] = 0x5A;
        seal(block);
        ASSERT_EQ(parse_edid(4 * edid::BLOCK_BYTES), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);
    }
}

// With 122 bytes of data blocks the section's checksum byte is the block's own, and with more it lies past the block,
// where the test writes it so the section's bytes would sum to 0 modulo 256 if read there
static constexpr uint8_t LONG_DATA_BYTES[] = {122, 123, 251, 255};

// Product types and counts of extension sections for which a stricter parser could skip a DisplayID block
static constexpr uint8_t UNCOMPARED_SECTION_VALUES[] = {7, 0xFF};

TEST(edid, skips_a_displayid_block_whose_data_blocks_take_more_than_121_bytes) {
    for (uint8_t version : SECTION_VERSION_BYTES) {
        make_base_block(AOC_FEATURES);
        copy_block_into_edid();
        set_extension_count(1);
        write_displayid_block(1, version);
        block_at(1)[DISPLAYID_DATA_BYTES_OFFSET] = DISPLAYID_DATA_BYTES_MAX;
        seal_displayid_block(1);
        ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
        EXPECT_EQ(g_monitor.skipped_extensions, 0u);

        // The product type and the count of extension sections are summed but never compared
        for (uint8_t value : UNCOMPARED_SECTION_VALUES) {
            block_at(1)[DISPLAYID_PRODUCT_TYPE_OFFSET] = value;
            block_at(1)[DISPLAYID_EXTENSION_SECTIONS_OFFSET] = value;
            seal_displayid_block(1);

            ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
            EXPECT_EQ(g_monitor.skipped_extensions, 0u);
        }

        write_displayid_block(1, version);

        for (uint8_t data_bytes : LONG_DATA_BYTES) {
            uint8_t* block = block_at(1);
            block[DISPLAYID_DATA_BYTES_OFFSET] = data_bytes;
            seal(block);
            if (DISPLAYID_DATA_OFFSET + data_bytes > CHECKSUM_OFFSET) {
                write_section_checksum(block);
            }

            ASSERT_EQ(parse_edid(sizeof(g_edid)), edid::OK);
            EXPECT_EQ(g_monitor.skipped_extensions, 1u);
        }
    }
}
