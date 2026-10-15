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
 * with gfxfb, when the chip table does not list the card's chip, or when a
 * directory the chip's firmware files belong in is not installed, as in an
 * image without the nvidia-firmware package. Bring-up logs the layout of the
 * card's VBIOS, opens the chip's firmware files, checks the GSP image's
 * firmware version and the headers of the bootloader and booter load, and
 * finds FWSEC, the signed program in the VBIOS that fences off protected
 * memory for the GSP's firmware, through the VBIOS's falcon program table.
 * Once the card's boot firmware has finished setting up video memory, it
 * works out where in video memory FWSEC's FRTS region goes, the part of
 * protected memory FWSEC fences off first. It then chooses which of FWSEC's
 * signatures its signature slot must hold, the one for the version the
 * chip's fuses record for FWSEC. Bring-up ends there, so when every step
 * succeeds, `attach()` declines as well.
 * `attach()` fails with a negative error when BAR0 cannot be mapped, the card
 * does not respond, its VBIOS's images are not laid out as
 * `nvidia::read_vbios_layout` requires, a firmware file of an installed
 * directory cannot be opened, the GSP image fails `nvidia::check_gsp_image`,
 * which requires a 64-bit little-endian ELF file whose one .fwversion section
 * holds exactly `FIRMWARE_VERSION` and its NUL, the bootloader or booter load
 * is shorter than its header, cannot be read or holds a header that does not
 * describe it, or a firmware name built from the chip table is too long or
 * malformed. Once the firmware files check out, it also fails when the VBIOS
 * does not lead to a falcon program table as
 * `nvidia::find_falcon_program_table` requires, or the table does not lead
 * to FWSEC as `nvidia::find_fwsec` requires. Once FWSEC is found, it fails
 * when the card's boot firmware has not finished within
 * `nvidia::BOOT_FIRMWARE_TIMEOUT_NS`, or when `nvidia::place_frts_region`
 * refuses the card's state, as when protected memory is already set up. Once
 * the region is placed, it fails when `nvidia::select_fwsec_signature`
 * refuses FWSEC's signatures, as when none is for the version the chip's
 * fuses record.
 * Whether `attach()` declines or fails, the screen is left as it was, and the
 * card keeps only the memory and I/O decoding that `attach()` turned on. That
 * decoding stays on because the boot framebuffer may be in this card's memory,
 * which the CPU reaches only while memory decoding is on.
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
