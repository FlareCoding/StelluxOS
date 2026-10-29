// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_METHODS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_METHODS_H

#include "common/types.h"
#include "drivers/graphics/nvidia/rm.h"

namespace drivers::nvidia {

// The most values one of a window image's methods carries: the update timestamp's two halves share one method
constexpr uint32_t WINDOW_METHOD_VALUES_MAX = 2;

// One method for a window channel: a register offset of the window's class, and the `count` values written to that
// register and the ones after it in turn
struct window_method {
    uint32_t method;
    uint32_t values[WINDOW_METHOD_VALUES_MAX];
    uint32_t count;
};

// The methods of one window image, which take 109 words of a push buffer with their headers
constexpr uint32_t WINDOW_IMAGE_METHODS = 54;

// A window image's methods in the order they are written
struct window_image_methods {
    window_method methods[WINDOW_IMAGE_METHODS];
};

// An image for a window to show unscaled, X8R8G8B8 pixels in rows. Both handles must be nonzero, and each value
// must fit its methods' field: window below 32, width and height below 65536, pitch a multiple of 64 below 512 KiB
struct window_image {
    uint8_t    window;                    // the window, whose channel the methods are written to
    rm::handle surface_context_dma;       // the surface's context DMA, bound to the window's channel
    uint32_t   width;                     // in pixels, what the window reads and shows
    uint32_t   height;                    // in rows
    uint32_t   pitch;                     // the bytes from one row's start to the next
    rm::handle look_up_table_context_dma; // the look-up table surface's, bound to the window's channel
};

// How the update showing a window image reports and takes effect. The slot must fit its method's field, below 256
struct window_image_update {
    rm::handle notifier_context_dma; // the window notifier's context DMA, 0 for none
    uint32_t   notifier_slot;        // the 16-byte slot of the window notifier the update writes, unused for none
    bool       interlocks_core;      // whether the update takes effect only with the core channel's next update
};

/**
 * @brief Give the window channel's methods showing `image`, then the update
 * applying them, as NVIDIA's modesetting module writes a flip of this state:
 * one surface of X8R8G8B8 pixels in rows shown whole and unscaled where the
 * window already lies, from a vertical blank for at least a frame, a notifier
 * written with no event or none, no timestamp, semaphore, flip lock, variable
 * refresh rate, rotation, reflection or tone mapping, the identity color
 * matrix, an input look-up table at the look-up table surface's start and
 * opaque composition. The methods are the scan direction, the notifier, the
 * present control, the surface's six slots, eye by eye and plane by plane (its
 * context DMA and offset 0 in slot 0, none in the others), its size as the
 * size read and shown, its storage and each plane's pitch, no timestamp and
 * its format. Then the color pipeline: the identity format matrix, two input
 * scaler taps, the last matrix of the color space conversion (CSC) stages
 * disabled, the input look-up table read directly by 10-bit color, and the
 * window's pixels covering what lies below at the depth of a head's main
 * layer. Then no semaphores, the interlock flags (the core channel's when
 * `update` asks, and the window's own channel's) and the update. The other CSC
 * stages' controls are not written, so they keep what the window holds: NVIDIA
 * writes them disabled only for a window whose capabilities report those
 * stages, and the driver reads no window capabilities.
 * @param image The image, within the bounds `window_image` gives.
 * @param update The update's notifier and interlock with the core channel.
 * @param out Receives the methods.
 */
void build_window_image_methods(
    const window_image& image,
    const window_image_update& update,
    window_image_methods& out
);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_WINDOW_METHODS_H
