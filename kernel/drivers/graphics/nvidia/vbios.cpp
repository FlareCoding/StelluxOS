#include "drivers/graphics/nvidia/vbios.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// Each structure opens with its signature, and NVIDIA's own images carry signatures of their own
constexpr uint16_t IMAGE_SIGNATURE           = 0xAA55;
constexpr uint16_t NVIDIA_IMAGE_SIGNATURE    = 0x4E56;
constexpr uint32_t PCI_DATA_SIGNATURE        = 0x52494350; // "PCIR"
constexpr uint32_t NVIDIA_PCI_DATA_SIGNATURE = 0x5344504E; // "NPDS"
constexpr uint32_t EXTENSION_SIGNATURE       = 0x4544504E; // "NPDE"

// Image header fields, as byte offsets from the start of the image
constexpr uint32_t HEADER_PCI_DATA_POINTER = 0x18; // 16 bits, from the start of the image

// PCI data structure fields, as byte offsets from its start
constexpr uint32_t PCI_DATA_LENGTH       = 0x0A; // 16 bits, bytes
constexpr uint32_t PCI_DATA_IMAGE_BLOCKS = 0x10; // 16 bits
constexpr uint32_t PCI_DATA_CODE_TYPE    = 0x14; // 8 bits
constexpr uint32_t PCI_DATA_INDICATOR    = 0x15; // 8 bits, bit 7 marks the last image

// NVIDIA's extension fields, as byte offsets from its start
constexpr uint32_t EXTENSION_LENGTH       = 0x06; // 16 bits, bytes
constexpr uint32_t EXTENSION_IMAGE_BLOCKS = 0x08; // 16 bits
constexpr uint32_t EXTENSION_FLAGS        = 0x0A; // 8 bits

// The extension starts at the first 16-byte boundary at or after the end of the PCI data structure
constexpr uint32_t EXTENSION_ALIGNMENT = 16;

// Bytes of each structure up to the end of the last field the walk reads
constexpr uint32_t HEADER_READ_BYTES    = HEADER_PCI_DATA_POINTER + 2;
constexpr uint32_t PCI_DATA_READ_BYTES  = PCI_DATA_INDICATOR + 1;
constexpr uint32_t EXTENSION_READ_BYTES = EXTENSION_FLAGS + 1;

// Bit 7 of the PCI data structure's indicator and of the extension's flags
constexpr uint8_t LAST_IMAGE = 0x80;

// Image sizes count blocks of 512 bytes
constexpr uint32_t IMAGE_BLOCK_BYTES = 512;

// A `registers` view reads only aligned 32-bit words, so fields are built a byte at a time from the words holding them
static uint8_t read_rom_u8(const registers& rom, uint32_t offset) {
    uint32_t word = rom.read(offset & ~3u);
    uint32_t shift = (offset & 3) * 8;
    return static_cast<uint8_t>(word >> shift);
}

static uint16_t read_rom_u16(const registers& rom, uint32_t offset) {
    uint32_t low = read_rom_u8(rom, offset);
    uint32_t high = read_rom_u8(rom, offset + 1);
    return static_cast<uint16_t>(low | (high << 8));
}

static uint32_t read_rom_u32(const registers& rom, uint32_t offset) {
    uint32_t low = read_rom_u16(rom, offset);
    uint32_t high = read_rom_u16(rom, offset + 2);
    return low | (high << 16);
}

static bool is_inside_rom_window(uint32_t offset, uint32_t bytes) {
    return offset <= ROM_WINDOW_BYTES && bytes <= ROM_WINDOW_BYTES - offset;
}

// Reads the size and code type of the image at `offset` from the structures that
// describe it, and whether the chain ends with it
static int32_t read_image_layout(
    const registers& rom,
    uint32_t offset,
    vbios_image& image_out,
    bool& is_last_out
) {
    bool has_header = is_inside_rom_window(offset, HEADER_READ_BYTES);
    uint16_t signature = has_header ? read_rom_u16(rom, offset) : 0;
    if (signature != IMAGE_SIGNATURE && signature != NVIDIA_IMAGE_SIGNATURE) {
        log::error("nvidia: no VBIOS image signature at 0x%X", offset);
        return ERR_BAD_VBIOS;
    }

    uint32_t pci_data_offset = offset + read_rom_u16(rom, offset + HEADER_PCI_DATA_POINTER);
    bool has_pci_data = is_inside_rom_window(pci_data_offset, PCI_DATA_READ_BYTES);
    uint32_t pci_data_signature = has_pci_data ? read_rom_u32(rom, pci_data_offset) : 0;
    if (pci_data_signature != PCI_DATA_SIGNATURE && pci_data_signature != NVIDIA_PCI_DATA_SIGNATURE) {
        log::error("nvidia: the VBIOS image at 0x%X has no PCI data structure", offset);
        return ERR_BAD_VBIOS;
    }

    uint32_t blocks = read_rom_u16(rom, pci_data_offset + PCI_DATA_IMAGE_BLOCKS);
    uint8_t code_type = read_rom_u8(rom, pci_data_offset + PCI_DATA_CODE_TYPE);
    bool is_last = (read_rom_u8(rom, pci_data_offset + PCI_DATA_INDICATOR) & LAST_IMAGE) != 0;

    uint32_t pci_data_end = pci_data_offset + read_rom_u16(rom, pci_data_offset + PCI_DATA_LENGTH);
    uint32_t extension_offset = (pci_data_end + EXTENSION_ALIGNMENT - 1) & ~(EXTENSION_ALIGNMENT - 1);
    bool has_extension = is_inside_rom_window(extension_offset, EXTENSION_READ_BYTES)
        && read_rom_u32(rom, extension_offset) == EXTENSION_SIGNATURE;
    if (has_extension) {
        if (read_rom_u16(rom, extension_offset + EXTENSION_LENGTH) < EXTENSION_READ_BYTES) {
            log::error("nvidia: the VBIOS image at 0x%X has an extension too short for its flags", offset);
            return ERR_BAD_VBIOS;
        }

        blocks = read_rom_u16(rom, extension_offset + EXTENSION_IMAGE_BLOCKS);
        is_last = (read_rom_u8(rom, extension_offset + EXTENSION_FLAGS) & LAST_IMAGE) != 0;
    }

    uint32_t size = blocks * IMAGE_BLOCK_BYTES;
    if (size == 0) {
        log::error("nvidia: the VBIOS image at 0x%X has a size of zero", offset);
        return ERR_BAD_VBIOS;
    }

    if (!is_inside_rom_window(offset, size)) {
        log::error("nvidia: the VBIOS image at 0x%X is %u bytes long and ends past the ROM window", offset, size);
        return ERR_BAD_VBIOS;
    }

    image_out.offset = offset;
    image_out.size = size;
    image_out.code_type = code_type;
    is_last_out = is_last;

    return OK;
}

int32_t read_vbios_layout(const registers& rom, vbios_layout& layout_out) {
    layout_out.image_count = 0;

    uint32_t offset = 0;
    bool is_last = false;
    while (!is_last) {
        if (layout_out.image_count == VBIOS_MAX_IMAGES) {
            log::error("nvidia: the VBIOS chain does not end within %zu images", VBIOS_MAX_IMAGES);
            return ERR_BAD_VBIOS;
        }

        vbios_image& image = layout_out.images[layout_out.image_count];
        int32_t rc = read_image_layout(rom, offset, image, is_last);
        if (rc != OK) {
            return rc;
        }

        layout_out.image_count++;
        offset += image.size;
    }

    return OK;
}

} // namespace drivers::nvidia
