// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_objects.h"
#include "drivers/graphics/nvidia/display_outputs.h"
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
 * the core channel with its push buffer, the queue of methods written into it
 * and its completion notifier. The instance memory is the video memory where
 * the display engine looks up the memory its channels may read. The core
 * channel is the display channel that sets up heads, links and which head owns
 * each window. The core channel's push buffer is the memory it takes its
 * commands from, and its completion notifier is the page where the display
 * engine reports that an update has finished. An update is the method that
 * applies the methods written before it. The engine reaches the firmware only
 * through the `firmware` and video memory only through the `video_memory` it
 * is built with, and the display engine's registers only through the
 * `registers` passed to `set_up`, whose core channel area it keeps until
 * `release`. The instance memory is never released once cleared, and nothing
 * frees any push buffer once allocated, since the display engine's registers
 * may name either from the moment its address is sent, and a channel released
 * with the hardware kept as it is stays allocated in the hardware. The
 * notifier's page is never freed either, as `completion_notifier` describes.
 * The engine also owns the channel of the window chosen to light first, that
 * channel's push buffer, the chosen output's base buffer (the video memory the
 * window is to scan out) and the look-up table surface (the video memory
 * holding the window's input table, which converts its colors into the
 * half-precision floats the display engine computes with). `set_up` binds each
 * surface it keeps to the window's channel, and nothing frees a bound surface,
 * since the display engine may read it through the binding from then on, and
 * the window's channel, while released with the hardware kept as it is, stays
 * allocated in the hardware.
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
     * `read_display_outputs`, logging the outputs with their facts and the
     * most lanes and the highest rate `connector_link_limits` gives for a link
     * on each one's connector, the rate capped by the chip's highest, then,
     * when there are outputs, take the DisplayPort hardware from the firmware
     * with `take_displayport_from_firmware` and log it, then for each
     * connected output's monitor its EDID (the monitor's description of
     * itself, with the timings it accepts) with `read_output_edid` into one
     * `edid::monitor` on the unprivileged heap, logging the maker, name and
     * timings of an EDID that parses, then the monitor's DPCD capabilities
     * (the link limits a DisplayPort monitor's registers report) and, when the
     * GPU trains PHY repeaters, those of any PHY repeater between the card and
     * the monitor with `read_output_dpcd`, and if it reads the monitor's, log
     * their bytes, the most lanes and the highest rate `monitor_link_limits`
     * gives for a link to the monitor, and whether the monitor takes enhanced
     * framing, and when the EDID parsed too, log the output's mode, the timing
     * `find_mode_candidates` ranks first for the output's link facts from
     * `output_link_facts`, with the lowest link configuration carrying it, or
     * why the output has none: its monitor's DPCD giving a revision below 1.0,
     * or no timing kept. When the GPU's links lack FEC support, log once,
     * before reading any monitor, that no output has a mode, and log no
     * output's mode. Then allocate the core channel's push buffer in DMA pages
     * and the core channel under the display with `allocate_core_channel`, and
     * log its handle, where its push buffer lies and one read of its PUT and
     * GET. Then allocate the core channel's completion notifier with
     * `completion_notifier::allocate`, where NVIDIA's modesetting module
     * allocates it before the push buffer, bind its context DMA to the core
     * channel under a new handle with `instance_memory::bind`, where that
     * module binds it after writing the channel's initial methods and frees
     * the core channel at once when the binding fails, and log the physical
     * address of the notifier and the context DMA's handle. Then write the
     * core channel's initial methods, which `core_initial_method` gives, for
     * the heads read, kicking and waiting for GET to reach PUT whenever the
     * push buffer has no room, kick the rest with no update, so nothing on
     * screen changes, wait for GET to reach PUT, and log one read of PUT and
     * GET with the count of methods written. Then read the display id each
     * head shows and the link driving that display id with
     * `read_active_heads`, which takes `has_crossbar` and the chip's serial
     * links, and log each active head with its display id, its link and the
     * output `find_shown_output` gives for it, or that the display id names no
     * output, then the boot head `find_boot_head` gives and the boot output
     * (the output it shows), or that there is none. Then choose the output to
     * light first and its mode with `choose_first_light`, asking the bandwidth
     * check about each mode in turn, trying first the boot output, else output
     * 0, on the boot head, else head 0, with that head's lowest window, as
     * `find_light_target` gives them, and log the choice or why none is made:
     * no connected output having a mode, every mode being refused, or the
     * request not being allocated. Then, when an output is chosen, allocate
     * the channel of its window: check that the chip's heads report output
     * scalers and that the window is among the present windows read, allocate
     * a push buffer in DMA pages for the channel, then the channel with
     * `allocate_window_channel`, and log the channel's handle, where its push
     * buffer lies and one read of its PUT and GET. The channel takes no
     * initial method. NVIDIA writes a window channel's scaler coefficients
     * only for a window reporting an input scaler while no usable head reports
     * an output scaler, and the chip table says whether the chip's heads
     * report one. The driver also leaves out the color tables NVIDIA writes
     * for a window reporting them, since they are curves for HDR monitors.
     * When the chip's heads report no output scaler, the window is not among
     * those read, its push buffer's pages cannot be allocated, or a request
     * for its channel is refused or cannot be allocated, log why the window
     * has no channel and return `OK`, which leaves the driver attached as when
     * no output is chosen. Then, once the window has its channel, allocate the
     * chosen output's base buffer, the mode's rows `base_buffer_pitch` bytes
     * apart in `vram_use::scanout` video memory 4 KiB aligned (as NVIDIA's
     * modesetting module aligns the surfaces the display engine scans out),
     * then map it with `video_memory::map_for_cpu`, clear it to black, unmap
     * it with `video_memory::unmap_for_cpu`, which completes the writes, and
     * log where it lies with its rows and pitch. Then bind its context DMA to
     * the window's channel under a new handle with `instance_memory::bind`,
     * naming its video memory, and log the handle, where NVIDIA's modesetting
     * module binds each surface's context DMA to the core channel and every
     * window's channel. Then allocate the look-up table surface,
     * `LOOK_UP_TABLE_SURFACE_BYTES` of `vram_use::general` video memory 4 KiB
     * aligned, as NVIDIA's modesetting module allocates it, write each of its
     * words as `look_up_table_word` gives it through a mapping for the CPU,
     * unmap it, log where it lies, then bind its context DMA to the window's
     * channel as the base buffer's, naming its whole pages, where that
     * module's context DMA ends at the table's last byte, and log the handle.
     * When a surface's memory cannot be allocated, mapping it finds no room or
     * its binding finds no room in the instance memory or in the mapping it
     * writes through, log why the output has no base buffer or the display
     * engine no look-up table surface and return `OK`, releasing the memory
     * after a failed mapping or binding, since nothing names it then. Then,
     * once both surfaces are bound, ask the display for the core channel's
     * state with `wait_core_channel_idle` until the firmware reports the
     * channel idle or more than `CORE_CHANNEL_IDLE_WAIT_NS` has passed since
     * the first reply, and log which of the two ended the wait. Then read each
     * present window's owner from the core channel's armed state with
     * `read_window_owners`, and log each one's owner. Then bring the display
     * engine up for the chosen output in a mode set (the programming of heads
     * to drive monitors): send display common the start of the mode set naming
     * the output's display id, and when a head is active, send the shutdown
     * update (the methods `build_shutdown_methods` gives with the windows'
     * heads, the completion notifier cleared and an update naming it with no
     * channel interlocked), wait for the notifier with
     * `completion_notifier::wait_finished` for `COMPLETION_NOTIFIER_WAIT_NS`,
     * stop later updates from writing the notifier, and log that the core
     * channel read the update, with each active head's display id and link
     * control as the armed state reads them. When a method's write, the
     * notifier's wait or the stop of its writes fails, take the engine down:
     * the same methods again, with the windows' heads only when no update was
     * kicked, an update naming no notifier and a wait for GET to reach PUT,
     * logging whether the take-down succeeds. Once the core channel has read
     * the shutdown update, or with no head active, train the output's
     * DisplayPort link with `set_up_output_link`: assign the display id an
     * output resource when display common reports a crossbar, read the
     * resource's DisplayPort facts, train the link at the lowest configuration
     * carrying the mode under them and check the training against the link's
     * status bytes, logging each step. When a step fails, the steps after it
     * are not taken: log that the link is not set up and take the engine down
     * the same way. Then send the mode set's end. A refused start is logged
     * and nothing is written to the core channel, the set-up returning `OK`,
     * and a refused end is logged, changing nothing. Each output's modes are
     * kept on the unprivileged heap from the monitors' reads to the choice, or
     * to `release` when the set-up fails before it, and the other facts read
     * are not kept. Each request spins on the calling CPU, waiting for its
     * reply as `firmware` describes, and so does each wait for GET, for at
     * most 5 seconds, and the wait for the completion notifier, for at most
     * `COMPLETION_NOTIFIER_WAIT_NS`. The requests for the core channel's state
     * follow one another with no pause while the firmware reports the channel
     * busy, as `wait_core_channel_idle` describes. `read_output_edid` also
     * spins before each reread of an EDID as it describes, and `train_link`
     * for each wait an accepted training reply asks for. `set_up` must be
     * called at most once, as `instance_memory::set_up` must, and since it
     * waits on video memory's lock and on the card as `instance_memory` and
     * `video_memory` describe, it must never run in an interrupt handler or
     * under a spinlock, and on a thread that cannot sleep, such as the boot
     * thread, must run only while no other thread can call into video memory
     * or the firmware.
     * @param regs The card's BAR0 registers, through which the core channel's
     * methods are kicked and its PUT, GET and armed state read, and the window
     * channel's PUT and GET read. BAR0 must stay mapped until `release`
     * returns, since the engine keeps a view of the core channel's area until
     * then.
     * @param card The card's DMA profile, for the push buffer's pages and the
     * notifier's page.
     * @param chip The chip table's entry for the card's chip, whose display
     * entry gives the display's limits and classes, whether its heads report
     * output scalers and what the chip's DisplayPort links can do.
     * @param device The driver's device, the parent of the display's objects.
     * @return `OK`, with the firmware holding the instance memory's address,
     * the display's objects, the core channel and its push buffer's address,
     * the instance memory binding the completion notifier's context DMA to the
     * core channel, the display engine having read the core channel's initial
     * methods, and the firmware having reported only the chip's windows, a
     * display engine that can reach push buffers, semaphores and notifiers in
     * system memory, connected display ids among the supported ones, heads
     * within the chip's, head 0 usable and served, with nonzero masks only for
     * the chip's windows, each naming one of the chip's heads outside a
     * flexible mapping, at most one display id shown by each head, each link
     * read for a head one of the chip's serial links and, without a crossbar,
     * a link driving each display id shown. Otherwise the error of the first
     * step that fails, logged as that step describes, such as when the
     * firmware reports a window the chip lacks, the display engine reaches
     * push buffers, semaphores and notifiers only in video memory, display
     * common's report leaves it no head, the firmware refuses to report the
     * supported or connected display ids or it refuses to name the link
     * driving a display id a head shows. When the outputs' modes, the
     * `edid::monitor` or the core channel's push buffer cannot be allocated,
     * the error is `ERR_NO_MEMORY`, which is logged here, and when the
     * completion notifier's page cannot, `ERR_NO_MEMORY` too, logged as
     * `completion_notifier::allocate` describes. A refused binding of the
     * notifier fails the set-up with what `instance_memory::bind` returns,
     * such as `ERR_BAD_STATE` or `ERR_TIMEOUT` when the card does not take the
     * instance memory's mapping, logged as it describes. When GET does not
     * reach PUT within 5 seconds of a kick, the error is `ERR_TIMEOUT`, and
     * when a read of GET finds it past the push buffer's writable end or not a
     * multiple of 4, `ERR_BAD_STATE`, each logged as `display_channel`
     * describes and here. When a request to the bandwidth check fails other
     * than with `ERR_FIRMWARE` or `ERR_NO_MEMORY`, the set-up fails with its
     * error, logged as `choose_first_light` describes, and when the window
     * channel's allocation fails other than with `ERR_FIRMWARE` or
     * `ERR_NO_MEMORY`, the set-up fails with that error, logged as
     * `allocate_window_channel` describes. When a surface's allocation,
     * mapping or binding fails other than with `ERR_NO_MEMORY`, the set-up
     * fails with that error, logged here and as `video_memory` or
     * `instance_memory::bind` describes, the memory released after a failed
     * mapping or binding. When a request for the core channel's state fails
     * other than with `ERR_FIRMWARE`, the set-up fails with its error, logged
     * as `wait_core_channel_idle` describes. When the mode set's start or end
     * fails other than with `ERR_FIRMWARE`, the set-up fails with its error,
     * logged as `firmware::control` describes. When a request for the chosen
     * output's link fails other than with `ERR_FIRMWARE`, the set-up fails
     * with its error, logged as `firmware::control` describes, after the
     * take-down and the mode set's end, whose own failure then changes
     * nothing. A failed shutdown update or take-down, or a link left not set
     * up otherwise, is logged and does not fail the set-up, which then returns
     * `OK` unless the mode set's end fails other than with `ERR_FIRMWARE`. The
     * display's objects and the channels stay allocated once allocated, even
     * after a later step fails, until `release`, the completion notifier stays
     * bound to the core channel once bound, and each surface to the window's
     * channel.
     */
    int32_t set_up(
        registers& regs,
        const dma::device_profile& card,
        const chip_info& chip,
        rm::handle device
    );

    /**
     * @brief Drop the core channel's queue of methods, so nothing writes the
     * released channel's area, free the outputs' modes if `set_up` kept them,
     * then release the chosen window's channel and then the core channel, each
     * once `set_up` allocated it or the firmware may hold it after a failed
     * allocation, as `release_display_channel` describes, then the display's
     * objects, once `set_up` allocated them, as `release_display_objects`
     * describes. The channels are released first, the window's before the
     * core's, as NVIDIA's modesetting module releases them. While no update
     * has been kicked, each is released with the hardware kept as it is, so
     * the heads the boot firmware lit keep scanning out the boot framebuffer,
     * and once one has, without keeping it, since the update changed the boot
     * firmware's state. Its requests spin on the calling CPU as `set_up`'s do.
     * @return `OK` when every release succeeds or nothing is allocated, else
     * the window channel's error ahead of the core channel's and the core
     * channel's ahead of the objects', each logged as the release describes.
     * The channels and the objects are released whatever an earlier release
     * returns, and a later call releases nothing and returns `OK`.
     */
    int32_t release();

private:
    friend struct display_engine_test_access;

    firmware&       m_firmware;
    video_memory&   m_memory;
    instance_memory m_instance_memory;
    display_objects m_objects = {};           // all 0 before `set_up` allocates them and after `release`
    rm::handle      m_core_channel = 0;       // 0 before `set_up` allocates it and after `release`
    dma::buffer     m_core_push_buffer = {};
    rm::handle      m_window_channel = 0;     // the chosen window's, 0 before `set_up` allocates it and after `release`
    dma::buffer     m_window_push_buffer = {};
    vram_range      m_base_buffer = {};       // the chosen output's, of 0 bytes while none is allocated
    vram_range      m_look_up_table = {};     // the look-up table surface, of 0 bytes while none is allocated
    output_modes*   m_output_modes = nullptr; // one per output from the monitors' reads until freed, else null

    // The core channel's completion notifier, its page allocated and its context DMA bound to the channel by `set_up`,
    // and the wait `shut_down_heads` gives it, 0 in tests that skip it
    completion_notifier m_core_notifier;
    rm::handle          m_notifier_context_dma = 0; // 0 until `set_up` binds the notifier's context DMA
    uint64_t            m_notifier_wait_ns = COMPLETION_NOTIFIER_WAIT_NS;

    bool m_has_sent_update = false; // set once an update is kicked on the core channel

    // The core channel's queue of methods, built in `m_core_queue_bytes` once the channel is allocated, null until
    // then and after `release`
    display_channel*                 m_core_queue = nullptr;
    alignas(display_channel) uint8_t m_core_queue_bytes[sizeof(display_channel)];

    // Brings the engine up for the chosen output as `set_up` describes, from the mode set's start to its end
    int32_t bring_up(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active,
        bool has_crossbar,
        const display_output& output,
        const dpcd_capabilities& dpcd,
        const first_light& light
    );

    // Writes the shutdown update naming the notifier, waits for it and stops later updates from writing the notifier
    int32_t shut_down_heads(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active
    );

    // Writes the shutdown methods again, with the windows' heads only when no update was kicked, then an update
    // naming no notifier, and waits for GET to reach PUT
    int32_t take_down(const display_heads& heads, uint32_t present_windows, const active_heads& active);
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
