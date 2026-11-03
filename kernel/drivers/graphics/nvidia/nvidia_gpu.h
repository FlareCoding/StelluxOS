// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H

#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_engine.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/gpu_objects.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/interrupts.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/pci_driver.h"

namespace drivers {

/**
 * The driver for an NVIDIA GPU. It boots the firmware of the card's GSP (the
 * processor inside the GPU that runs NVIDIA's firmware) through SEC2 (the
 * GPU's security processor). When the chip's display is enabled, it then sets
 * up the display engine for gfxfb, which gives userland the screen.
 *
 * The PCI framework creates the object for each NVIDIA display controller and
 * calls `attach()` on the boot thread before userland starts. After `OK` it
 * starts `run()` as the driver task, and when it cannot, it calls `detach()`
 * and keeps the object. After any other result it calls `detach()` and frees
 * the object.
 *
 * Protected memory (the video memory reserved for the GSP's firmware) is set
 * up partway through `attach()` and stays set up, even when `attach()` then
 * fails, until `detach()` takes it down or the card is reset. An `attach()`
 * that finds protected memory set up fails with `nvidia::ERR_BAD_STATE`.
 */
class nvidia_gpu_driver : public pci_driver {
public:
    nvidia_gpu_driver(pci::device* dev)
        : pci_driver("nvidia_gpu", dev)
        , m_display_engine(m_gsp, m_video_memory) {}

    /**
     * @brief Boot the GSP's firmware and, unless the chip's fuses disable its
     * display, set up the display engine and register it with gfxfb as
     * `nvidia::register_display_engine` describes. It declines the card when:
     * - a driver display is already registered with gfxfb
     * - the chip table does not list the card's chip
     * - a directory the chip's firmware files belong in is not installed, as
     *   in an image without the nvidia-firmware package
     * @return `OK`, `DECLINED`, or the negative error of the step that failed.
     */
    int32_t attach() override;

    /**
     * @brief Undo `attach()` as far as it got, ending with what
     * `pci_driver::detach()` does. Its waits spin on the calling CPU. Decoding
     * and bus mastering that `attach()` turned on stay on, gfxfb keeps a
     * display engine that `attach()` registered, and memory the card may still
     * read stays allocated for good. A second call writes no register.
     * @return `nvidia::ERR_TIMEOUT` or `nvidia::ERR_BAD_STATE` when stopping
     * SEC2 or the GSP fails, leaving protected memory as it was, else the
     * error of unloading the firmware, else that of taking protected memory
     * down, else what `pci_driver::detach()` returns. Each failure is logged.
     */
    int32_t detach() override;

    /**
     * @brief The driver task, which the framework starts once `attach()`
     * returns `OK`. Each time an interrupt wakes it and the card still
     * responds, it reads the messages the GSP's firmware queued, checks the
     * GSP's health, then logs the light's first second of vertical blanks at
     * the first wake after the handler has serviced it. So a failure the GSP
     * reports without a message is looked for only at the next wake. It never
     * returns.
     */
    void run() override;

    /**
     * @brief The handler of the card's one MSI vector, which the GSP's
     * interrupt and, with flips on, the display's raise. It records the GSP's
     * sources it cleared for the driver task and services the heads' vertical
     * blanks as `nvidia::display_engine::service_vertical_blanks` describes.
     * It can run before `attach()` returns, as soon as the GSP's interrupt is
     * enabled, and once more after `detach()` disables it.
     * @return true when the GSP's acknowledge returned a source, all ones from
     * a stopped card among them, or a service completed the first second.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool on_interrupt(uint32_t vector) override;

private:
    int32_t bring_up_bar1(
        nvidia::registers& regs,
        const nvidia::static_configuration& configuration,
        uint64_t bar1_address,
        uint64_t bar1_bytes,
        uint64_t console_bytes
    );
    int32_t tear_down_gsp();
    void release_firmware_objects();
    int32_t unload_firmware(nvidia::registers& regs);
    void run_fwsec_sb(nvidia::registers& regs);
    int32_t take_down_protected_memory(nvidia::registers& regs);

    nvidia::gsp_boot_memory       m_boot_memory;
    nvidia::gsp                   m_gsp;
    nvidia::gpu_objects           m_gpu_objects = {}; // both 0 before `attach()` allocates them and after `detach()`
    nvidia::bar1_video_memory     m_video_memory;
    nvidia::display_engine        m_display_engine;
    nvidia::fwsec_layout          m_fwsec = {};
    nvidia::fwsec_signature       m_fwsec_signature = {};
    dma::buffer                   m_booter_unload_copy = {};
    nvidia::booter_layout         m_booter_unload_parts = {};
    const nvidia::chip_info*      m_chip = nullptr;
    uint32_t                      m_boot0 = 0;
    uint32_t                      m_bar1_block = 0;
    nvidia::interrupt_vectors     m_interrupt_vectors = {};
    uint32_t                      m_gsp_sources = 0;       // sources the handler cleared since the task's last wake
    uint64_t                      m_last_interrupt_ns = 0; // when the handler last cleared one
    bool                          m_must_stop_gsp = false;
    bool                          m_must_stop_sec2 = false;
    bool                          m_must_unload = false;
    bool                          m_must_run_fwsec_sb = false;
    bool                          m_is_gsp_interrupt_enabled = false;

    friend struct nvidia_gpu_test_access;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
