// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_fwsec);

using drivers::nvidia::copy_vbios_bytes;
using drivers::nvidia::describe_fwsec_program;
using drivers::nvidia::ERR_BAD_VBIOS;
using drivers::nvidia::falcon_program;
using drivers::nvidia::fwsec_layout;
using drivers::nvidia::fwsec_signature;
using drivers::nvidia::OK;
using drivers::nvidia::prepare_fwsec_frts;
using drivers::nvidia::registers;
using drivers::nvidia::ROM_WINDOW_BYTES;
using drivers::nvidia::select_fwsec_signature;

// The first fuse register recording a program's version and the bytes to the next, restated here so the tests hold
// the choice to NVIDIA's register map rather than to its own constants
static constexpr uint32_t UCODE_VERSION_FUSES       = 0x8241C0;
static constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4;

// The application interface, the DMEM mapper, the FRTS command and the signatures' size, restated here so the tests
// hold the patch to NVIDIA's layout rather than to its own constants
static constexpr uint32_t INTERFACE_ENTRY_COUNT       = 0x03;
static constexpr uint32_t INTERFACE_HEADER_BYTES      = 4;
static constexpr uint32_t INTERFACE_ENTRY_ID          = 0x00;
static constexpr uint32_t INTERFACE_ENTRY_DATA_OFFSET = 0x04;
static constexpr uint32_t INTERFACE_ENTRY_BYTES       = 8;
static constexpr uint32_t DMEM_MAPPER_ID              = 0x4;
static constexpr uint32_t MAPPER_INPUT_OFFSET         = 0x08;
static constexpr uint32_t MAPPER_INPUT_SIZE           = 0x0C;
static constexpr uint32_t MAPPER_INIT_COMMAND         = 0x2C;
static constexpr uint32_t MAPPER_BYTES                = 64;
static constexpr uint32_t FRTS_COMMAND_ID             = 0x15;
static constexpr uint32_t FRTS_COMMAND_WORDS          = 12;
static constexpr uint32_t FRTS_COMMAND_BYTES          = 4 * FRTS_COMMAND_WORDS;
static constexpr uint32_t SIGNATURE_BYTES             = 384;

// An interface entry id other than the DMEM mapper's
static constexpr uint32_t OTHER_ENTRY_ID = 0x5;

// A zeroed fake of BAR0, all 16 MiB of it
struct fuse_bar0 {
    static constexpr size_t BYTES = 0x1000000;

    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~fuse_bar0() { heap::ufree(bytes); }

    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// Writes the fuse register of the program with `ucode_id`, which may name a register just outside the 16
static void put_fuses(fuse_bar0& bar0, uint32_t ucode_id, uint32_t fuses) {
    bar0.write(UCODE_VERSION_FUSES + UCODE_VERSION_FUSE_STRIDE * ucode_id - UCODE_VERSION_FUSE_STRIDE, fuses);
}

// FWSEC as `find_fwsec` returns it for the RTX 3080's VBIOS
static constexpr fwsec_layout RTX_3080_FWSEC = {
    .offset = 0x43864,
    .size = 59904,
    .code_size = 57856,
    .data_size = 2048,
    .signature_slot = 0x5A4,
    .interface_offset = 0x1C,
    .signatures_offset = 0x433E4,
    .imem_physical_base = 0,
    .imem_virtual_base = 0,
    .dmem_physical_base = 0,
    .engine_mask = 0x400,
    .signature_count = 3,
    .ucode_id = 9,
    .signature_versions = 0x7,
};

// What the RTX 3080's fuse register for FWSEC holds, recording fuse version 2
static constexpr uint32_t RTX_3080_FWSEC_FUSES = 0x3;

static int32_t select_for_fuses(
    fuse_bar0& bar0,
    const fwsec_layout& fwsec,
    uint32_t fuses,
    fwsec_signature& signature
) {
    put_fuses(bar0, fwsec.ucode_id, fuses);
    return select_fwsec_signature(bar0.regs, fwsec, signature);
}

// The fuse registers next to FWSEC's record other versions, so reading the wrong one picks another signature
TEST(nvidia_fwsec, rtx_3080_signature_is_its_third) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_fuses(bar0, 8, 0x1);
    put_fuses(bar0, 9, RTX_3080_FWSEC_FUSES);
    put_fuses(bar0, 10, 0x0);

    fwsec_signature signature = {};
    EXPECT_EQ(select_fwsec_signature(bar0.regs, RTX_3080_FWSEC, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);
    EXPECT_EQ(signature.index, 2u);
}

// With a signature for every version up to 15, the index is the fuse version, and versions from 16 have none
TEST(nvidia_fwsec, fuse_version_is_the_highest_set_bit_plus_one) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout every_version = RTX_3080_FWSEC;
    every_version.signature_versions = 0xFFFF;
    every_version.signature_count = 16;

    fwsec_signature signature = {};
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x0, signature), OK);
    EXPECT_EQ(signature.index, 0u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x1, signature), OK);
    EXPECT_EQ(signature.index, 1u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x5, signature), OK);
    EXPECT_EQ(signature.index, 3u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x7FFF, signature), OK);
    EXPECT_EQ(signature.fuse_version, 15u);
    EXPECT_EQ(signature.index, 15u);

    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x8000, signature), ERR_BAD_VBIOS);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x80000000, signature), ERR_BAD_VBIOS);

    // Version 32 has no signature. x86_64 and aarch64 shift a 32-bit value by 32 as by 0, so an unbounded bit test
    // would see version 0's bit, and a count of 2 lets the index 1 through.
    fwsec_layout first_version = RTX_3080_FWSEC;
    first_version.signature_versions = 0x1;
    first_version.signature_count = 2;
    EXPECT_EQ(select_for_fuses(bar0, first_version, 0x80000000, signature), ERR_BAD_VBIOS);
}

// Ucode ids 0 and 17 name no fuse register, so the registers right outside the 16 are never read
TEST(nvidia_fwsec, only_ucode_ids_1_to_16_read_a_fuse_register) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_fuses(bar0, 0, 0x3);
    put_fuses(bar0, 1, 0x1);
    put_fuses(bar0, 16, 0x3);
    put_fuses(bar0, 17, 0x1);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.ucode_id = 0;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);

    fwsec.ucode_id = 1;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 1u);

    fwsec.ucode_id = 16;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);

    fwsec.ucode_id = 17;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);
}

// A signature's place counts only lower versions FWSEC has signatures for, so with 0x8001 version 15 takes the
// second, and signatures for all the other versions do not stand in for the fuse version's
TEST(nvidia_fwsec, signature_versions_must_include_the_fuse_version) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.signature_versions = 0x3;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_versions = 0xFFFB;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_versions = 0x4;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);
    EXPECT_EQ(signature.index, 0u);

    fwsec.signature_versions = 0x8001;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, 0x7FFF, signature), OK);
    EXPECT_EQ(signature.fuse_version, 15u);
    EXPECT_EQ(signature.index, 1u);
}

// The RTX 3080's FWSEC leaves room for 3 signatures before it, so a descriptor sized for them but counting 2 passes
// NVIDIA's bound and fails this one
TEST(nvidia_fwsec, signature_must_be_among_the_signatures) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.signature_count = 2;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_count = 3;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), OK);
}

// The ROM window, filled so each byte differs from its neighbors and from those any multiple of 64 KiB away, so a copy
// from a nearby offset or from one cut to 16 bits differs
struct fwsec_rom {
    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(ROM_WINDOW_BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    fwsec_rom() {
        if (!bytes) {
            return;
        }

        for (uint32_t i = 0; i < ROM_WINDOW_BYTES; i++) {
            bytes[i] = static_cast<uint8_t>(i + (i >> 8) + (i >> 16) + 1);
        }
    }

    ~fwsec_rom() { heap::ufree(bytes); }
};

// Zeroed bytes past FWSEC's end in both images, so a nonzero write within 256 bytes past its end makes them differ
static constexpr uint32_t IMAGE_GUARD_BYTES = 256;

// FWSEC as `prepare_fwsec_frts` writes it and as a test expects it
struct fwsec_image {
    uint32_t size;
    uint8_t* actual;
    uint8_t* expected;

    explicit fwsec_image(const fwsec_layout& fwsec)
        : size(fwsec.size + IMAGE_GUARD_BYTES)
        , actual(static_cast<uint8_t*>(heap::uzalloc(size)))
        , expected(static_cast<uint8_t*>(heap::uzalloc(size))) {}

    ~fwsec_image() {
        heap::ufree(actual);
        heap::ufree(expected);
    }

    bool matches() const { return string::memcmp(actual, expected, size) == 0; }
};

static void put_u32(uint8_t* bytes, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

struct interface_entry {
    uint32_t id;
    uint32_t data_offset;
};

// Where the DMEM mapper and its input buffer lie, in bytes from the start of FWSEC's data, and the buffer's size
struct mapper_placement {
    uint32_t mapper;
    uint32_t input;
    uint32_t input_bytes;
};

static uint8_t* data_in_rom(fwsec_rom& rom, const fwsec_layout& fwsec) {
    return rom.bytes + fwsec.offset + fwsec.code_size;
}

static void write_interface(fwsec_rom& rom, const fwsec_layout& fwsec, const interface_entry* entries, uint8_t count) {
    uint8_t* interface = data_in_rom(rom, fwsec) + fwsec.interface_offset;

    interface[INTERFACE_ENTRY_COUNT] = count;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* entry = interface + INTERFACE_HEADER_BYTES + i * INTERFACE_ENTRY_BYTES;
        put_u32(entry + INTERFACE_ENTRY_ID, entries[i].id);
        put_u32(entry + INTERFACE_ENTRY_DATA_OFFSET, entries[i].data_offset);
    }
}

static void write_mapper(fwsec_rom& rom, const fwsec_layout& fwsec, const mapper_placement& placement) {
    uint8_t* mapper = data_in_rom(rom, fwsec) + placement.mapper;

    put_u32(mapper + MAPPER_INPUT_OFFSET, placement.input);
    put_u32(mapper + MAPPER_INPUT_SIZE, placement.input_bytes);
}

// FWSEC as the ROM holds it, with the FRTS command in the mapper's input buffer, its id in the mapper's command to run
// and the chosen signature in the signature slot
static void build_expected(
    const fwsec_rom& rom,
    const fwsec_layout& fwsec,
    const mapper_placement& placement,
    const uint32_t* command,
    const fwsec_signature& signature,
    uint8_t* expected
) {
    uint8_t* data = expected + fwsec.code_size;
    const uint8_t* signature_bytes = rom.bytes + fwsec.signatures_offset + signature.index * SIGNATURE_BYTES;

    string::memcpy(expected, rom.bytes + fwsec.offset, fwsec.size);
    for (uint32_t i = 0; i < FRTS_COMMAND_WORDS; i++) {
        put_u32(data + placement.input + 4 * i, command[i]);
    }

    put_u32(data + placement.mapper + MAPPER_INIT_COMMAND, FRTS_COMMAND_ID);
    string::memcpy(data + fwsec.signature_slot, signature_bytes, SIGNATURE_BYTES);
}

// The RTX 3080's interface lists the DMEM mapper and then another entry, and the mapper's 64-byte input buffer ends
// the data
static constexpr interface_entry RTX_3080_ENTRIES[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = 0x560},
    {.id = OTHER_ENTRY_ID, .data_offset = 0x7AC},
};

static constexpr mapper_placement RTX_3080_MAPPER = {.mapper = 0x560, .input = 0x7C0, .input_bytes = 0x40};

static constexpr fwsec_signature RTX_3080_SIGNATURE = {.fuse_version = 2, .index = 2};

static constexpr uint64_t RTX_3080_FRTS_OFFSET = 0x27FE00000;

// The FRTS command for the RTX 3080's FRTS region, as 32-bit words
static constexpr uint32_t RTX_3080_FRTS_COMMAND[FRTS_COMMAND_WORDS] = {
    1, 24, 0, 0, 0, 2, 1, 20, 0x27FE00, 0x100, 2, 0,
};

static void write_rtx_3080_fwsec(fwsec_rom& rom, const fwsec_layout& fwsec) {
    write_interface(rom, fwsec, RTX_3080_ENTRIES, 2);
    write_mapper(rom, fwsec, RTX_3080_MAPPER);
}

static int32_t prepare_for_rtx_3080(const fwsec_rom& rom, const fwsec_layout& fwsec, fwsec_image& image) {
    return prepare_fwsec_frts(rom.regs, fwsec, RTX_3080_SIGNATURE, RTX_3080_FRTS_OFFSET, image.actual);
}

// Only the command, its id and the signature differ from FWSEC as the ROM holds it
TEST(nvidia_fwsec, rtx_3080_fwsec_is_patched_for_its_frts_region) {
    fwsec_rom rom;
    fwsec_image image(RTX_3080_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    ASSERT_NOT_NULL(image.expected);
    write_rtx_3080_fwsec(rom, RTX_3080_FWSEC);

    build_expected(rom, RTX_3080_FWSEC, RTX_3080_MAPPER, RTX_3080_FRTS_COMMAND, RTX_3080_SIGNATURE, image.expected);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);
    EXPECT_TRUE(image.matches());
}

// FWSEC, its data, its signatures, the interface, the mapper and the input buffer all start at offsets that are not
// multiples of 4, and with no signature for version 1 the one for version 2 is the second
static constexpr fwsec_layout UNALIGNED_FWSEC = {
    .offset = 0x10001,
    .size = 0x500,
    .code_size = 0x102,
    .data_size = 0x300,
    .signature_slot = 0x31,
    .interface_offset = 0x1C1,
    .signatures_offset = 0xFB7F,
    .signature_count = 2,
    .ucode_id = 9,
    .signature_versions = 0x5,
};

static constexpr interface_entry UNALIGNED_ENTRIES[] = {
    {.id = OTHER_ENTRY_ID, .data_offset = 0x11},
    {.id = DMEM_MAPPER_ID, .data_offset = 0x1D7},
};

static constexpr mapper_placement UNALIGNED_MAPPER = {.mapper = 0x1D7, .input = 0x22B, .input_bytes = 0x30};

// The command for an FRTS region at the top of 44-bit video memory, whose offset in 4 KiB units takes all 32 bits
static constexpr uint64_t HIGHEST_FRTS_OFFSET = 0xFFFFFF00000;

static constexpr uint32_t HIGHEST_FRTS_COMMAND[FRTS_COMMAND_WORDS] = {
    1, 24, 0, 0, 0, 2, 1, 20, 0xFFFFFF00, 0x100, 2, 0,
};

TEST(nvidia_fwsec, unaligned_fwsec_is_patched_for_the_highest_frts_offset) {
    fwsec_rom rom;
    fwsec_image image(UNALIGNED_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    ASSERT_NOT_NULL(image.expected);
    write_interface(rom, UNALIGNED_FWSEC, UNALIGNED_ENTRIES, 2);
    write_mapper(rom, UNALIGNED_FWSEC, UNALIGNED_MAPPER);

    fwsec_signature version_2 = {.fuse_version = 2, .index = 1};
    build_expected(rom, UNALIGNED_FWSEC, UNALIGNED_MAPPER, HIGHEST_FRTS_COMMAND, version_2, image.expected);
    EXPECT_EQ(prepare_fwsec_frts(rom.regs, UNALIGNED_FWSEC, version_2, HIGHEST_FRTS_OFFSET, image.actual), OK);
    EXPECT_TRUE(image.matches());
}

// Two DMEM mapper entries around another entry, the first leading to a mapper before the signature slot
static constexpr interface_entry TWO_MAPPER_ENTRIES[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = 0x100},
    {.id = OTHER_ENTRY_ID, .data_offset = 0x7AC},
    {.id = DMEM_MAPPER_ID, .data_offset = 0x560},
};

static constexpr mapper_placement FIRST_MAPPER = {.mapper = 0x100, .input = 0x200, .input_bytes = 0x40};

// Only the last DMEM mapper receives the command, so the first keeps the ROM's bytes
TEST(nvidia_fwsec, command_goes_to_the_last_dmem_mapper) {
    fwsec_rom rom;
    fwsec_image image(RTX_3080_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    ASSERT_NOT_NULL(image.expected);
    write_interface(rom, RTX_3080_FWSEC, TWO_MAPPER_ENTRIES, 3);
    write_mapper(rom, RTX_3080_FWSEC, FIRST_MAPPER);
    write_mapper(rom, RTX_3080_FWSEC, RTX_3080_MAPPER);

    build_expected(rom, RTX_3080_FWSEC, RTX_3080_MAPPER, RTX_3080_FRTS_COMMAND, RTX_3080_SIGNATURE, image.expected);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);
    EXPECT_TRUE(image.matches());
}

// Ids on both sides of the DMEM mapper's and one equal to it in its low 16 bits only, then the RTX 3080's DMEM
// mapper entry
static constexpr interface_entry MAPPER_ENTRY_LAST[] = {
    {.id = DMEM_MAPPER_ID - 1, .data_offset = 0x560},
    {.id = DMEM_MAPPER_ID + 1, .data_offset = 0x560},
    {.id = DMEM_MAPPER_ID + 0x10000, .data_offset = 0x560},
    {.id = DMEM_MAPPER_ID, .data_offset = 0x560},
};

// A valid mapper at the data's start, where the patch would look had the walk kept no DMEM mapper entry
static constexpr mapper_placement MAPPER_AT_START = {.mapper = 0, .input = 0x200, .input_bytes = 0x40};

// One entry is refused even when it is a DMEM mapper's, lowering the count from 4 to 3 leaves the only DMEM mapper
// entry just past it, refused though a valid mapper lies at offset 0, and a DMEM mapper entry after 39 others is found
TEST(nvidia_fwsec, interface_needs_two_entries_and_a_dmem_mapper) {
    fwsec_rom rom;
    fwsec_image image(RTX_3080_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    write_rtx_3080_fwsec(rom, RTX_3080_FWSEC);

    write_interface(rom, RTX_3080_FWSEC, RTX_3080_ENTRIES, 1);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), ERR_BAD_VBIOS);

    write_interface(rom, RTX_3080_FWSEC, MAPPER_ENTRY_LAST, 4);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);

    write_mapper(rom, RTX_3080_FWSEC, MAPPER_AT_START);
    write_interface(rom, RTX_3080_FWSEC, MAPPER_ENTRY_LAST, 3);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), ERR_BAD_VBIOS);

    interface_entry many_entries[40];
    for (interface_entry& entry : many_entries) {
        entry = RTX_3080_ENTRIES[1];
    }

    many_entries[39] = RTX_3080_ENTRIES[0];
    write_interface(rom, RTX_3080_FWSEC, many_entries, 40);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);
}

// A mapper whose input buffer lies before the signature slot, clear of an interface at the data's end
static constexpr mapper_placement INPUT_BEFORE_SLOT = {.mapper = 0x560, .input = 0x200, .input_bytes = 0x40};

// The header and 2 entries fit ending at the data's end and not a byte later, though FWSEC goes on past its data, and
// offsets valid only in their low 16 bits or at minus the header's size, whose end wraps to 0, are refused
TEST(nvidia_fwsec, interface_must_lie_inside_the_data) {
    fwsec_layout room_past_data = RTX_3080_FWSEC;
    room_past_data.size += 0x100;

    fwsec_rom rom;
    fwsec_image image(room_past_data);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    write_mapper(rom, room_past_data, INPUT_BEFORE_SLOT);

    fwsec_layout at_end = room_past_data;
    at_end.interface_offset = RTX_3080_FWSEC.data_size - INTERFACE_HEADER_BYTES - 2 * INTERFACE_ENTRY_BYTES;
    write_interface(rom, at_end, RTX_3080_ENTRIES, 2);
    EXPECT_EQ(prepare_for_rtx_3080(rom, at_end, image), OK);

    fwsec_layout byte_past_end = at_end;
    byte_past_end.interface_offset++;
    write_interface(rom, byte_past_end, RTX_3080_ENTRIES, 2);
    EXPECT_EQ(prepare_for_rtx_3080(rom, byte_past_end, image), ERR_BAD_VBIOS);

    fwsec_layout high_offset = room_past_data;
    high_offset.interface_offset += 0x10000;
    write_interface(rom, room_past_data, RTX_3080_ENTRIES, 2);
    EXPECT_EQ(prepare_for_rtx_3080(rom, high_offset, image), ERR_BAD_VBIOS);

    fwsec_layout wrapping = room_past_data;
    wrapping.interface_offset = 0u - INTERFACE_HEADER_BYTES;
    EXPECT_EQ(prepare_for_rtx_3080(rom, wrapping, image), ERR_BAD_VBIOS);
}

static void write_mapper_entry(fwsec_rom& rom, uint32_t mapper) {
    const interface_entry entries[] = {
        {.id = DMEM_MAPPER_ID, .data_offset = mapper},
        RTX_3080_ENTRIES[1],
    };

    write_interface(rom, RTX_3080_FWSEC, entries, 2);
}

// A mapper ending at the data's end, and one a byte later whose fields would otherwise serve
static constexpr mapper_placement MAPPER_AT_END = {
    .mapper = RTX_3080_FWSEC.data_size - MAPPER_BYTES,
    .input = 0x200,
    .input_bytes = 0x40,
};

static constexpr mapper_placement MAPPER_BYTE_PAST_END = {
    .mapper = MAPPER_AT_END.mapper + 1,
    .input = 0x200,
    .input_bytes = 0x40,
};

// A DMEM mapper entry leading to a mapper that ends a byte past the data's end, before the RTX 3080's DMEM mapper entry
static constexpr interface_entry MAPPER_PAST_END_FIRST[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = MAPPER_BYTE_PAST_END.mapper},
    RTX_3080_ENTRIES[1],
    RTX_3080_ENTRIES[0],
};

// A mapper fits ending at the data's end and not a byte later, though FWSEC goes on past its data and a valid mapper
// comes last, and offsets valid only in their low 16 bits or at minus the mapper's size, whose end wraps, are refused
TEST(nvidia_fwsec, dmem_mapper_must_lie_inside_the_data) {
    fwsec_layout room_past_data = RTX_3080_FWSEC;
    room_past_data.size += 0x100;

    fwsec_rom rom;
    fwsec_image image(room_past_data);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    write_mapper(rom, room_past_data, RTX_3080_MAPPER);

    write_mapper_entry(rom, MAPPER_AT_END.mapper);
    write_mapper(rom, room_past_data, MAPPER_AT_END);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), OK);

    write_mapper_entry(rom, MAPPER_BYTE_PAST_END.mapper);
    write_mapper(rom, room_past_data, MAPPER_BYTE_PAST_END);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);

    write_interface(rom, room_past_data, MAPPER_PAST_END_FIRST, 3);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);

    write_mapper_entry(rom, 0x10000000 + RTX_3080_MAPPER.mapper);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);

    write_mapper_entry(rom, 0u - MAPPER_BYTES);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);
}

// A buffer size whose low 16 bits are 0 still holds the command, and a refusal after the mapper is found leaves the
// copy as the ROM holds it
TEST(nvidia_fwsec, input_buffer_must_hold_the_command) {
    fwsec_rom rom;
    fwsec_image image(RTX_3080_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    write_rtx_3080_fwsec(rom, RTX_3080_FWSEC);

    mapper_placement exact_buffer = RTX_3080_MAPPER;
    exact_buffer.input_bytes = FRTS_COMMAND_BYTES;
    write_mapper(rom, RTX_3080_FWSEC, exact_buffer);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);

    mapper_placement short_buffer = RTX_3080_MAPPER;
    short_buffer.input_bytes = FRTS_COMMAND_BYTES - 1;
    write_mapper(rom, RTX_3080_FWSEC, short_buffer);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), ERR_BAD_VBIOS);
    EXPECT_EQ(string::memcmp(image.actual, rom.bytes + RTX_3080_FWSEC.offset, RTX_3080_FWSEC.size), 0);

    mapper_placement wide_buffer = RTX_3080_MAPPER;
    wide_buffer.input_bytes = 0x10000;
    write_mapper(rom, RTX_3080_FWSEC, wide_buffer);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);
}

// The interface's last entry gives an offset within the command's size of the data's end, or one past it whose end
// wraps, which NVIDIA's driver refuses although the command fits
static constexpr interface_entry LAST_ENTRY_NEAR_THE_END[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = 0x560},
    {.id = OTHER_ENTRY_ID, .data_offset = RTX_3080_FWSEC.data_size - 1},
};

static constexpr interface_entry LAST_ENTRY_PAST_THE_END[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = 0x560},
    {.id = OTHER_ENTRY_ID, .data_offset = 0u - 1},
};

// A command fits ending at the data's end whatever offset the last entry gives, and not a byte later though FWSEC goes
// on past its data, leaving the copy unpatched. Offsets valid only in their low 16 bits or whose end wraps fail too.
TEST(nvidia_fwsec, command_must_end_inside_the_data) {
    fwsec_layout room_past_data = RTX_3080_FWSEC;
    room_past_data.size += 0x100;

    fwsec_rom rom;
    fwsec_image image(room_past_data);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    write_interface(rom, room_past_data, LAST_ENTRY_NEAR_THE_END, 2);

    mapper_placement at_end = RTX_3080_MAPPER;
    at_end.input = RTX_3080_FWSEC.data_size - FRTS_COMMAND_BYTES;
    write_mapper(rom, room_past_data, at_end);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), OK);

    write_interface(rom, room_past_data, LAST_ENTRY_PAST_THE_END, 2);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), OK);

    // The RTX 3080's last entry gives 0x7AC, so NVIDIA's driver writes this command though it ends past the data
    mapper_placement byte_past_end = at_end;
    byte_past_end.input++;
    write_interface(rom, room_past_data, RTX_3080_ENTRIES, 2);
    write_mapper(rom, room_past_data, byte_past_end);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);
    EXPECT_EQ(string::memcmp(image.actual, rom.bytes + room_past_data.offset, room_past_data.size), 0);

    mapper_placement high_offset = RTX_3080_MAPPER;
    high_offset.input += 0x10000000;
    write_mapper(rom, room_past_data, high_offset);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);

    mapper_placement wrapping = RTX_3080_MAPPER;
    wrapping.input = 0u - FRTS_COMMAND_BYTES;
    write_mapper(rom, room_past_data, wrapping);
    EXPECT_EQ(prepare_for_rtx_3080(rom, room_past_data, image), ERR_BAD_VBIOS);
}

// An input buffer covering its mapper's field for the command to run, which neither driver refuses
static constexpr interface_entry MAPPER_UNDER_INPUT_ENTRIES[] = {
    {.id = DMEM_MAPPER_ID, .data_offset = 0x100},
    RTX_3080_ENTRIES[1],
};

static constexpr mapper_placement MAPPER_UNDER_INPUT = {.mapper = 0x100, .input = 0x110, .input_bytes = 0x40};

// The command's id is written after the command, so it is what the shared field holds, while NVIDIA's driver writes
// the id first and leaves the command's word there
TEST(nvidia_fwsec, command_id_overwrites_an_input_buffer_over_the_mapper) {
    fwsec_rom rom;
    fwsec_image image(RTX_3080_FWSEC);
    ASSERT_NOT_NULL(rom.bytes);
    ASSERT_NOT_NULL(image.actual);
    ASSERT_NOT_NULL(image.expected);
    write_interface(rom, RTX_3080_FWSEC, MAPPER_UNDER_INPUT_ENTRIES, 2);
    write_mapper(rom, RTX_3080_FWSEC, MAPPER_UNDER_INPUT);

    build_expected(rom, RTX_3080_FWSEC, MAPPER_UNDER_INPUT, RTX_3080_FRTS_COMMAND, RTX_3080_SIGNATURE, image.expected);
    EXPECT_EQ(prepare_for_rtx_3080(rom, RTX_3080_FWSEC, image), OK);
    EXPECT_TRUE(image.matches());
}

// Copies starting and ending inside a word, up to the ROM window's last byte, match the ROM
TEST(nvidia_fwsec, vbios_bytes_are_copied_at_any_offset) {
    fwsec_rom rom;
    ASSERT_NOT_NULL(rom.bytes);

    uint8_t copy[7] = {};
    copy_vbios_bytes(rom.regs, 0x1, 6, copy);
    EXPECT_EQ(string::memcmp(copy, rom.bytes + 0x1, 6), 0);

    copy_vbios_bytes(rom.regs, 0x3, 1, copy);
    EXPECT_EQ(copy[0], rom.bytes[0x3]);

    copy_vbios_bytes(rom.regs, ROM_WINDOW_BYTES - 7, 7, copy);
    EXPECT_EQ(string::memcmp(copy, rom.bytes + ROM_WINDOW_BYTES - 7, 7), 0);
}

// FWSEC with every field the description reads distinct
static constexpr fwsec_layout DISTINCT_FWSEC = {
    .offset = 0x10000,
    .size = 0x14700,
    .code_size = 0x1700,
    .data_size = 0x13000,
    .signature_slot = 0x12680,
    .interface_offset = 0x1C,
    .signatures_offset = 0xFE00,
    .imem_physical_base = 0x100,
    .imem_virtual_base = 0x10200,
    .dmem_physical_base = 0x300,
    .engine_mask = 0x400,
    .signature_count = 2,
    .ucode_id = 0x13,
    .signature_versions = 0x3,
};

static constexpr uint64_t COPY_DMA_ADDRESS = 0x4A1234567000;

// FWSEC's sizes, bases, engine mask and ucode id carry over, its signature slot becomes the signature's offset in
// the data, and the copy's address and caching are the caller's
TEST(nvidia_fwsec, fwsec_is_described_as_a_falcon_program) {
    falcon_program program = describe_fwsec_program(DISTINCT_FWSEC, COPY_DMA_ADDRESS, true);

    EXPECT_EQ(program.dma_address, COPY_DMA_ADDRESS);
    EXPECT_TRUE(program.is_cached);
    EXPECT_EQ(program.code_size, 0x1700u);
    EXPECT_EQ(program.imem_physical_base, 0x100u);
    EXPECT_EQ(program.imem_virtual_base, 0x10200u);
    EXPECT_EQ(program.data_size, 0x13000u);
    EXPECT_EQ(program.dmem_physical_base, 0x300u);
    EXPECT_EQ(program.signature_offset, 0x12680u);
    EXPECT_EQ(program.engine_mask, 0x400u);
    EXPECT_EQ(program.ucode_id, 0x13u);

    EXPECT_FALSE(describe_fwsec_program(DISTINCT_FWSEC, COPY_DMA_ADDRESS, false).is_cached);
}
