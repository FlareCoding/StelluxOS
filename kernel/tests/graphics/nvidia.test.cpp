#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
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
using drivers::nvidia::find_chip;
using drivers::nvidia::firmware_files;
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

// The RTX 3080's identity decodes to the GA102 entry of the chip table
TEST(nvidia, rtx_3080_decodes_as_ga102) {
    EXPECT_EQ(decode_chip_id(RTX_3080_BOOT_0), GA102_CHIP_ID);
    EXPECT_EQ(decode_chip_revision(RTX_3080_BOOT_0), RTX_3080_REVISION);

    const chip_info* chip = find_chip(GA102_CHIP_ID);
    ASSERT_NOT_NULL(chip);
    EXPECT_EQ(chip->id, GA102_CHIP_ID);
    EXPECT_STREQ(chip->name, "GA102");
}

// GA104 shares GA102's architecture but has no chip table entry
static constexpr uint16_t GA104_CHIP_ID = 0x174;

// Sharing a listed chip's architecture does not give a chip an entry
TEST(nvidia, ga104_has_no_entry) {
    EXPECT_NULL(find_chip(GA104_CHIP_ID));
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
};

TEST(nvidia, chip_whose_boot_directory_is_not_installed_declines) {
    firmware_files files;
    EXPECT_EQ(open_firmware_files(UNINSTALLED_BOOT_DIRECTORY, files), DECLINED);
}

// The longest name, nvidia/535.183.01/<boot directory>/booter_load.bin, fills the driver's 128 bytes with its NUL
// when the boot directory has this length
static constexpr size_t LONGEST_BOOT_DIRECTORY_BYTES = 93;

static void fill_boot_directory(char* boot_directory, size_t length) {
    string::memset(boot_directory, 'a', length);
    boot_directory[length] = '\0';
}

// The longest boot directory still builds its names and declines, since no package installs it
TEST(nvidia, firmware_names_too_long_to_build_fail) {
    char boot_directory[LONGEST_BOOT_DIRECTORY_BYTES + 2];
    chip_firmware firmware = {.gsp_image = "gsp_test.bin", .boot_directory = boot_directory};

    fill_boot_directory(boot_directory, LONGEST_BOOT_DIRECTORY_BYTES);
    firmware_files longest_files;
    EXPECT_EQ(open_firmware_files(firmware, longest_files), DECLINED);

    fill_boot_directory(boot_directory, LONGEST_BOOT_DIRECTORY_BYTES + 1);
    firmware_files overlong_files;
    EXPECT_EQ(open_firmware_files(firmware, overlong_files), ERR_BAD_FIRMWARE);
}
