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

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_VBIOS_H
