// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "common/string.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/pci_driver.h"
#include "fw/fw.h"
#include "mm/heap.h"

TEST_SUITE(nvidia);

using drivers::DECLINED;
using drivers::nvidia::chip_firmware;
using drivers::nvidia::chip_info;
using drivers::nvidia::decode_chip_id;
using drivers::nvidia::decode_chip_revision;
using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::ERR_BAD_VBIOS;
using drivers::nvidia::falcon_program_table;
using drivers::nvidia::find_chip;
using drivers::nvidia::find_falcon_program_table;
using drivers::nvidia::find_fwsec;
using drivers::nvidia::firmware_files;
using drivers::nvidia::fwsec_layout;
using drivers::nvidia::INTERRUPT_LEAVES_MAX;
using drivers::nvidia::OK;
using drivers::nvidia::open_firmware_files;
using drivers::nvidia::PMC_BOOT_0;
using drivers::nvidia::read_vbios_layout;
using drivers::nvidia::registers;
using drivers::nvidia::ROM_WINDOW_BYTES;
using drivers::nvidia::to_bring_up_result;
using drivers::nvidia::VBIOS_MAX_IMAGES;
using drivers::nvidia::vbios_image;
using drivers::nvidia::vbios_layout;

// PMC_BOOT_0 as an RTX 3080 reports it, a GA102 at revision A1
static constexpr uint32_t RTX_3080_BOOT_0 = 0xb72000a1;
static constexpr uint16_t GA102_CHIP_ID = 0x172;
static constexpr uint8_t RTX_3080_REVISION = 0xa1;

// The physical address bits NVIDIA's driver gives GA102 for reaching system memory
static constexpr uint8_t GA102_DMA_ADDRESS_BITS = 47;

// The GSP image section NVIDIA's driver takes GA102's signature from, named for the GA10x chips
static constexpr char GA102_SIGNATURE_SECTION[] = ".fwsignature_ga10x";

// How NVIDIA's driver sizes the GSP firmware's memory on GA102: 20 MiB of the heap for the firmware's operating
// system, an 8 MiB base share, a minimum heap of 84 MiB, a 1 MiB unprotected heap and a prescrubbed top of 256 MiB
static constexpr uint32_t GA102_HEAP_OS_BYTES          = 0x1400000;
static constexpr uint32_t GA102_HEAP_BASE_BYTES        = 0x800000;
static constexpr uint32_t GA102_HEAP_MIN_BYTES         = 0x5400000;
static constexpr uint32_t GA102_UNPROTECTED_HEAP_BYTES = 0x100000;
static constexpr uint64_t GA102_PRESCRUBBED_TOP_BYTES  = 0x10000000;

// The 32-bit leaf registers in GA102's CPU interrupt tree (the GPU's registers reporting its interrupts to the CPU), as
// NVIDIA's headers for GA102 give them
static constexpr uint8_t GA102_INTERRUPT_LEAF_COUNT = 8;

// GA102's heads, as many as the RTX 3080's firmware reports usable, and NVIDIA's classes of its display object and
// core channel, NVC670_DISPLAY and NVC67D_CORE_CHANNEL_DMA
static constexpr uint8_t  GA102_HEAD_COUNT         = 4;
static constexpr uint32_t GA102_DISPLAY_CLASS      = 0xC670;
static constexpr uint32_t GA102_CORE_CHANNEL_CLASS = 0xC67D;

// Whether GA102's firmware trains PHY repeaters, as the RTX 3080's firmware reports for each of its links 0 to 3
static constexpr bool GA102_TRAINS_PHY_REPEATERS = true;

// The highest rate of GA102's links as DisplayPort codes it, HBR3 (8.1 Gbps per lane), as the RTX 3080's firmware
// reports for each of its links 0 to 3
static constexpr uint8_t GA102_MAX_RATE = 0x1E;

// Whether GA102's links support FEC and take the increased watermark limits, as the RTX 3080's firmware reports for
// each of its links 0 to 3
static constexpr bool GA102_FEC_SUPPORTED              = true;
static constexpr bool GA102_INCREASED_WATERMARK_LIMITS = true;

// The RTX 3080's identity decodes to the GA102 entry of the chip table
TEST(nvidia, rtx_3080_decodes_as_ga102) {
    EXPECT_EQ(decode_chip_id(RTX_3080_BOOT_0), GA102_CHIP_ID);
    EXPECT_EQ(decode_chip_revision(RTX_3080_BOOT_0), RTX_3080_REVISION);

    const chip_info* chip = find_chip(GA102_CHIP_ID);
    ASSERT_NOT_NULL(chip);
    EXPECT_EQ(chip->id, GA102_CHIP_ID);
    EXPECT_STREQ(chip->name, "GA102");
    EXPECT_EQ(chip->dma_address_bits, GA102_DMA_ADDRESS_BITS);
    EXPECT_STREQ(chip->firmware.gsp_signature_section, GA102_SIGNATURE_SECTION);
    EXPECT_EQ(chip->gsp_reservation.heap_os_bytes, GA102_HEAP_OS_BYTES);
    EXPECT_EQ(chip->gsp_reservation.heap_base_bytes, GA102_HEAP_BASE_BYTES);
    EXPECT_EQ(chip->gsp_reservation.heap_min_bytes, GA102_HEAP_MIN_BYTES);
    EXPECT_EQ(chip->gsp_reservation.unprotected_heap_bytes, GA102_UNPROTECTED_HEAP_BYTES);
    EXPECT_EQ(chip->gsp_reservation.prescrubbed_top_bytes, GA102_PRESCRUBBED_TOP_BYTES);
    EXPECT_EQ(chip->interrupt_leaf_count, GA102_INTERRUPT_LEAF_COUNT);

    EXPECT_EQ(chip->display.head_count, GA102_HEAD_COUNT);
    EXPECT_EQ(chip->display.display_class, GA102_DISPLAY_CLASS);
    EXPECT_EQ(chip->display.core_channel_class, GA102_CORE_CHANNEL_CLASS);

    EXPECT_EQ(chip->display.displayport.trains_phy_repeaters, GA102_TRAINS_PHY_REPEATERS);
    EXPECT_EQ(static_cast<uint8_t>(chip->display.displayport.max_rate), GA102_MAX_RATE);
    EXPECT_EQ(chip->display.displayport.fec_supported, GA102_FEC_SUPPORTED);
    EXPECT_EQ(chip->display.displayport.increased_watermark_limits, GA102_INCREASED_WATERMARK_LIMITS);
}

// GA104 shares GA102's architecture but has no chip table entry
static constexpr uint16_t GA104_CHIP_ID = 0x174;

// Sharing a listed chip's architecture does not give a chip an entry
TEST(nvidia, ga104_has_no_entry) {
    EXPECT_NULL(find_chip(GA104_CHIP_ID));
}

// The most leaf registers NVIDIA's driver expects a chip's CPU interrupt tree to have, NV_MAX_INTR_LEAVES
static constexpr uint8_t NVIDIA_MAX_INTERRUPT_LEAVES = 16;

// The bound the chip table keeps every tree's leaves within is NVIDIA's
TEST(nvidia, interrupt_leaves_max_is_nvidias) {
    EXPECT_EQ(INTERRUPT_LEAVES_MAX, NVIDIA_MAX_INTERRUPT_LEAVES);
}

// Large enough that a read counting words instead of bytes stays inside the
// array, so that bug fails the test instead of reading past it
static constexpr size_t FAKE_BAR0_WORDS = 16;
static constexpr uint32_t THIRD_WORD_OFFSET = 8;
static constexpr uint32_t THIRD_WORD_VALUE = 0x0000c0de;

// Offsets into a `registers` view count bytes, so offset 8 reads the third 32-bit word
TEST(nvidia, registers_view_reads_at_byte_offsets) {
    uint32_t fake_bar0[FAKE_BAR0_WORDS] = {RTX_3080_BOOT_0, 0, THIRD_WORD_VALUE};
    registers regs(reinterpret_cast<uintptr_t>(fake_bar0));

    EXPECT_EQ(regs.read(PMC_BOOT_0), RTX_3080_BOOT_0);
    EXPECT_EQ(regs.read(THIRD_WORD_OFFSET), THIRD_WORD_VALUE);
}

// Signatures and field offsets of the VBIOS structures, restated here so the
// tests hold the walk to the format rather than to its own constants
static constexpr uint16_t IMAGE_SIGNATURE           = 0xAA55;
static constexpr uint16_t NVIDIA_IMAGE_SIGNATURE    = 0x4E56;
static constexpr uint32_t PCI_DATA_SIGNATURE        = 0x52494350; // "PCIR"
static constexpr uint32_t NVIDIA_PCI_DATA_SIGNATURE = 0x5344504E; // "NPDS"
static constexpr uint32_t EXTENSION_SIGNATURE       = 0x4544504E; // "NPDE"
static constexpr uint32_t HEADER_PCI_DATA_POINTER   = 0x18;
static constexpr uint32_t PCI_DATA_LENGTH           = 0x0A;
static constexpr uint32_t PCI_DATA_IMAGE_BLOCKS     = 0x10;
static constexpr uint32_t PCI_DATA_CODE_TYPE        = 0x14;
static constexpr uint32_t PCI_DATA_INDICATOR        = 0x15;
static constexpr uint32_t EXTENSION_LENGTH          = 0x06;
static constexpr uint32_t EXTENSION_IMAGE_BLOCKS    = 0x08;
static constexpr uint32_t EXTENSION_FLAGS           = 0x0A;
static constexpr uint8_t  LAST_IMAGE                = 0x80;
static constexpr uint32_t IMAGE_BLOCK_BYTES         = 512;

static constexpr uint8_t X86_CODE    = 0x00;
static constexpr uint8_t EFI_CODE    = 0x03;
static constexpr uint8_t NVIDIA_DATA = 0xE0;

// Where an image's PCI data structure and extension start, in bytes from the image's start, and how
// long each is. `extension_start` must be the first 16-byte boundary at or after the PCI data structure's end
struct image_placement {
    uint16_t pci_data_start;
    uint16_t pci_data_bytes;
    uint16_t extension_start;
    uint16_t extension_bytes;
};

// Synthetic images place their structures as the RTX 3080's last image does, with the
// extension at the first 16-byte boundary after the PCI data structure
static constexpr image_placement SYNTHETIC_PLACEMENT = {
    .pci_data_start = 0x20,
    .pci_data_bytes = 0x18,
    .extension_start = 0x40,
    .extension_bytes = 0x14,
};

// Past the fake ROM window lies a guard block, where tests put valid structures the walk must never read
static constexpr uint32_t GUARD_BYTES = IMAGE_BLOCK_BYTES;

// A zeroed fake of the card's ROM window, followed by its guard block
struct rom_window {
    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(ROM_WINDOW_BYTES + GUARD_BYTES));

    ~rom_window() { heap::ufree(bytes); }
};

static void put_u16(rom_window& rom, uint32_t offset, uint16_t value) {
    rom.bytes[offset] = static_cast<uint8_t>(value);
    rom.bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

static void put_u32(rom_window& rom, uint32_t offset, uint32_t value) {
    put_u16(rom, offset, static_cast<uint16_t>(value));
    put_u16(rom, offset + 2, static_cast<uint16_t>(value >> 16));
}

// Writes an image's header and PCI data structure, with NVIDIA's signatures for NVIDIA's own data
static void write_image(
    rom_window& rom,
    const vbios_image& image,
    bool is_last,
    const image_placement& placement = SYNTHETIC_PLACEMENT
) {
    bool is_nvidia = image.code_type == NVIDIA_DATA;
    uint16_t image_signature = is_nvidia ? NVIDIA_IMAGE_SIGNATURE : IMAGE_SIGNATURE;
    uint32_t pci_data_signature = is_nvidia ? NVIDIA_PCI_DATA_SIGNATURE : PCI_DATA_SIGNATURE;
    uint32_t pci_data_offset = image.offset + placement.pci_data_start;
    uint16_t blocks = static_cast<uint16_t>(image.size / IMAGE_BLOCK_BYTES);

    put_u16(rom, image.offset, image_signature);
    put_u16(rom, image.offset + HEADER_PCI_DATA_POINTER, placement.pci_data_start);

    put_u32(rom, pci_data_offset, pci_data_signature);
    put_u16(rom, pci_data_offset + PCI_DATA_LENGTH, placement.pci_data_bytes);
    put_u16(rom, pci_data_offset + PCI_DATA_IMAGE_BLOCKS, blocks);
    rom.bytes[pci_data_offset + PCI_DATA_CODE_TYPE] = image.code_type;
    rom.bytes[pci_data_offset + PCI_DATA_INDICATOR] = is_last ? LAST_IMAGE : 0;
}

// Writes NVIDIA's extension for an image, giving the image's size and whether it is the last
static void write_extension(
    rom_window& rom,
    const vbios_image& image,
    bool is_last,
    const image_placement& placement = SYNTHETIC_PLACEMENT
) {
    uint32_t extension_offset = image.offset + placement.extension_start;
    uint16_t blocks = static_cast<uint16_t>(image.size / IMAGE_BLOCK_BYTES);

    put_u32(rom, extension_offset, EXTENSION_SIGNATURE);
    put_u16(rom, extension_offset + EXTENSION_LENGTH, placement.extension_bytes);
    put_u16(rom, extension_offset + EXTENSION_IMAGE_BLOCKS, blocks);
    rom.bytes[extension_offset + EXTENSION_FLAGS] = is_last ? LAST_IMAGE : 0;
}

static int32_t read_layout(const rom_window& rom, vbios_layout& layout) {
    registers window(reinterpret_cast<uintptr_t>(rom.bytes));
    return read_vbios_layout(window, layout);
}

struct rtx_3080_image {
    vbios_image     image;
    image_placement placement;
};

// The RTX 3080's VBIOS as its ROM window reads, with each image's structures where the card has them
static constexpr rtx_3080_image RTX_3080_IMAGES[] = {
    {
        .image = {.offset = 0x00000, .size = 65024, .code_type = X86_CODE},
        .placement = {
            .pci_data_start = 0x170,
            .pci_data_bytes = 0x18,
            .extension_start = 0x190,
            .extension_bytes = 0x14,
        },
    },
    {
        .image = {.offset = 0x0FE00, .size = 92672, .code_type = EFI_CODE},
        .placement = {
            .pci_data_start = 0x1C,
            .pci_data_bytes = 0x1C,
            .extension_start = 0x40,
            .extension_bytes = 0x10,
        },
    },
    {
        .image = {.offset = 0x26800, .size = 22016, .code_type = NVIDIA_DATA},
        .placement = {
            .pci_data_start = 0x140,
            .pci_data_bytes = 0x18,
            .extension_start = 0x160,
            .extension_bytes = 0x14,
        },
    },
    {
        .image = {.offset = 0x2BE00, .size = 383488, .code_type = NVIDIA_DATA},
        .placement = {
            .pci_data_start = 0x20,
            .pci_data_bytes = 0x18,
            .extension_start = 0x40,
            .extension_bytes = 0x14,
        },
    },
};

static constexpr size_t RTX_3080_IMAGE_COUNT = sizeof(RTX_3080_IMAGES) / sizeof(RTX_3080_IMAGES[0]);

// The EFI image's PCI data structure marks it last, as on the RTX 3080, while its extension
// does not, so the walk reads NVIDIA's two images only because the extension takes precedence
TEST(nvidia, rtx_3080_vbios_layout_is_read) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    for (size_t i = 0; i < RTX_3080_IMAGE_COUNT; i++) {
        const rtx_3080_image& entry = RTX_3080_IMAGES[i];
        bool is_final = i == RTX_3080_IMAGE_COUNT - 1;
        bool is_last_in_pci_data = is_final || entry.image.code_type == EFI_CODE;

        write_image(rom, entry.image, is_last_in_pci_data, entry.placement);
        write_extension(rom, entry.image, is_final, entry.placement);
    }

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), OK);
    ASSERT_EQ(layout.image_count, RTX_3080_IMAGE_COUNT);

    for (size_t i = 0; i < RTX_3080_IMAGE_COUNT; i++) {
        const vbios_image& expected = RTX_3080_IMAGES[i].image;

        EXPECT_EQ(layout.images[i].offset, expected.offset);
        EXPECT_EQ(layout.images[i].size, expected.size);
        EXPECT_EQ(layout.images[i].code_type, expected.code_type);
    }
}

static constexpr vbios_image FOUR_BLOCK_IMAGE = {.offset = 0, .size = 4 * IMAGE_BLOCK_BYTES, .code_type = NVIDIA_DATA};
static constexpr vbios_image TWO_BLOCK_IMAGE = {.offset = 0, .size = 2 * IMAGE_BLOCK_BYTES, .code_type = NVIDIA_DATA};

// The PCI data structure ends at 0x44, off the 16-byte grid, so the extension starts at 0x50
static constexpr image_placement UNEVEN_PLACEMENT = {
    .pci_data_start = 0x28,
    .pci_data_bytes = 0x1C,
    .extension_start = 0x50,
    .extension_bytes = 0x14,
};

// The PCI data structure gives four blocks and does not mark the image last, while the
// extension gives two blocks and marks it last
TEST(nvidia, extension_size_and_flags_take_precedence) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, FOUR_BLOCK_IMAGE, false, UNEVEN_PLACEMENT);
    write_extension(rom, TWO_BLOCK_IMAGE, true, UNEVEN_PLACEMENT);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), OK);
    ASSERT_EQ(layout.image_count, 1u);
    EXPECT_EQ(layout.images[0].size, TWO_BLOCK_IMAGE.size);
}

static constexpr vbios_image X86_IMAGE = {.offset = 0, .size = 2 * IMAGE_BLOCK_BYTES, .code_type = X86_CODE};
static constexpr vbios_image EFI_IMAGE = {
    .offset = 2 * IMAGE_BLOCK_BYTES,
    .size = IMAGE_BLOCK_BYTES,
    .code_type = EFI_CODE,
};

TEST(nvidia, pci_data_describes_images_without_an_extension) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, X86_IMAGE, false);
    write_image(rom, EFI_IMAGE, true);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), OK);
    ASSERT_EQ(layout.image_count, 2u);
    EXPECT_EQ(layout.images[0].size, X86_IMAGE.size);
    EXPECT_EQ(layout.images[1].offset, EFI_IMAGE.offset);
    EXPECT_EQ(layout.images[1].size, EFI_IMAGE.size);
}

TEST(nvidia, image_without_a_signature_is_refused) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, X86_IMAGE, true);
    put_u16(rom, X86_IMAGE.offset, 0);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

TEST(nvidia, image_without_pci_data_is_refused) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, X86_IMAGE, true);
    put_u32(rom, X86_IMAGE.offset + SYNTHETIC_PLACEMENT.pci_data_start, 0);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

TEST(nvidia, extension_too_short_for_its_flags_is_refused) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, TWO_BLOCK_IMAGE, true);
    write_extension(rom, TWO_BLOCK_IMAGE, true);
    put_u16(rom, TWO_BLOCK_IMAGE.offset + SYNTHETIC_PLACEMENT.extension_start + EXTENSION_LENGTH, EXTENSION_FLAGS);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

static constexpr vbios_image EMPTY_IMAGE = {.offset = 0, .size = 0, .code_type = X86_CODE};

// Marked last, so only the size check can refuse it
TEST(nvidia, image_of_zero_blocks_is_refused) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, EMPTY_IMAGE, true);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

static constexpr vbios_image WINDOW_IMAGE = {.offset = 0, .size = ROM_WINDOW_BYTES, .code_type = X86_CODE};
static constexpr vbios_image OVERSIZED_IMAGE = {
    .offset = 0,
    .size = ROM_WINDOW_BYTES + IMAGE_BLOCK_BYTES,
    .code_type = X86_CODE,
};

// An image may end at the window's end only as the last image, since the next header would lie past it
TEST(nvidia, images_must_end_inside_the_rom_window) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    vbios_layout layout = {};
    write_image(rom, WINDOW_IMAGE, true);
    EXPECT_EQ(read_layout(rom, layout), OK);

    write_image(rom, WINDOW_IMAGE, false);
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);

    write_image(rom, OVERSIZED_IMAGE, true);
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

// A chain whose second image fills the window's last block, so that image's structures can lie past the window's end
static constexpr vbios_image ALL_BUT_LAST_BLOCK = {
    .offset = 0,
    .size = ROM_WINDOW_BYTES - IMAGE_BLOCK_BYTES,
    .code_type = X86_CODE,
};

static constexpr vbios_image LAST_BLOCK = {
    .offset = ROM_WINDOW_BYTES - IMAGE_BLOCK_BYTES,
    .size = IMAGE_BLOCK_BYTES,
    .code_type = X86_CODE,
};

// The last block's header points at a valid PCI data structure in the guard block
static constexpr image_placement PCI_DATA_IN_GUARD = {
    .pci_data_start = 0x220,
    .pci_data_bytes = 0x18,
    .extension_start = 0x240,
    .extension_bytes = 0x14,
};

// The PCI data structure starts 16 bytes before the window's end, so its size and flags lie in the guard block
static constexpr image_placement PCI_DATA_ACROSS_WINDOW_END = {
    .pci_data_start = 0x1F0,
    .pci_data_bytes = 0x18,
    .extension_start = 0x210,
    .extension_bytes = 0x14,
};

TEST(nvidia, pci_data_reaching_past_the_rom_window_is_refused) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    vbios_layout layout = {};
    write_image(rom, ALL_BUT_LAST_BLOCK, false);
    write_image(rom, LAST_BLOCK, true, PCI_DATA_IN_GUARD);
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);

    write_image(rom, LAST_BLOCK, true, PCI_DATA_ACROSS_WINDOW_END);
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

// The last block's PCI data structure ends at the window's end, so its extension would start
// in the guard block, where a valid one gives two blocks and more images to come
static constexpr image_placement EXTENSION_IN_GUARD = {
    .pci_data_start = 0x1E8,
    .pci_data_bytes = 0x18,
    .extension_start = 0x200,
    .extension_bytes = 0x14,
};

static constexpr vbios_image TWO_BLOCKS_FROM_LAST_BLOCK = {
    .offset = ROM_WINDOW_BYTES - IMAGE_BLOCK_BYTES,
    .size = 2 * IMAGE_BLOCK_BYTES,
    .code_type = X86_CODE,
};

TEST(nvidia, extension_past_the_rom_window_is_ignored) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_image(rom, ALL_BUT_LAST_BLOCK, false);
    write_image(rom, LAST_BLOCK, true, EXTENSION_IN_GUARD);
    write_extension(rom, TWO_BLOCKS_FROM_LAST_BLOCK, false, EXTENSION_IN_GUARD);

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), OK);
    ASSERT_EQ(layout.image_count, 2u);
    EXPECT_EQ(layout.images[1].size, LAST_BLOCK.size);
}

static vbios_image one_block_image(size_t index) {
    uint32_t offset = static_cast<uint32_t>(index * IMAGE_BLOCK_BYTES);
    return {.offset = offset, .size = IMAGE_BLOCK_BYTES, .code_type = X86_CODE};
}

TEST(nvidia, chain_holds_at_most_the_image_limit) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    for (size_t i = 0; i < VBIOS_MAX_IMAGES; i++) {
        write_image(rom, one_block_image(i), i == VBIOS_MAX_IMAGES - 1);
    }

    vbios_layout layout = {};
    EXPECT_EQ(read_layout(rom, layout), OK);
    EXPECT_EQ(layout.image_count, VBIOS_MAX_IMAGES);

    write_image(rom, one_block_image(VBIOS_MAX_IMAGES - 1), false);
    write_image(rom, one_block_image(VBIOS_MAX_IMAGES), true);
    EXPECT_EQ(read_layout(rom, layout), ERR_BAD_VBIOS);
}

// The ids, signatures and fields of the BIT, NVIDIA's index of the VBIOS, and of the falcon program table,
// restated here so the tests hold the lookup to the format rather than to its own constants
static constexpr uint16_t BIT_ID                   = 0xB8FF;
static constexpr uint32_t BIT_SIGNATURE            = 0x00544942; // "BIT\0"
static constexpr uint16_t BIT_VERSION              = 0x0100;
static constexpr uint32_t BIT_SIGNATURE_FIELD      = 0x02;
static constexpr uint32_t BIT_VERSION_FIELD        = 0x06;
static constexpr uint32_t BIT_HEADER_SIZE          = 0x08;
static constexpr uint32_t BIT_TOKEN_SIZE           = 0x09;
static constexpr uint32_t BIT_TOKEN_COUNT          = 0x0A;
static constexpr uint32_t BIT_CHECKSUM             = 0x0B;
static constexpr uint32_t BIT_HEADER_FIELD_BYTES   = 12;
static constexpr uint32_t TOKEN_ID                 = 0x00;
static constexpr uint32_t TOKEN_DATA_VERSION       = 0x01;
static constexpr uint32_t TOKEN_DATA_SIZE          = 0x02;
static constexpr uint32_t TOKEN_DATA_OFFSET        = 0x04;
static constexpr uint8_t  TOKEN_FIELD_BYTES        = 6;
static constexpr uint8_t  FALCON_DATA_TOKEN        = 0x70;
static constexpr uint16_t FALCON_DATA_FIELD_BYTES  = 4;
static constexpr uint32_t TABLE_VERSION            = 0x00;
static constexpr uint32_t TABLE_HEADER_SIZE        = 0x01;
static constexpr uint32_t TABLE_ENTRY_SIZE         = 0x02;
static constexpr uint32_t TABLE_ENTRY_COUNT        = 0x03;
static constexpr uint32_t TABLE_HEADER_FIELD_BYTES = 6;

// A token id the lookup has no use for
static constexpr uint8_t OTHER_TOKEN = 0x01;

// Where a synthetic VBIOS places the BIT, the falcon data and the falcon program table, in bytes from the
// VBIOS's start, and the values each holds
struct table_placement {
    uint32_t bit;
    uint8_t  bit_header_size;
    uint8_t  token_size;
    uint8_t  token_count;
    uint8_t  falcon_data_token_index;
    uint8_t  falcon_data_version;
    uint16_t falcon_data_size;
    uint32_t falcon_data;
    uint32_t table;
    uint8_t  table_version;
    uint8_t  table_header_size;
    uint8_t  entry_size;
    uint8_t  entry_count;
};

// A synthetic VBIOS's images, and the base its pointers into NVIDIA's data are written from: the offset of the first
// image of NVIDIA's data less the size of the first x86 image, or 0 when either image is missing
struct layout_with_base {
    vbios_layout layout;
    uint32_t     nvidia_data_base;
};

// The bytes of a BIT header past its 12 bytes of fields hold this nonzero value, so a lookup that sums or skips
// only the fields fails
static constexpr uint8_t BIT_HEADER_PADDING = 0x5A;

// Sets the byte at `adjusted` so the `bytes` bytes at `offset` sum to zero
static void zero_sum(rom_window& rom, uint32_t offset, uint32_t bytes, uint32_t adjusted) {
    rom.bytes[adjusted] = 0;

    uint8_t sum = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        sum = static_cast<uint8_t>(sum + rom.bytes[offset + i]);
    }

    rom.bytes[adjusted] = static_cast<uint8_t>(0 - sum);
}

static uint32_t token_offset(const table_placement& placement, uint32_t index) {
    return placement.bit + placement.bit_header_size + index * placement.token_size;
}

static void write_token(
    rom_window& rom,
    uint32_t token,
    uint8_t id,
    uint8_t version,
    uint16_t size,
    uint16_t data_offset
) {
    rom.bytes[token + TOKEN_ID] = id;
    rom.bytes[token + TOKEN_DATA_VERSION] = version;
    put_u16(rom, token + TOKEN_DATA_SIZE, size);
    put_u16(rom, token + TOKEN_DATA_OFFSET, data_offset);
}

// The other tokens give 4 bytes of data at the VBIOS's start, whose zeros name no table: the falcon data token's
// id at version 1 right before it and at version 2 right after it, and another id at version 2 elsewhere
static void write_bit(rom_window& rom, const table_placement& placement) {
    uint32_t bit = placement.bit;
    uint32_t index = placement.falcon_data_token_index;

    put_u16(rom, bit, BIT_ID);
    put_u32(rom, bit + BIT_SIGNATURE_FIELD, BIT_SIGNATURE);
    put_u16(rom, bit + BIT_VERSION_FIELD, BIT_VERSION);
    rom.bytes[bit + BIT_HEADER_SIZE] = placement.bit_header_size;
    rom.bytes[bit + BIT_TOKEN_SIZE] = placement.token_size;
    rom.bytes[bit + BIT_TOKEN_COUNT] = placement.token_count;
    for (uint32_t i = BIT_HEADER_FIELD_BYTES; i < placement.bit_header_size; i++) {
        rom.bytes[bit + i] = BIT_HEADER_PADDING;
    }

    zero_sum(rom, bit, placement.bit_header_size, bit + BIT_CHECKSUM);

    for (uint32_t i = 0; i < placement.token_count; i++) {
        uint32_t token = token_offset(placement, i);
        if (i == index) {
            write_token(
                rom,
                token,
                FALCON_DATA_TOKEN,
                placement.falcon_data_version,
                placement.falcon_data_size,
                static_cast<uint16_t>(placement.falcon_data)
            );
        } else if (i + 1 == index) {
            write_token(rom, token, FALCON_DATA_TOKEN, 1, FALCON_DATA_FIELD_BYTES, 0);
        } else if (i == index + 1) {
            write_token(rom, token, FALCON_DATA_TOKEN, 2, FALCON_DATA_FIELD_BYTES, 0);
        } else {
            write_token(rom, token, OTHER_TOKEN, 2, FALCON_DATA_FIELD_BYTES, 0);
        }
    }
}

// Writes only the table's header, since `find_falcon_program_table` reads no entry
static void write_table(rom_window& rom, const table_placement& placement) {
    uint32_t table = placement.table;

    rom.bytes[table + TABLE_VERSION] = placement.table_version;
    rom.bytes[table + TABLE_HEADER_SIZE] = placement.table_header_size;
    rom.bytes[table + TABLE_ENTRY_SIZE] = placement.entry_size;
    rom.bytes[table + TABLE_ENTRY_COUNT] = placement.entry_count;
}

// Clears the ROM window first, so no structure of an earlier placement remains for the lookup to find
static void write_placement(rom_window& rom, const layout_with_base& vbios, const table_placement& placement) {
    string::memset(rom.bytes, 0, ROM_WINDOW_BYTES + GUARD_BYTES);
    write_bit(rom, placement);
    put_u32(rom, placement.falcon_data, placement.table - vbios.nvidia_data_base);
    write_table(rom, placement);
}

// The lookup gets the layout as `read_vbios_layout` would return it, so the fake ROM window holds no images
static int32_t find_table(const rom_window& rom, const layout_with_base& vbios, falcon_program_table& table) {
    registers window(reinterpret_cast<uintptr_t>(rom.bytes));
    return find_falcon_program_table(window, vbios.layout, table);
}

static int32_t find_placed(rom_window& rom, const layout_with_base& vbios, const table_placement& placement) {
    write_placement(rom, vbios, placement);

    falcon_program_table table = {};
    return find_table(rom, vbios, table);
}

// The RTX 3080's images, whose EFI image of 0x16A00 bytes lies between its x86 image and NVIDIA's data
static constexpr layout_with_base RTX_3080_VBIOS = {
    .layout = {
        .images = {
            RTX_3080_IMAGES[0].image,
            RTX_3080_IMAGES[1].image,
            RTX_3080_IMAGES[2].image,
            RTX_3080_IMAGES[3].image,
        },
        .image_count = RTX_3080_IMAGE_COUNT,
    },
    .nvidia_data_base = 0x16A00,
};

// As the RTX 3080's VBIOS places its BIT, falcon data token and falcon program table, with the falcon data at an
// offset that is arbitrary here
static constexpr table_placement RTX_3080_TABLE = {
    .bit = 0x1B0,
    .bit_header_size = 12,
    .token_size = 6,
    .token_count = 18,
    .falcon_data_token_index = 14,
    .falcon_data_version = 2,
    .falcon_data_size = 4,
    .falcon_data = 0x800,
    .table = 0x895F1,
    .table_version = 1,
    .table_header_size = 6,
    .entry_size = 6,
    .entry_count = 16,
};

// The base comes from the first of the RTX 3080's two images of NVIDIA's data, and the table from the first of two
// falcon data tokens of version 2. Its pointer 0x72BF1 does not fit in 16 bits.
TEST(nvidia, rtx_3080_falcon_program_table_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_placement(rom, RTX_3080_VBIOS, RTX_3080_TABLE);

    falcon_program_table table = {};
    EXPECT_EQ(find_table(rom, RTX_3080_VBIOS, table), OK);
    EXPECT_EQ(table.offset, 0x895F1u);
    EXPECT_EQ(table.base, 0x16A00u);
    EXPECT_EQ(table.header_size, 6u);
    EXPECT_EQ(table.entry_size, 6u);
    EXPECT_EQ(table.entry_count, 16u);
}

// NVIDIA's data starts right at the first x86 image's size, so the base is zero, and a second x86 image ends
// the VBIOS where the BIT's 16-bit offsets still reach
static constexpr layout_with_base EDGE_VBIOS = {
    .layout = {
        .images = {
            {.offset = 0x0000, .size = 0x4000, .code_type = X86_CODE},
            {.offset = 0x4000, .size = 0xB000, .code_type = NVIDIA_DATA},
            {.offset = 0xF000, .size = 0x1000, .code_type = X86_CODE},
        },
        .image_count = 3,
    },
    .nvidia_data_base = 0,
};

static constexpr uint32_t EDGE_VBIOS_END = 0x10000;

// Every size and count but the falcon data's 4 bytes differs from the RTX 3080's, the BIT's header is a byte longer
// than its fields and its tokens two bytes longer, and the falcon data token is the last token
static constexpr table_placement EDGE_TABLE = {
    .bit = 0x40,
    .bit_header_size = 13,
    .token_size = 8,
    .token_count = 3,
    .falcon_data_token_index = 2,
    .falcon_data_version = 2,
    .falcon_data_size = 4,
    .falcon_data = 0x100,
    .table = 0x9000,
    .table_version = 1,
    .table_header_size = 7,
    .entry_size = 9,
    .entry_count = 29,
};

// The VBIOS ends with its only image of NVIDIA's data, so a walk of the images that stops short of the last fails
static constexpr layout_with_base NVIDIA_DATA_LAST = {
    .layout = {
        .images = {
            {.offset = 0x0000, .size = 0x4000, .code_type = X86_CODE},
            {.offset = 0x4000, .size = 0xC000, .code_type = NVIDIA_DATA},
        },
        .image_count = 2,
    },
    .nvidia_data_base = 0,
};

// The falcon data token is found both as the BIT's last token and as its first, so a walk skipping either end fails
TEST(nvidia, falcon_program_table_of_other_sizes_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);

    falcon_program_table table = {};
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), OK);
    EXPECT_EQ(table.offset, 0x9000u);
    EXPECT_EQ(table.base, 0u);
    EXPECT_EQ(table.header_size, 7u);
    EXPECT_EQ(table.entry_size, 9u);
    EXPECT_EQ(table.entry_count, 29u);

    table_placement first_token = EDGE_TABLE;
    first_token.falcon_data_token_index = 0;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, first_token), OK);

    EXPECT_EQ(find_placed(rom, NVIDIA_DATA_LAST, EDGE_TABLE), OK);
}

// Pointers are written from the base the rule gives when it wraps below zero, so only the check refuses it
static constexpr layout_with_base NVIDIA_DATA_FIRST = {
    .layout = {
        .images = {
            {.offset = 0x0000, .size = 0xC000, .code_type = NVIDIA_DATA},
            {.offset = 0xC000, .size = 0x4000, .code_type = X86_CODE},
        },
        .image_count = 2,
    },
    .nvidia_data_base = 0u - 0x4000u,
};

static constexpr layout_with_base NO_NVIDIA_DATA = {
    .layout = {
        .images = {
            {.offset = 0x0000, .size = 0x4000, .code_type = X86_CODE},
            {.offset = 0x4000, .size = 0xC000, .code_type = EFI_CODE},
        },
        .image_count = 2,
    },
    .nvidia_data_base = 0,
};

static constexpr layout_with_base NO_X86_IMAGE = {
    .layout = {
        .images = {
            {.offset = 0x0000, .size = 0x4000, .code_type = EFI_CODE},
            {.offset = 0x4000, .size = 0xC000, .code_type = NVIDIA_DATA},
        },
        .image_count = 2,
    },
    .nvidia_data_base = 0,
};

TEST(nvidia, nvidia_data_base_needs_both_images_and_cannot_be_negative) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    EXPECT_EQ(find_placed(rom, NVIDIA_DATA_FIRST, EDGE_TABLE), ERR_BAD_VBIOS);
    EXPECT_EQ(find_placed(rom, NO_NVIDIA_DATA, EDGE_TABLE), ERR_BAD_VBIOS);
    EXPECT_EQ(find_placed(rom, NO_X86_IMAGE, EDGE_TABLE), ERR_BAD_VBIOS);
}

// Writes a header with the BIT's id and signature and no tokens, so it leads to no table if the scan takes it
static void write_bit_decoy(rom_window& rom, uint32_t offset, uint8_t header_size) {
    put_u16(rom, offset, BIT_ID);
    put_u32(rom, offset + BIT_SIGNATURE_FIELD, BIT_SIGNATURE);
    rom.bytes[offset + BIT_HEADER_SIZE] = header_size;
    rom.bytes[offset + BIT_TOKEN_SIZE] = TOKEN_FIELD_BYTES;
    rom.bytes[offset + BIT_TOKEN_COUNT] = 0;
}

// Before the BIT come a header whose bytes do not sum to zero, one whose size is zero and one a byte shorter
// than its fields, both summing to zero, and a valid header follows it
TEST(nvidia, bit_is_the_first_valid_header) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);

    write_bit_decoy(rom, 0x10, BIT_HEADER_FIELD_BYTES);
    zero_sum(rom, 0x10, BIT_HEADER_FIELD_BYTES, 0x10 + BIT_CHECKSUM);
    rom.bytes[0x10 + BIT_CHECKSUM]++;

    write_bit_decoy(rom, 0x20, 0);

    write_bit_decoy(rom, 0x30, BIT_HEADER_FIELD_BYTES - 1);
    zero_sum(rom, 0x30, BIT_HEADER_FIELD_BYTES - 1, 0x30 + BIT_VERSION_FIELD);

    write_bit_decoy(rom, 0x80, BIT_HEADER_FIELD_BYTES);
    zero_sum(rom, 0x80, BIT_HEADER_FIELD_BYTES, 0x80 + BIT_CHECKSUM);

    falcon_program_table table = {};
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), OK);
    EXPECT_EQ(table.offset, EDGE_TABLE.table);
}

// A 255-byte decoy header, the most its size can give, before the BIT near the VBIOS's end: the scan takes it when it
// ends exactly at the VBIOS's end, which leads to no table, and skips it for the BIT when it ends a byte past
static constexpr uint8_t LONGEST_BIT_HEADER = 255;
static constexpr uint32_t LATE_BIT = 0xFF80;

TEST(nvidia, bit_header_must_lie_inside_the_vbios) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement placement = EDGE_TABLE;
    placement.bit = LATE_BIT;
    uint32_t ending_at_end = EDGE_VBIOS_END - LONGEST_BIT_HEADER;
    uint32_t ending_past_end = ending_at_end + 1;
    falcon_program_table table = {};

    write_placement(rom, EDGE_VBIOS, placement);
    write_bit_decoy(rom, ending_at_end, LONGEST_BIT_HEADER);
    zero_sum(rom, ending_at_end, LONGEST_BIT_HEADER, ending_at_end + BIT_VERSION_FIELD);
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), ERR_BAD_VBIOS);

    write_placement(rom, EDGE_VBIOS, placement);
    write_bit_decoy(rom, ending_past_end, LONGEST_BIT_HEADER);
    zero_sum(rom, ending_past_end, LONGEST_BIT_HEADER, ending_past_end + BIT_VERSION_FIELD);
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), OK);
}

TEST(nvidia, bit_needs_its_id_and_signature) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    falcon_program_table table = {};

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);
    rom.bytes[EDGE_TABLE.bit + 1] ^= 1;
    zero_sum(rom, EDGE_TABLE.bit, EDGE_TABLE.bit_header_size, EDGE_TABLE.bit + BIT_CHECKSUM);
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), ERR_BAD_VBIOS);

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);
    rom.bytes[EDGE_TABLE.bit + BIT_SIGNATURE_FIELD + 3] ^= 1;
    zero_sum(rom, EDGE_TABLE.bit, EDGE_TABLE.bit_header_size, EDGE_TABLE.bit + BIT_CHECKSUM);
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), ERR_BAD_VBIOS);
}

// Tokens of exactly their 6 bytes of fields pass in the RTX 3080 test, so only a shorter stride is tested here
TEST(nvidia, bit_tokens_must_hold_their_fields) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement short_tokens = EDGE_TABLE;
    short_tokens.token_size = TOKEN_FIELD_BYTES - 1;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, short_tokens), ERR_BAD_VBIOS);
}

// A data size of 0x100 reads as zero in 8 bits, so only a 16-bit read of the size accepts it
TEST(nvidia, falcon_data_token_must_be_version_2_with_a_table_pointer) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement version_3 = EDGE_TABLE;
    version_3.falcon_data_version = 3;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, version_3), ERR_BAD_VBIOS);

    table_placement too_short = EDGE_TABLE;
    too_short.falcon_data_size = FALCON_DATA_FIELD_BYTES - 1;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, too_short), ERR_BAD_VBIOS);

    table_placement larger = EDGE_TABLE;
    larger.falcon_data_size = 0x100;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, larger), OK);
}

// The falcon data token is written in full before the count is lowered, so it lies right past the count
TEST(nvidia, falcon_data_token_past_the_token_count_is_not_read) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);
    rom.bytes[EDGE_TABLE.bit + BIT_TOKEN_COUNT] = EDGE_TABLE.falcon_data_token_index;
    zero_sum(rom, EDGE_TABLE.bit, EDGE_TABLE.bit_header_size, EDGE_TABLE.bit + BIT_CHECKSUM);

    falcon_program_table table = {};
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), ERR_BAD_VBIOS);
}

// The falcon data token's 6 bytes of fields start 29 bytes into the BIT, after its 13-byte header and two 8-byte
// tokens
static constexpr uint32_t EDGE_FALCON_DATA_TOKEN_FIELDS_END = 35;

TEST(nvidia, bit_tokens_and_falcon_data_must_lie_inside_the_vbios) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement token_at_end = EDGE_TABLE;
    token_at_end.bit = EDGE_VBIOS_END - EDGE_FALCON_DATA_TOKEN_FIELDS_END;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, token_at_end), OK);

    table_placement token_past_end = EDGE_TABLE;
    token_past_end.bit = EDGE_VBIOS_END - EDGE_FALCON_DATA_TOKEN_FIELDS_END + 1;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, token_past_end), ERR_BAD_VBIOS);

    table_placement data_at_end = EDGE_TABLE;
    data_at_end.falcon_data = EDGE_VBIOS_END - FALCON_DATA_FIELD_BYTES;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, data_at_end), OK);

    table_placement data_past_end = EDGE_TABLE;
    data_past_end.falcon_data = EDGE_VBIOS_END - FALCON_DATA_FIELD_BYTES + 1;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, data_past_end), ERR_BAD_VBIOS);
}

TEST(nvidia, falcon_program_table_must_be_version_1_and_hold_its_fields) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement version_0 = EDGE_TABLE;
    version_0.table_version = 0;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, version_0), ERR_BAD_VBIOS);

    table_placement version_2 = EDGE_TABLE;
    version_2.table_version = 2;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, version_2), ERR_BAD_VBIOS);

    table_placement short_header = EDGE_TABLE;
    short_header.table_header_size = 5;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, short_header), ERR_BAD_VBIOS);

    table_placement short_entries = EDGE_TABLE;
    short_entries.entry_size = 5;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, short_entries), ERR_BAD_VBIOS);
}

// `EDGE_TABLE`'s 7-byte header and 29 entries of 9 bytes span 268 bytes, the entries alone 261, both more than
// 8 bits can count
static constexpr uint32_t EDGE_TABLE_BYTES = 268;

// Tables in the RTX 3080's x86 image and 6 bytes before the base need pointers that wrap, the latter's wrapping to 0
// with its 6 bytes of header fields added. A pointer of 0x19000 lies past the VBIOS, its low 16 bits naming the table.
TEST(nvidia, falcon_program_table_must_lie_inside_the_vbios) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement entries_at_end = EDGE_TABLE;
    entries_at_end.table = EDGE_VBIOS_END - EDGE_TABLE_BYTES;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, entries_at_end), OK);

    table_placement entries_past_end = EDGE_TABLE;
    entries_past_end.table = EDGE_VBIOS_END - EDGE_TABLE_BYTES + 1;
    EXPECT_EQ(find_placed(rom, EDGE_VBIOS, entries_past_end), ERR_BAD_VBIOS);

    table_placement wrapping = RTX_3080_TABLE;
    wrapping.table = 0x1000;
    EXPECT_EQ(find_placed(rom, RTX_3080_VBIOS, wrapping), ERR_BAD_VBIOS);

    table_placement header_ending_at_base = RTX_3080_TABLE;
    header_ending_at_base.table = RTX_3080_VBIOS.nvidia_data_base - TABLE_HEADER_FIELD_BYTES;
    EXPECT_EQ(find_placed(rom, RTX_3080_VBIOS, header_ending_at_base), ERR_BAD_VBIOS);

    write_placement(rom, EDGE_VBIOS, EDGE_TABLE);
    put_u32(rom, EDGE_TABLE.falcon_data, 0x10000 + EDGE_TABLE.table);
    falcon_program_table table = {};
    EXPECT_EQ(find_table(rom, EDGE_VBIOS, table), ERR_BAD_VBIOS);
}

// The table's entry fields, FWSEC's app ids, its version 3 descriptor's fields and its signature size, restated here
// so the tests hold the lookup to the format rather than to its own constants
static constexpr uint32_t ENTRY_APPLICATION             = 0x00;
static constexpr uint32_t ENTRY_TARGET                  = 0x01;
static constexpr uint32_t ENTRY_DESCRIPTOR              = 0x02;
static constexpr uint8_t  FIRMWARE_SEC_LIC              = 0x05;
static constexpr uint8_t  FWSEC_DEBUG                   = 0x45;
static constexpr uint8_t  FWSEC_PRODUCTION              = 0x85;
static constexpr uint32_t DESCRIPTOR_HAS_VERSION        = 0x1;
static constexpr uint32_t DESCRIPTOR_VERSION_SHIFT      = 8;
static constexpr uint32_t DESCRIPTOR_SIZE_SHIFT         = 16;
static constexpr uint32_t DESCRIPTOR_STORED_SIZE        = 0x04;
static constexpr uint32_t DESCRIPTOR_SIGNATURE_SLOT     = 0x08;
static constexpr uint32_t DESCRIPTOR_INTERFACE_OFFSET   = 0x0C;
static constexpr uint32_t DESCRIPTOR_IMEM_PHYSICAL_BASE = 0x10;
static constexpr uint32_t DESCRIPTOR_CODE_SIZE          = 0x14;
static constexpr uint32_t DESCRIPTOR_IMEM_VIRTUAL_BASE  = 0x18;
static constexpr uint32_t DESCRIPTOR_DMEM_PHYSICAL_BASE = 0x1C;
static constexpr uint32_t DESCRIPTOR_DATA_SIZE          = 0x20;
static constexpr uint32_t DESCRIPTOR_ENGINE_MASK        = 0x24;
static constexpr uint32_t DESCRIPTOR_UCODE_ID           = 0x26;
static constexpr uint32_t DESCRIPTOR_SIGNATURE_COUNT    = 0x27;
static constexpr uint32_t DESCRIPTOR_SIGNATURE_VERSIONS = 0x28;
static constexpr uint32_t DESCRIPTOR_FIELD_BYTES        = 44;
static constexpr uint32_t SIGNATURE_BYTES               = 384;

// An app id the lookup has no use for
static constexpr uint8_t OTHER_PROGRAM = 0x01;

// Every entry's target id, a byte the lookup does not read, so a lookup reading the app id as 16 bits fails
static constexpr uint8_t TARGET_ID = 0x07;

// Where a synthetic VBIOS places production FWSEC's entry, by its index in the falcon program table, and FWSEC's
// descriptor, in bytes from the VBIOS's start, and the values the entry and the descriptor hold
struct fwsec_placement {
    uint8_t  entry_index;
    uint8_t  application;
    uint32_t descriptor;
    uint8_t  descriptor_flags;
    uint8_t  descriptor_version;
    uint16_t descriptor_size;
    uint32_t stored_size;
    uint32_t signature_slot;
    uint32_t interface_offset;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t imem_physical_base;
    uint32_t imem_virtual_base;
    uint32_t dmem_physical_base;
    uint16_t engine_mask;
    uint8_t  signature_count;
    uint8_t  ucode_id;
    uint16_t signature_versions;
};

// The descriptor's 44 bytes of fields first hold this value plus their offset, so no two of them are alike
static constexpr uint8_t DESCRIPTOR_PATTERN = 0x80;

// The entries right before and after FWSEC's, if any, are debug FWSEC's and a second production entry, and the rest
// are another program's. All but FWSEC's point at the base, whose zeros give no descriptor version.
static void write_entries(
    rom_window& rom,
    const layout_with_base& vbios,
    const table_placement& table,
    const fwsec_placement& placement
) {
    uint32_t index = placement.entry_index;

    for (uint32_t i = 0; i < table.entry_count; i++) {
        uint32_t entry = table.table + table.table_header_size + i * table.entry_size;
        uint8_t application = OTHER_PROGRAM;
        uint32_t pointer = 0;
        if (i == index) {
            application = placement.application;
            pointer = placement.descriptor - vbios.nvidia_data_base;
        } else if (i + 1 == index) {
            application = FWSEC_DEBUG;
        } else if (i == index + 1) {
            application = FWSEC_PRODUCTION;
        }

        rom.bytes[entry + ENTRY_APPLICATION] = application;
        rom.bytes[entry + ENTRY_TARGET] = TARGET_ID;
        put_u32(rom, entry + ENTRY_DESCRIPTOR, pointer);
    }
}

// The fields the lookup does not read keep the pattern, so a field read at the wrong offset differs from the one
// written
static void write_descriptor(rom_window& rom, const fwsec_placement& placement) {
    uint32_t descriptor = placement.descriptor;
    uint32_t header = placement.descriptor_flags
        | static_cast<uint32_t>(placement.descriptor_version) << DESCRIPTOR_VERSION_SHIFT
        | static_cast<uint32_t>(placement.descriptor_size) << DESCRIPTOR_SIZE_SHIFT;

    for (uint32_t i = 0; i < DESCRIPTOR_FIELD_BYTES; i++) {
        rom.bytes[descriptor + i] = static_cast<uint8_t>(DESCRIPTOR_PATTERN + i);
    }

    put_u32(rom, descriptor, header);
    put_u32(rom, descriptor + DESCRIPTOR_STORED_SIZE, placement.stored_size);
    put_u32(rom, descriptor + DESCRIPTOR_SIGNATURE_SLOT, placement.signature_slot);
    put_u32(rom, descriptor + DESCRIPTOR_INTERFACE_OFFSET, placement.interface_offset);
    put_u32(rom, descriptor + DESCRIPTOR_CODE_SIZE, placement.code_size);
    put_u32(rom, descriptor + DESCRIPTOR_DATA_SIZE, placement.data_size);
    put_u32(rom, descriptor + DESCRIPTOR_IMEM_PHYSICAL_BASE, placement.imem_physical_base);
    put_u32(rom, descriptor + DESCRIPTOR_IMEM_VIRTUAL_BASE, placement.imem_virtual_base);
    put_u32(rom, descriptor + DESCRIPTOR_DMEM_PHYSICAL_BASE, placement.dmem_physical_base);
    put_u16(rom, descriptor + DESCRIPTOR_ENGINE_MASK, placement.engine_mask);
    rom.bytes[descriptor + DESCRIPTOR_UCODE_ID] = placement.ucode_id;
    rom.bytes[descriptor + DESCRIPTOR_SIGNATURE_COUNT] = placement.signature_count;
    put_u16(rom, descriptor + DESCRIPTOR_SIGNATURE_VERSIONS, placement.signature_versions);
}

static void write_fwsec(
    rom_window& rom,
    const layout_with_base& vbios,
    const table_placement& table,
    const fwsec_placement& placement
) {
    write_placement(rom, vbios, table);
    write_entries(rom, vbios, table, placement);
    write_descriptor(rom, placement);
}

// The table as `find_falcon_program_table` returns it for the placement, so only `find_fwsec` can refuse
static falcon_program_table placed_table(const layout_with_base& vbios, const table_placement& table) {
    return {
        .offset = table.table,
        .base = vbios.nvidia_data_base,
        .header_size = table.table_header_size,
        .entry_size = table.entry_size,
        .entry_count = table.entry_count,
    };
}

static int32_t find_placed_fwsec(
    rom_window& rom,
    const layout_with_base& vbios,
    const table_placement& table,
    const fwsec_placement& placement,
    fwsec_layout& fwsec
) {
    write_fwsec(rom, vbios, table, placement);

    registers window(reinterpret_cast<uintptr_t>(rom.bytes));
    return find_fwsec(window, vbios.layout, placed_table(vbios, table), fwsec);
}

static int32_t find_placed_fwsec(
    rom_window& rom,
    const layout_with_base& vbios,
    const table_placement& table,
    const fwsec_placement& placement
) {
    fwsec_layout fwsec = {};
    return find_placed_fwsec(rom, vbios, table, placement, fwsec);
}

// As the RTX 3080's VBIOS places production FWSEC's entry and FWSEC's descriptor, with the fields the descriptor
// holds. The entry's pointer 0x2C9B8 does not fit in 16 bits.
static constexpr fwsec_placement RTX_3080_FWSEC = {
    .entry_index = 9,
    .application = FWSEC_PRODUCTION,
    .descriptor = 0x433B8,
    .descriptor_flags = 1,
    .descriptor_version = 3,
    .descriptor_size = 1196,
    .stored_size = 59904,
    .signature_slot = 0x5A4,
    .interface_offset = 0x1C,
    .code_size = 57856,
    .data_size = 2048,
    .imem_physical_base = 0,
    .imem_virtual_base = 0,
    .dmem_physical_base = 0,
    .engine_mask = 0x400,
    .signature_count = 3,
    .ucode_id = 9,
    .signature_versions = 0x7,
};

// The debug entry right before FWSEC's and the second production entry right after it point at zeros, so FWSEC is
// found only through the first production entry
TEST(nvidia, rtx_3080_fwsec_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_layout fwsec = {};
    EXPECT_EQ(find_placed_fwsec(rom, RTX_3080_VBIOS, RTX_3080_TABLE, RTX_3080_FWSEC, fwsec), OK);
    EXPECT_EQ(fwsec.offset, 0x43864u);
    EXPECT_EQ(fwsec.size, 59904u);
    EXPECT_EQ(fwsec.code_size, 57856u);
    EXPECT_EQ(fwsec.data_size, 2048u);
    EXPECT_EQ(fwsec.signature_slot, 0x5A4u);
    EXPECT_EQ(fwsec.signatures_offset, 0x433E4u);
    EXPECT_EQ(fwsec.signature_count, 3u);
    EXPECT_EQ(fwsec.interface_offset, 0x1Cu);
    EXPECT_EQ(fwsec.imem_physical_base, 0u);
    EXPECT_EQ(fwsec.imem_virtual_base, 0u);
    EXPECT_EQ(fwsec.dmem_physical_base, 0u);
    EXPECT_EQ(fwsec.engine_mask, 0x400u);
    EXPECT_EQ(fwsec.ucode_id, 9u);
    EXPECT_EQ(fwsec.signature_versions, 0x7u);
}

// FWSEC ends at 0x89000, past 0x72E00, the RTX 3080's VBIOS's end less the base, which bounds the descriptor's
// pointer but not FWSEC
TEST(nvidia, fwsec_ending_past_the_vbios_end_less_the_base_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement past_end_less_base = RTX_3080_FWSEC;
    past_end_less_base.descriptor = 0x7A154;

    fwsec_layout fwsec = {};
    EXPECT_EQ(find_placed_fwsec(rom, RTX_3080_VBIOS, RTX_3080_TABLE, past_end_less_base, fwsec), OK);
    EXPECT_EQ(fwsec.offset, 0x7A600u);
}

// FWSEC's entry is `EDGE_TABLE`'s last, and its descriptor sets a flag besides the version flag. Its stored size
// rounded up ends FWSEC at the VBIOS's end, its code and data fill FWSEC, and its signature slot ends its data.
static constexpr fwsec_placement EDGE_FWSEC = {
    .entry_index = 28,
    .application = FWSEC_PRODUCTION,
    .descriptor = 0xDF54,
    .descriptor_flags = 3,
    .descriptor_version = 3,
    .descriptor_size = DESCRIPTOR_FIELD_BYTES + SIGNATURE_BYTES,
    .stored_size = 0x1E01,
    .signature_slot = 0x680,
    .interface_offset = 0x80402010,
    .code_size = 0x1700,
    .data_size = 0x800,
    .imem_physical_base = 0x81412111,
    .imem_virtual_base = 0x82422212,
    .dmem_physical_base = 0x83432313,
    .engine_mask = 0x8414,
    .signature_count = 1,
    .ucode_id = 0xFF,
    .signature_versions = 0x8001,
};

// FWSEC's entry is found both as `EDGE_TABLE`'s last entry and as its first, so a walk skipping either end fails.
// The interface offset's and the bases' bytes all differ, and the mask, ucode id and versions set their top bits.
TEST(nvidia, fwsec_at_its_limits_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_layout fwsec = {};
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, EDGE_FWSEC, fwsec), OK);
    EXPECT_EQ(fwsec.offset, 0xE100u);
    EXPECT_EQ(fwsec.size, 0x1F00u);
    EXPECT_EQ(fwsec.code_size, 0x1700u);
    EXPECT_EQ(fwsec.data_size, 0x800u);
    EXPECT_EQ(fwsec.signature_slot, 0x680u);
    EXPECT_EQ(fwsec.signatures_offset, 0xDF80u);
    EXPECT_EQ(fwsec.signature_count, 1u);
    EXPECT_EQ(fwsec.interface_offset, 0x80402010u);
    EXPECT_EQ(fwsec.imem_physical_base, 0x81412111u);
    EXPECT_EQ(fwsec.imem_virtual_base, 0x82422212u);
    EXPECT_EQ(fwsec.dmem_physical_base, 0x83432313u);
    EXPECT_EQ(fwsec.engine_mask, 0x8414u);
    EXPECT_EQ(fwsec.ucode_id, 0xFFu);
    EXPECT_EQ(fwsec.signature_versions, 0x8001u);

    fwsec_placement first_entry = EDGE_FWSEC;
    first_entry.entry_index = 0;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, first_entry), OK);
}

// The descriptor runs 4 bytes past its one signature, so FWSEC starts where the descriptor's size says rather than
// right after the signature
TEST(nvidia, fwsec_starts_where_its_descriptor_size_says) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement padded = EDGE_FWSEC;
    padded.descriptor = EDGE_FWSEC.descriptor - 4;
    padded.descriptor_size = EDGE_FWSEC.descriptor_size + 4;

    fwsec_layout fwsec = {};
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, padded, fwsec), OK);
    EXPECT_EQ(fwsec.offset, 0xE100u);
    EXPECT_EQ(fwsec.signatures_offset, 0xDF7Cu);
}

// FWSEC holds 0x100 bytes past its data, so its code and data need only fit in it
TEST(nvidia, fwsec_with_room_past_its_data_is_found) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement room_past_data = EDGE_FWSEC;
    room_past_data.data_size = 0x700;
    room_past_data.signature_slot = 0x580;

    fwsec_layout fwsec = {};
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, room_past_data, fwsec), OK);
    EXPECT_EQ(fwsec.size, 0x1F00u);
    EXPECT_EQ(fwsec.data_size, 0x700u);
    EXPECT_EQ(fwsec.signature_slot, 0x580u);
}

// FWSEC's entry is the table's last, so the table has no production FWSEC entry when that entry's app id is one below
// or above production FWSEC's, or `FIRMWARE_SEC_LIC`'s, which NVIDIA's open kernel modules also take for FWSEC
TEST(nvidia, fwsec_entry_must_have_production_fwsec_app_id) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement below = EDGE_FWSEC;
    below.application = FWSEC_PRODUCTION - 1;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, below), ERR_BAD_VBIOS);

    fwsec_placement above = EDGE_FWSEC;
    above.application = FWSEC_PRODUCTION + 1;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, above), ERR_BAD_VBIOS);

    fwsec_placement firmware_sec_lic = EDGE_FWSEC;
    firmware_sec_lic.application = FIRMWARE_SEC_LIC;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, firmware_sec_lic), ERR_BAD_VBIOS);
}

// The entries are written in full before the table's count is lowered, so FWSEC's entry lies right past the count
TEST(nvidia, fwsec_entry_past_the_entry_count_is_not_read) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    table_placement lowered = EDGE_TABLE;
    lowered.entry_count = EDGE_FWSEC.entry_index;

    write_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, EDGE_FWSEC);
    rom.bytes[EDGE_TABLE.table + TABLE_ENTRY_COUNT] = lowered.entry_count;

    fwsec_layout fwsec = {};
    registers window(reinterpret_cast<uintptr_t>(rom.bytes));
    EXPECT_EQ(find_fwsec(window, EDGE_VBIOS.layout, placed_table(EDGE_VBIOS, lowered), fwsec), ERR_BAD_VBIOS);
}

// A descriptor in the RTX 3080's x86 image lies before the base, so its entry's pointer wraps around to name it, and
// one whose fields end at the base needs a pointer whose sum with their 44 bytes wraps around 32 bits to 0
TEST(nvidia, fwsec_descriptor_pointer_must_not_wrap_below_the_base) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement wrapping = RTX_3080_FWSEC;
    wrapping.descriptor = 0x2000;
    EXPECT_EQ(find_placed_fwsec(rom, RTX_3080_VBIOS, RTX_3080_TABLE, wrapping), ERR_BAD_VBIOS);

    fwsec_placement fields_ending_at_base = RTX_3080_FWSEC;
    fields_ending_at_base.descriptor = RTX_3080_VBIOS.nvidia_data_base - DESCRIPTOR_FIELD_BYTES;
    EXPECT_EQ(find_placed_fwsec(rom, RTX_3080_VBIOS, RTX_3080_TABLE, fields_ending_at_base), ERR_BAD_VBIOS);
}

// Only the version flag is cleared, so a lookup testing the whole flags byte would take EDGE's other flag for it,
// and version 0x83 holds 3 in its low bits, so a lookup reading fewer than 8 bits of version would take it for 3
TEST(nvidia, fwsec_descriptor_must_be_version_3) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement without_version = EDGE_FWSEC;
    without_version.descriptor_flags = EDGE_FWSEC.descriptor_flags & ~DESCRIPTOR_HAS_VERSION;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, without_version), ERR_BAD_VBIOS);

    fwsec_placement version_2 = EDGE_FWSEC;
    version_2.descriptor_version = 2;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, version_2), ERR_BAD_VBIOS);

    fwsec_placement version_4 = EDGE_FWSEC;
    version_4.descriptor_version = 4;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, version_4), ERR_BAD_VBIOS);

    fwsec_placement version_0x83 = EDGE_FWSEC;
    version_0x83.descriptor_version = 0x83;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, version_0x83), ERR_BAD_VBIOS);
}

// The shortened descriptor starts a byte later, so FWSEC still starts where it does in `EDGE_FWSEC`. The fields
// and 171 signatures need 65708 bytes, more than the 16-bit size can give, which a 16-bit sum wraps to 172.
TEST(nvidia, fwsec_descriptor_must_list_and_hold_its_signatures) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement without_signatures = EDGE_FWSEC;
    without_signatures.signature_count = 0;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, without_signatures), ERR_BAD_VBIOS);

    fwsec_placement byte_short = EDGE_FWSEC;
    byte_short.descriptor = EDGE_FWSEC.descriptor + 1;
    byte_short.descriptor_size = EDGE_FWSEC.descriptor_size - 1;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, byte_short), ERR_BAD_VBIOS);

    fwsec_placement too_many = EDGE_FWSEC;
    too_many.signature_count = 171;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, too_many), ERR_BAD_VBIOS);
}

// The descriptor a byte later ends FWSEC a byte past the VBIOS, a stored size of 0x11E01 passes in its low 16 bits,
// and one of 0xFFFFF000 ends FWSEC past the VBIOS only in 64 bits, its 32-bit end wrapping around to 0xD100
TEST(nvidia, fwsec_must_end_inside_the_vbios) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement byte_past_end = EDGE_FWSEC;
    byte_past_end.descriptor = EDGE_FWSEC.descriptor + 1;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, byte_past_end), ERR_BAD_VBIOS);

    fwsec_placement upper_bits = EDGE_FWSEC;
    upper_bits.stored_size = 0x11E01;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, upper_bits), ERR_BAD_VBIOS);

    fwsec_placement wrapping = EDGE_FWSEC;
    wrapping.stored_size = 0xFFFFF000;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, wrapping), ERR_BAD_VBIOS);
}

// Code of 0x11700 bytes and data of 0x10800 pass in their low 16 bits, and 0xFFFFFF00 bytes of code and 0x800 of
// data add up to 0x700 in 32 bits
TEST(nvidia, fwsec_code_and_data_must_fit_in_its_size) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement byte_too_many = EDGE_FWSEC;
    byte_too_many.data_size = 0x801;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, byte_too_many), ERR_BAD_VBIOS);

    fwsec_placement code_upper_bits = EDGE_FWSEC;
    code_upper_bits.code_size = 0x11700;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, code_upper_bits), ERR_BAD_VBIOS);

    fwsec_placement data_upper_bits = EDGE_FWSEC;
    data_upper_bits.data_size = 0x10800;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, data_upper_bits), ERR_BAD_VBIOS);

    fwsec_placement wrapping = EDGE_FWSEC;
    wrapping.code_size = 0xFFFFFF00;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, wrapping), ERR_BAD_VBIOS);
}

// With 0x700 bytes of data, a slot ending a byte past them still lies inside FWSEC. A slot at 0x10680 passes in
// its low 16 bits, and one at 0xFFFFFF00 ends at 0x80 in 32 bits.
TEST(nvidia, fwsec_signature_slot_must_lie_inside_its_data) {
    rom_window rom;
    ASSERT_NOT_NULL(rom.bytes);

    fwsec_placement byte_past_end = EDGE_FWSEC;
    byte_past_end.signature_slot = 0x681;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, byte_past_end), ERR_BAD_VBIOS);

    fwsec_placement past_shorter_data = EDGE_FWSEC;
    past_shorter_data.data_size = 0x700;
    past_shorter_data.signature_slot = 0x581;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, past_shorter_data), ERR_BAD_VBIOS);

    fwsec_placement upper_bits = EDGE_FWSEC;
    upper_bits.signature_slot = 0x10680;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, upper_bits), ERR_BAD_VBIOS);

    fwsec_placement wrapping = EDGE_FWSEC;
    wrapping.signature_slot = 0xFFFFFF00;
    EXPECT_EQ(find_placed_fwsec(rom, EDGE_VBIOS, EDGE_TABLE, wrapping), ERR_BAD_VBIOS);
}

// Only a firmware directory no package installed declines, and every other `fw::file` result fails bring-up
TEST(nvidia, only_an_uninstalled_firmware_directory_declines) {
    EXPECT_EQ(to_bring_up_result(fw::OK), OK);
    EXPECT_EQ(to_bring_up_result(fw::ERR_NOT_INSTALLED), DECLINED);
    EXPECT_EQ(to_bring_up_result(fw::ERR_MISSING), ERR_BAD_FIRMWARE);
    EXPECT_EQ(to_bring_up_result(fw::ERR_UNREADABLE), ERR_BAD_FIRMWARE);
    EXPECT_EQ(to_bring_up_result(fw::ERR_INVALID_ARG), ERR_BAD_FIRMWARE);
}

// No package installs a boot directory of this name
static constexpr chip_firmware UNINSTALLED_BOOT_DIRECTORY = {
    .gsp_image = "gsp_test.bin",
    .boot_directory = "test_boot_directory",
    .gsp_signature_section = ".fwsignature_test",
};

TEST(nvidia, chip_whose_boot_directory_is_not_installed_declines) {
    firmware_files files;
    EXPECT_EQ(open_firmware_files(UNINSTALLED_BOOT_DIRECTORY, files), DECLINED);
}

// The longest name, nvidia/535.183.01/<boot directory>/booter_unload.bin, fills the driver's 128 bytes with its NUL
// when the boot directory has this length
static constexpr size_t LONGEST_BOOT_DIRECTORY_BYTES = 91;

static void fill_boot_directory(char* boot_directory, size_t length) {
    string::memset(boot_directory, 'a', length);
    boot_directory[length] = '\0';
}

// The longest boot directory still builds its names and declines, since no package installs it
TEST(nvidia, firmware_names_too_long_to_build_fail) {
    char boot_directory[LONGEST_BOOT_DIRECTORY_BYTES + 2];
    chip_firmware firmware = {
        .gsp_image = "gsp_test.bin",
        .boot_directory = boot_directory,
        .gsp_signature_section = ".fwsignature_test",
    };

    fill_boot_directory(boot_directory, LONGEST_BOOT_DIRECTORY_BYTES);
    firmware_files longest_files;
    EXPECT_EQ(open_firmware_files(firmware, longest_files), DECLINED);

    fill_boot_directory(boot_directory, LONGEST_BOOT_DIRECTORY_BYTES + 1);
    firmware_files overlong_files;
    EXPECT_EQ(open_firmware_files(firmware, overlong_files), ERR_BAD_FIRMWARE);
}
