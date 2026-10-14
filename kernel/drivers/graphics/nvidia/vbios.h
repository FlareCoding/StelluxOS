// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_VBIOS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_VBIOS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// Byte offset in BAR0 of the window onto the card's flash ROM, which starts with
// the VBIOS: the card's own boot code and data
constexpr uint32_t ROM_WINDOW = 0x300000;

// The window spans 1 MiB, so every VBIOS image must end inside it
constexpr uint32_t ROM_WINDOW_BYTES = 0x100000;

// The most images a layout holds. A chain that does not end within this many
// images is rejected as malformed.
constexpr size_t VBIOS_MAX_IMAGES = 8;

struct vbios_image {
    uint32_t offset;    // bytes from the start of the VBIOS
    uint32_t size;      // bytes
    uint8_t  code_type; // what the image holds, such as 0x00 x86 code, 0x03 EFI code or 0xE0 NVIDIA's own data
};

/**
 * Where the images of a card's VBIOS sit, in ROM order. The VBIOS is a chain
 * of images, each starting where the previous one ends. An image opens with a
 * header pointing to its PCI data structure, which gives the image's size,
 * its code type and whether it is the last image. NVIDIA may follow the PCI
 * data structure with an extension, and when one is present, its size and
 * last-image flag take precedence. A VBIOS can mark its EFI image last in the
 * PCI data structure, for system firmware that knows only the standard
 * images, while the extension's last-image flag is clear and NVIDIA's own
 * images follow. A walk that reads only the PCI data structures stops at the
 * EFI image and misses them.
 *
 *   image 0                                            image 1
 *   +--------+-----+-----------+-----+-----------+-----+--------+-----
 *   | header | ... | PCI data  | ... | NVIDIA's  | ... | header | ...
 *   |        |     | structure |     | extension |     |        |
 *   +--------+-----+-----------+-----+-----------+-----+--------+-----
 *   offset 0                                           offset 0 + size of image 0
 */
struct vbios_layout {
    vbios_image images[VBIOS_MAX_IMAGES];
    size_t      image_count;
};

/**
 * Where the VBIOS keeps its falcon program table, which lists the programs the
 * VBIOS stores for the card's microcontrollers, called falcons. Each entry
 * says what its program does and points to the program's descriptor, the
 * record that says where the program lies.
 * Pointers into NVIDIA's data, the entries' pointers among them, count bytes
 * from `base`: the offset of the first image of NVIDIA's data less the size of
 * the first x86 image. When the x86 image is the VBIOS's first image, the base
 * is the total size of the images between it and NVIDIA's data, such as an EFI
 * image.
 */
struct falcon_program_table {
    uint32_t offset;      // bytes from the start of the VBIOS
    uint32_t base;        // bytes from the start of the VBIOS
    uint8_t  header_size; // bytes, with the entries right after the header
    uint8_t  entry_size;  // bytes
    uint8_t  entry_count;
};

/**
 * Where FWSEC lies in the VBIOS. FWSEC is a program that NVIDIA signs and
 * stores in the VBIOS. The GSP's falcon runs it before the GSP's firmware
 * starts, to fence off the protected memory that firmware boots into. The
 * program is its code followed by its data, and its signatures follow its
 * descriptor's fields. The falcon checks the signature at the signature slot
 * in the program's data, so before FWSEC runs, the slot must hold the
 * signature for the version that the chip's fuses, bits burned into the chip
 * for good, record for FWSEC.
 */
struct fwsec_layout {
    uint32_t offset;            // bytes from the start of the VBIOS
    uint32_t size;              // bytes, the descriptor's stored size rounded up to a multiple of 256
    uint32_t code_size;         // bytes of code at the program's start
    uint32_t data_size;         // bytes of data right after the code
    uint32_t signature_slot;    // bytes from the start of the data
    uint32_t signatures_offset; // bytes from the start of the VBIOS to the first signature
    uint8_t  signature_count;   // signatures of `FWSEC_SIGNATURE_BYTES` each, back to back
};

// Bytes of each of FWSEC's signatures, an RSA-3072 signature
constexpr uint32_t FWSEC_SIGNATURE_BYTES = 384;

/**
 * @brief Walk the chain of VBIOS images through the card's ROM window,
 * reading only the structures that describe each image.
 * @param rom A view whose offset 0 is the first byte of the ROM window, and
 * which must cover all `ROM_WINDOW_BYTES` of it.
 * @param layout_out Receives the images when the result is `OK`.
 * @return `OK`, or `ERR_BAD_VBIOS` when an image has no signature, no PCI
 * data structure, an extension too short for its flags or a size of zero,
 * when an image ends past the ROM window, or when the chain does not end
 * within `VBIOS_MAX_IMAGES` images. The cause is logged.
 */
int32_t read_vbios_layout(const registers& rom, vbios_layout& layout_out);

/**
 * @brief Find the falcon program table through the BIT, NVIDIA's index of the
 * VBIOS: the first header from the VBIOS's start that has the BIT's id and
 * signature, is at least its 12 bytes of fields long, lies inside the VBIOS
 * and whose bytes sum to zero. Each of the BIT's tokens names a kind of data
 * with its version, size and offset from the VBIOS's start, and the falcon
 * data token names the data about the falcons' programs. The data of the
 * first falcon data token of version 2 with at least 4 bytes of data holds the
 * table's pointer. The table must be version 1, with a header and entries of
 * at least their 6 bytes of fields.
 * @param rom A view whose offset 0 is the first byte of the ROM window, and
 * which must cover all `ROM_WINDOW_BYTES` of it.
 * @param layout The VBIOS's images, which must be as `read_vbios_layout`
 * returns them.
 * @param table_out Receives the table when the result is `OK`. The table then
 * lies inside the VBIOS with all its entries.
 * @return `OK`, or `ERR_BAD_VBIOS` when the layout has no x86 image or no
 * image of NVIDIA's data, the first image of NVIDIA's data starts at an offset
 * below the first x86 image's size, no BIT is found, the BIT's tokens are
 * shorter than their 6 bytes of fields, it has no falcon data token of version
 * 2 with at least 4 bytes of data, the table is not version 1 or has a header
 * or entries shorter than their 6 bytes of fields, or the fields of a token
 * read on the way, the falcon data's first 4 bytes, the table's 6 bytes of
 * header fields at the base plus its pointer or the table with all its entries
 * end past the end of the VBIOS's last image. The cause is logged.
 */
int32_t find_falcon_program_table(const registers& rom, const vbios_layout& layout, falcon_program_table& table_out);

/**
 * @brief Find FWSEC through the falcon program table. The table's first entry
 * for production FWSEC, the build signed for production cards rather than
 * debug ones, points to FWSEC's descriptor, which must be version 3, list at
 * least one signature and be large enough for its 44 bytes of fields and its
 * signatures. A version 2 descriptor lays out other fields and is refused.
 * FWSEC starts where the descriptor ends.
 * @param rom A view whose offset 0 is the first byte of the ROM window, and
 * which must cover all `ROM_WINDOW_BYTES` of it.
 * @param layout The VBIOS's images, which must be as `read_vbios_layout`
 * returns them.
 * @param table The falcon program table, which must be as
 * `find_falcon_program_table` returns it for `layout`.
 * @param fwsec_out Receives where FWSEC lies when the result is `OK`. FWSEC
 * then lies inside the VBIOS, its code and data fit in its `size`, its
 * signature slot lies inside its data, and its `signature_count` signatures,
 * at least one, lie between the descriptor's fields and FWSEC.
 * @return `OK`, or `ERR_BAD_VBIOS` when the table has no production FWSEC
 * entry, the descriptor's 44 bytes of fields at the table's base plus the
 * entry's pointer end past the end of the VBIOS's last image, the descriptor
 * gives no version or one other than 3, lists no signatures or is smaller
 * than its fields and signatures, FWSEC ends past the end of the VBIOS's
 * last image, its code and data do not fit in it, or its signature slot does
 * not fit in its data. The cause is logged.
 */
int32_t find_fwsec(
    const registers& rom,
    const vbios_layout& layout,
    const falcon_program_table& table,
    fwsec_layout& fwsec_out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_VBIOS_H
