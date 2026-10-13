#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H

#include "drivers/pci_driver.h"

namespace drivers {

/**
 * The driver for NVIDIA GPUs, one object per card. The PCI framework creates
 * it on the unprivileged heap for every NVIDIA display controller, calls
 * `attach()` on the boot thread before userland starts, and frees it after any
 * result but `OK`. The driver owns its BAR0 mapping, which `detach()` unmaps.
 *
 * `attach()` declines the card when a driver display is already registered
 * with gfxfb, or when the chip table does not list the card's chip. Bring-up
 * ends at the chip check, so a listed chip is logged and declined as well.
 * `attach()` fails with a negative error when BAR0 cannot be mapped or the
 * card does not respond. Whether `attach()` declines or fails, the screen is
 * left as it was, and the card keeps only the memory and I/O decoding that
 * `attach()` turned on. That decoding stays on because the boot framebuffer
 * may be in this card's memory, which the CPU reaches only while memory
 * decoding is on.
 */
class nvidia_gpu_driver : public pci_driver {
public:
    nvidia_gpu_driver(pci::device* dev) : pci_driver("nvidia_gpu", dev) {}

    int32_t attach() override;
    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
