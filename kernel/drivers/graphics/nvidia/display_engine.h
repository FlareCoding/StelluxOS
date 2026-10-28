// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/display_objects.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"

namespace drivers::nvidia {

/**
 * The card's display engine as the driver sets it up, the part of the GPU that
 * reads images from memory and sends them to monitors. The driver object holds
 * one for its life, sets it up in `attach()` once the driver's device is
 * allocated, when the chip's fuses leave its display enabled, and releases it
 * in `detach()` before the GSP's firmware unloads. The engine's calls must
 * come from one thread at a time, since it takes no lock of its own. It owns
 * the display's instance memory, the objects the firmware keeps for the
 * display (display common, the bandwidth check's object and the display) and
 * the core channel with its push buffer. The instance memory is the video
 * memory where the display engine looks up the memory its channels may read.
 * The core channel is the display channel that sets up heads, links and which
 * head owns each window. The core channel's push buffer is the memory it takes
 * its commands from. The engine reaches the firmware only through the
 * `firmware` and video memory only through the `video_memory` it is built
 * with, and the display engine's registers only through the `registers` passed
 * to `set_up`. The instance memory is never released once cleared, and nothing
 * frees the push buffer once allocated, since the display engine's registers
 * may name either from the moment its address is sent, and a channel released
 * with the hardware kept as it is stays allocated in the hardware.
 */
class display_engine {
public:
    /**
     * @brief An engine that has set up nothing.
     * @param fw The firmware the driver's requests go to, which must outlive
     * the engine.
     * @param memory The driver's video memory, which must outlive the engine.
     */
    display_engine(firmware& fw, video_memory& memory);

    display_engine(const display_engine&) = delete;
    display_engine& operator=(const display_engine&) = delete;

    /**
     * @brief Read the windows this GPU has with `read_present_windows` and log
     * them with the chip's window count, set up the instance memory with
     * `instance_memory::set_up` and log where it lies in video memory,
     * allocate the display's objects under `device` with
     * `allocate_display_objects` and log their handles, then read their
     * capabilities with `read_display_capabilities` and log whether display
     * common reports a crossbar, then the heads and each window's head with
     * `read_display_heads`, logging the heads the display engine is left, with
     * the windows serving each, then the supported and connected display ids,
     * each supported one's kind and whether each digital one is dynamic with
     * `read_display_ids`, logging the supported, connected, digital and static
     * digital ones, then each static digital one's digital facts with
     * `read_display_outputs`, logging the outputs with their facts, then for
     * each connected output's monitor its EDID (the monitor's description of
     * itself, with the timings it accepts) with `read_output_edid` into one
     * `edid::monitor` on the unprivileged heap, logging the maker, name and
     * timings of an EDID that parses, then the monitor's DPCD capabilities
     * (the link limits a DisplayPort monitor's registers report) and those of
     * any PHY repeater between the card and the monitor with
     * `read_output_dpcd`, logging their bytes when it reads them all. Then
     * allocate the core channel's push buffer in DMA pages and the core
     * channel under the display with `allocate_core_channel`, and log its
     * handle, where its push buffer lies and one read of its PUT and GET. The
     * facts read are not kept, and nothing here writes to the display engine.
     * Each request spins on the calling CPU, waiting for its reply as
     * `firmware` describes, and `read_output_edid` spins before each reread of
     * an EDID as it describes. It must be called at most once, as
     * `instance_memory::set_up` must, and since it waits on video memory's
     * lock and on the card as `instance_memory` describes, it must never run
     * in an interrupt handler or under a spinlock, and on a thread that cannot
     * sleep, such as the boot thread, must run only while no other thread can
     * call into video memory or the firmware.
     * @param regs The card's BAR0 registers, through which the core channel's
     * PUT and GET are read.
     * @param card The card's DMA profile, for the push buffer's pages.
     * @param chip The chip table's entry for the card's chip, whose display
     * entry gives the display's limits and classes.
     * @param device The driver's device, the parent of the display's objects.
     * @return `OK`, with the firmware holding the instance memory's address,
     * the display's objects, the core channel and its push buffer's address,
     * and having reported only the chip's windows, a display engine that can
     * reach push buffers, semaphores and notifiers in system memory, connected
     * display ids among the supported ones, and heads within the chip's, head
     * 0 usable and served, with nonzero masks only for the chip's windows,
     * each naming one of the chip's heads outside a flexible mapping.
     * Otherwise the error of the first step that fails, logged as that step
     * describes, such as when the firmware reports a window the chip lacks,
     * the display engine reaches push buffers, semaphores and notifiers only
     * in video memory, display common's report leaves it no head or the
     * firmware refuses to report the supported or connected display ids. When
     * the `edid::monitor` or the push buffer's pages cannot be allocated, the
     * error is `ERR_NO_MEMORY`, logged here. The display's objects and the
     * core channel stay allocated once allocated, even after a later step
     * fails, until `release`.
     */
    int32_t set_up(
        const registers& regs,
        const dma::device_profile& card,
        const chip_info& chip,
        rm::handle device
    );

    /**
     * @brief Release the core channel, once `set_up` allocated it or the
     * firmware may hold it after a failed allocation, as
     * `release_core_channel` describes, then the display's objects, once
     * `set_up` allocated them, as `release_display_objects` describes. The
     * core channel is released first, with the hardware kept as it is, so the
     * heads the boot firmware lit keep scanning out the boot framebuffer. Its
     * requests spin on the calling CPU as `set_up`'s do.
     * @return `OK` when every release succeeds or nothing is allocated, else
     * the core channel's error ahead of the objects', each logged as the
     * release describes. The objects are released whatever the core channel's
     * release returns, and a later call releases nothing and returns `OK`.
     */
    int32_t release();

private:
    firmware&       m_firmware;
    instance_memory m_instance_memory;
    display_objects m_objects = {};     // all 0 before `set_up` allocates them and after `release`
    rm::handle      m_core_channel = 0; // 0 before `set_up` allocates it and after `release`
    dma::buffer     m_core_push_buffer = {};
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
