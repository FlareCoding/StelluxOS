// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/core_methods.h"
#include "drivers/graphics/nvidia/cursor_channel.h"
#include "drivers/graphics/nvidia/display_active_heads.h"
#include "drivers/graphics/nvidia/display_channel.h"
#include "drivers/graphics/nvidia/display_dpcd.h"
#include "drivers/graphics/nvidia/display_heads.h"
#include "drivers/graphics/nvidia/display_light.h"
#include "drivers/graphics/nvidia/display_objects.h"
#include "drivers/graphics/nvidia/display_outputs.h"
#include "drivers/graphics/nvidia/display_window_owners.h"
#include "drivers/graphics/nvidia/firmware.h"
#include "drivers/graphics/nvidia/instance_memory.h"
#include "drivers/graphics/nvidia/latch_rules.h"
#include "drivers/graphics/nvidia/operation_queue.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "drivers/graphics/nvidia/window_notifier.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "timer/timer.h"

namespace drivers::nvidia {

// A head table entry for a head the light has given no output
constexpr uint8_t NO_OUTPUT = 0xFF;

// The lit head's first second of vertical blanks as the interrupt handler serviced them, which the driver task logs
struct first_second_statistics {
    uint64_t start_ns;     // when the handler serviced the light's first vertical blank
    uint64_t end_ns;       // when it serviced the one completing the mode's refresh rate in frames
    uint16_t start_frame;  // the head's own frame count at the first service
    uint16_t end_frame;    // the head's own frame count at the last service
    uint16_t lowest_line;  // the lowest raster line a service read
    uint16_t highest_line; // the highest raster line a service read
};

class display_engine;
struct scanout_buffer;

// A buffer's binding and later its release on the driver task, kept in the buffer so its release allocates nothing
struct buffer_operation : operation {
    display_engine* engine;
    scanout_buffer* buffer;
};

/**
 * A buffer `create_buffer()` made, in `vram_use::scanout` video memory and
 * mapped for userland at the buffer's physical address. Only the driver task
 * binds or unbinds it, before `create_buffer()` returns and once gfxfb
 * releases it, so `present()` reads `bound_windows` without a lock.
 */
struct scanout_buffer : gfxfb::buffer {
    vram_range       range;
    rm::handle       context_dma;    // the context DMA naming `range`, 0 when no output was lit
    uint32_t         bound_windows;  // a bit for each window whose channel `context_dma` is bound to
    buffer_operation task_operation; // the binding, then the release
};

/**
 * The card's display engine as the driver sets it up, the part of the GPU that
 * reads images from memory and sends them to monitors. The driver object holds
 * one for its life, sets it up in `attach()` once the driver's device is
 * allocated, when the chip's fuses leave its display enabled, and releases it
 * in `detach()` before the GSP's firmware unloads. `set_up` and `release` must
 * come from one thread at a time, since neither holds a lock across its steps.
 * It owns the display's instance memory, the objects the firmware keeps for
 * the display (display common, the bandwidth check's object and the display)
 * and the core channel with its push buffer, the queue of methods written into
 * it and its completion notifier. The instance memory is the video memory
 * where the display engine looks up the memory its channels may read. The core
 * channel is the display channel that sets up heads, links and which head owns
 * each window. The core channel's push buffer is the memory it takes its
 * commands from, and its completion notifier is the page where the display
 * engine reports that an update has finished. An update is the method that
 * applies the methods written before it. The engine reaches the firmware only
 * through the `firmware` and video memory only through the `video_memory` it
 * is built with, and the display engine's registers only through the
 * `registers` passed to `set_up`, whose areas of its channels it keeps until
 * `release`. The instance memory is never released once cleared, and nothing
 * frees any push buffer once allocated, since the display engine's registers
 * may name either from the moment its address is sent, and a channel released
 * with the hardware kept as it is stays allocated in the hardware. The two
 * notifiers' pages are never freed either, as each notifier's class describes.
 * The engine also owns the channel of the window chosen to light first, that
 * channel's push buffer, queue of methods and notifier, the chosen output's
 * base buffer (the video memory the window is to scan out) and the look-up
 * table surface (the video memory holding the window's input table, which
 * converts its colors into the half-precision floats the display engine
 * computes with, and the head's output table, which converts them back for
 * the link). `set_up` binds each surface it keeps to the window's channel,
 * and the look-up table surface to the core channel too, keeping the context
 * DMA it binds for the methods naming it, and nothing frees a bound surface,
 * since the display engine may read it through the binding from then on, and
 * the window's channel, while released with the hardware kept as it is, stays
 * allocated in the hardware. It also owns the lit head's cursor channel and
 * the chosen output's cursor memory (the video memory holding the cursor's
 * picture), which stays mapped for the CPU and bound to the core channel, and
 * which nothing frees either.
 *
 * The engine is also a `gfxfb::display`, which the driver registers with gfxfb
 * as `nvidia_gpu_driver` describes. Its outputs are the first
 * `gfxfb::GFXFB_OUTPUTS_MAX` of the outputs `set_up` reads, each a static
 * DisplayPort display id, and gfxfb reads each one an active head showed with
 * `boot_output` set. The chosen output reads as lit once the light lights it,
 * at the mode chosen for it, and its base buffer as gfxfb reads it is the
 * mapping `set_up` keeps for userland, at that mapping's physical address in
 * BAR1 with `cacheable` false, so userland maps it write-combining. Every
 * other output reads as dark. No output relights or reports a cursor, and
 * `flush()` returns at once on the lit output, since its head scans the base
 * buffer itself. `create_buffer()` makes `scanout_buffer`s, which the driver
 * task binds to the lit window's channel, if any, and `present()` takes those
 * and the base buffer. `set_cursor()` and `move_cursor()` are unsupported, and
 * `release_buffer()` keeps the base buffer, a bound surface, for good.
 * `read_output()`, `base_buffer()` and `flush()` read the chosen output's
 * state under its spinlock, which no holder keeps across a wait, so they never
 * sleep or wait on the card. Once the display fails, as `present()` says, or
 * `release` runs, `flush()` and a supported `present()` or `create_buffer()`
 * fail with `gfxfb::ERR_DEVICE`. After `release`, `read_output()` reads every
 * output as dark too, but gfxfb keeps its record of the base buffer, since
 * `detach()` runs on the boot thread and only a task may report an output gone
 * dark, while BAR1 stops translating that buffer's address once `detach()`
 * restores BAR1's block register.
 */
class display_engine : public gfxfb::display {
public:
    /**
     * @brief An engine that has set up nothing.
     * @param fw The firmware the driver's requests go to, which must outlive
     * the engine.
     * @param memory The driver's video memory, which must outlive the engine.
     * @param operations The driver task's queue, which must outlive the engine.
     */
    display_engine(firmware& fw, video_memory& memory, operation_queue& operations);

    display_engine(const display_engine&) = delete;
    display_engine& operator=(const display_engine&) = delete;

    /**
     * @brief Read the heads and windows this GPU has with
     * `read_present_heads_and_windows` and log them, the windows with the
     * chip's window count, set up the instance memory with
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
     * window's channel. Then map the bound base buffer for the CPU again,
     * keeping that mapping while the driver is attached, for userland to draw
     * through as gfxfb's base buffer, and log its physical address in BAR1.
     * Then allocate the look-up table surface, `LOOK_UP_TABLE_SURFACE_BYTES`
     * of `vram_use::general` video memory 4 KiB aligned, as NVIDIA's
     * modesetting module allocates it, write each of its words as
     * `look_up_table_word` gives it through a mapping for the CPU, unmap it,
     * log where it lies, then bind its context DMA under a new handle to the
     * core channel, whose heads read its output table, and then to the
     * window's channel, as that module binds it, naming its whole pages, where
     * that module's context DMA ends at the table's last byte, and log the
     * handle. When a surface's memory cannot be allocated, mapping it to write
     * it finds no room or its binding finds no room in the instance memory or
     * in the mapping it writes through, log why the output has no base buffer
     * or the display engine no look-up table surface and return `OK`,
     * releasing the memory after a failed mapping or binding, since nothing
     * names it then. A failed binding of the table to the window's channel
     * first undoes the table's binding to the core channel. When undoing it
     * fails, the memory stays allocated and still bound to the core channel,
     * logged as such, and the engine keeps neither the table nor its context
     * DMA. The set-up then returns `OK` only when both found no room. When the
     * base buffer's mapping for userland finds no room, or maps it at no
     * physical address, after which it is unmapped, log that the output cannot
     * light and return `OK`, the memory staying bound. Then give the lit head
     * a cursor and log it: its channel, as `cursor_channel::allocate`
     * allocates it, and 256 KiB of video memory for its picture, mapped for
     * the CPU and bound to the core channel. When the head is not present, its
     * channel is refused or cannot be allocated, or the memory, its mapping or
     * its binding finds no room, log why and go on without a cursor, releasing
     * the memory after a failed mapping or binding. Then ask the display for
     * the core channel's state with `wait_core_channel_idle` until the
     * firmware reports the channel idle or more than
     * `CORE_CHANNEL_IDLE_WAIT_NS` has passed since the first reply, and log
     * which of the two ended the wait. Then read each present window's
     * owner from the core channel's armed state with `read_window_owners`, and
     * log each one's owner. Then bring the display engine up for the chosen
     * output in a mode set (the programming of heads to drive monitors): send
     * display common the start of the mode set naming the output's display id,
     * and when a head is active, send the shutdown update (the methods
     * `build_shutdown_methods` gives with the windows' heads, the completion
     * notifier cleared and an update naming it with no channel interlocked),
     * wait for the notifier with
     * `completion_notifier::wait_finished` for `COMPLETION_NOTIFIER_WAIT_NS`,
     * stop later updates from writing the notifier, and log that the core
     * channel read the update, with each active head's display id and link
     * control as the armed state reads them. When a method's write, the
     * notifier's wait or the stop of its writes fails, take the engine down:
     * the same methods again, with the windows' heads only when no update was
     * kicked, an update naming no notifier and a wait for GET to reach PUT,
     * logging whether the take-down succeeds. Once the core channel has read
     * the shutdown update, or with no head active, make the output's
     * DisplayPort link ready with `set_up_output_link`: assign the display id
     * an output resource when display common reports a crossbar, read the
     * resource's DisplayPort facts, train the link at the lowest configuration
     * carrying the mode under them, check the training against the link's
     * status bytes, then program the head's stream and the stream's attributes
     * on the link, logging each step. When a step fails, the steps after it
     * are not taken: log that the link is not set up and take the engine down
     * the same way, except that a refused override of the stream's attributes,
     * the last step, leaves the link set up with the attributes the firmware
     * sets. With the link ready, clear the cursor memory to transparent and
     * park the cursor where none of it shows, when the head has a cursor, then
     * light the head: the methods `build_head_light_methods` gives for the
     * chosen timing, the display id on that link and the head's output table,
     * which the look-up table surface holds, after the windows' heads when no
     * update was kicked, then the head's cursor methods, which show the cursor
     * memory only once the park succeeded, the window's usage bounds and, into
     * the window's channel, its first image (the base buffer shown through the
     * look-up table), the notifier cleared and one update naming it and
     * interlocked with the window channel's update, the core channel kicked
     * before the window's channel. Wait for the notifier, stop its writes and
     * log its word with the lit head's display id and link control as the
     * armed state reads them. When the update is the first kicked and changes
     * a present window's owner from what the armed state reads, a present
     * window serving no head always counting as changed, it interlocks no
     * window and shows no cursor, and the cursor methods, the window's usage
     * bounds and first image follow in a second update naming no notifier and
     * awaited by nothing. Either park write finding no room within
     * `gfxfb::GFXFB_PRESENT_TIMEOUT_NS` lights the head without a cursor,
     * logged. When the park reads all ones, as a card gone reads, or a write,
     * the wait or the stop fails, log it and take the engine down the same
     * way, stopping the lit head too, after the heads found active, and
     * otherwise mark the chosen output lit under its spinlock. Then send the
     * mode set's end. A refused start is logged and nothing is written to the
     * core channel, the set-up returning `OK`, and a refused end is logged,
     * changing nothing. Each output's modes are kept on the unprivileged heap
     * from the monitors' reads to the choice, or to `release` when the set-up
     * fails before it, and of the other facts read only those that gfxfb reads
     * are kept: the count of outputs up to `gfxfb::GFXFB_OUTPUTS_MAX`, which
     * ones an active head shows, and the chosen output with its mode once its
     * base buffer is mapped for userland. Each request spins on the calling
     * CPU, waiting for its reply as `firmware` describes, and so does each
     * wait for GET, for at most 5 seconds, and each wait for the completion
     * notifier, for at most `COMPLETION_NOTIFIER_WAIT_NS`. The requests for
     * the core channel's state follow one another with no pause while the
     * firmware reports the channel busy, as `wait_core_channel_idle`
     * describes. `read_output_edid` also spins before each reread of an EDID
     * as it describes, and `train_link` for each wait an accepted training
     * reply asks for. `set_up` must be called at most once, as
     * `instance_memory::set_up` must, and since it waits on video memory's
     * lock and on the card as `instance_memory` and `video_memory` describe,
     * it must never run in an interrupt handler or under a spinlock, and on a
     * thread that cannot sleep, such as the boot thread, must run only while
     * no other thread can call into video memory or the firmware.
     * @param regs The card's BAR0 registers, through which the core channel's
     * methods are kicked and its PUT, GET and armed state read, and the window
     * channel's methods kicked and its PUT and GET read. BAR0 must stay mapped
     * until `release` returns, since the engine keeps a view of each of its
     * channels' areas until then.
     * @param card The card's DMA profile, for the push buffer's pages and the
     * notifiers' pages.
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
     * `completion_notifier::allocate` describes. When the window notifier's
     * page or room for its binding cannot be had, the light goes on with
     * presents unsupported, logged here. Any other refused binding of either
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
     * mapping or binding unless undoing the table's binding to the core
     * channel fails. When that undoing fails after the window's binding found
     * no room, the set-up fails with the undoing's error unless it is
     * `ERR_NO_MEMORY`, logged here and as `instance_memory::unbind` describes.
     * The base buffer stays bound, though, after its mapping for userland
     * fails. When the cursor channel's allocation fails other than with
     * `ERR_INVALID_ARG`, `ERR_FIRMWARE` or `ERR_NO_MEMORY`, the set-up fails
     * with that error, logged as `firmware` describes, and when the cursor
     * memory's allocation, mapping or binding fails other than with
     * `ERR_NO_MEMORY`, with that error, logged here and as `video_memory` or
     * `instance_memory::bind` describes, the memory released after a failed
     * mapping or binding. When a request for the core channel's state fails
     * other than with `ERR_FIRMWARE`, the set-up fails with its error, logged
     * as `wait_core_channel_idle` describes. When the mode set's start or end
     * fails other than with `ERR_FIRMWARE`, the set-up fails with its error,
     * logged as `firmware::control` describes. When a request for the chosen
     * output's link fails other than with `ERR_FIRMWARE`, the set-up fails
     * with its error, logged as `firmware::control` describes, after the
     * take-down and the mode set's end, whose own failure then changes
     * nothing. A failed shutdown update, light or take-down, or a link left
     * not set up otherwise, is logged and does not fail the set-up, which then
     * returns `OK` unless the mode set's end fails other than with
     * `ERR_FIRMWARE`. The display's objects and the channels stay allocated
     * once allocated, even after a later step fails, until `release`, the
     * completion notifier stays bound to the core channel once bound, the
     * window notifier and each surface to the window's channel, and the
     * look-up table surface to the core channel as well unless the set-up
     * undoes that binding after a failed binding to the window's channel. The
     * cursor memory stays mapped and bound to the core channel once bound.
     */
    int32_t set_up(
        registers& regs,
        const dma::device_profile& card,
        const chip_info& chip,
        rm::handle device
    );

    /**
     * @brief Under the chosen output's spinlock, cancel the latch check and
     * deadline timers, end a queued present with `GFXFB_PRESENT_FAILED`,
     * marking the display failed, and mark the engine released, so
     * `read_output()` reads every output as dark and `flush()`, `present()`
     * and `create_buffer()` fail, then drop the channels' queues of methods,
     * so nothing writes a released channel's area, free the outputs' modes if
     * `set_up` kept them, then release the lit head's cursor channel as
     * `cursor_channel::release` describes, then the chosen window's channel
     * and then the core channel, each once `set_up` allocated it or the
     * firmware may hold it after a failed allocation, as
     * `release_display_channel` describes, then the display's objects, once
     * `set_up` allocated them, as `release_display_objects` describes. While
     * no update has been kicked, the window's and the core's are released with
     * the hardware kept as it is, so the heads the boot firmware lit keep
     * scanning out the boot framebuffer, and once one has, without keeping it,
     * since the update changed the boot firmware's state. Its requests spin on
     * the calling CPU as `set_up`'s do.
     * @return `OK` when every release succeeds or nothing is allocated, else
     * the error of the first release to fail in that order, each logged as the
     * release describes. The channels and the objects are released whatever an
     * earlier release returns, and a later call releases nothing and returns
     * `OK`.
     */
    int32_t release();

    /**
     * @brief Whether the chosen output is lit, as gfxfb reads it, which holds
     * from when the light lights it until `release`. It must be called only
     * before the driver task starts, while the boot thread alone lights
     * outputs, since it takes no lock.
     * @return true while the chosen output is lit.
     */
    bool has_lit_output() const { return m_is_lit; }

    /**
     * @brief Turn flips on, letting the card's interrupt handler service the
     * heads' vertical blanks. `set_up` disables every head's vertical blank
     * interrupt as it starts, and the light enables its head's once the head
     * table names the output. `attach()` must call this once `set_up` has run
     * and before it enables the display's vector.
     */
    void turn_on_flips();

    // Safe to call from any context, the card's interrupt handler among them
    bool are_flips_on() const;

    /**
     * @brief Service the vertical blanks pending on the heads whose bits are
     * set in `pending_heads`, as `pending_vertical_blanks` reads them, from
     * the card's interrupt handler. Each is cleared, and one on the lit
     * output's head counts a frame and runs the latch step, ending the queued
     * present latched or scheduling the latch check, none after `release`,
     * which leaves the lit head's interrupt enabled. Takes the output's
     * spinlock and never sleeps.
     * @return true once, at the service completing the light's first second.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool service_vertical_blanks(registers& regs, uint32_t pending_heads);

    // Logs the light's first second of vertical blanks at most once, after its last service, from the driver task
    void log_first_second();

    uint32_t output_count() const override { return m_output_count; }
    bool needs_flush() const override { return false; }
    int32_t read_output(uint32_t output, gfxfb::output_state& out) override;
    gfxfb::buffer* base_buffer(uint32_t output) override;
    int32_t flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) override;

    /**
     * @brief Allocate a buffer in scanout video memory, cleared to black and
     * mapped for userland, then wait for the driver task to bind it to the lit
     * window's channel, if any, so it must not run in an interrupt handler.
     * @return `ERR_UNSUPPORTED` and `ERR_DEVICE` as `present()` returns them,
     * then `ERR_INVALID_ARG` for a format other than XRGB8888 or a size past
     * the window's 16-bit fields, all unlogged, then `ERR_NO_MEMORY` when
     * memory runs short and `ERR_REFUSED` when the buffer cannot be allocated,
     * mapped or bound, the engine itself logging neither.
     */
    int32_t create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) override;

    /**
     * @brief Write an update showing `buf` into the lit window's channel, for
     * its head to take at a vertical blank, ignoring the regions and waiting
     * only for the output's spinlock. The interrupt handler or a timer ends the
     * present by the latch rules (latch_rules.h), or `release` ends it failed.
     * @return gfxfb's errors in its order: `ERR_UNSUPPORTED` with flips off or
     * no window notifier, `ERR_DEVICE` once released or failed (a refused GET
     * or a missed deadline fails the display), `ERR_DARK` off the lit output,
     * `ERR_INVALID_ARG` off its mode's size, and `ERR_REFUSED` for a buffer
     * not bound to the window's channel or a full push buffer.
     */
    int32_t present(uint32_t output, gfxfb::buffer* buf, const gfxfb::gfxfb_rect* regions, uint32_t count) override;

    int32_t set_cursor(uint32_t, const gfxfb::cursor_image&) override { return gfxfb::ERR_UNSUPPORTED; }
    int32_t move_cursor(uint32_t, int32_t, int32_t) override { return gfxfb::ERR_UNSUPPORTED; }

    /**
     * @brief Queue the driver task's release of a buffer `create_buffer()`
     * made and return without waiting. The task unbinds, unmaps and frees it,
     * or keeps it all for good once the display failed, the engine was
     * released or unbinding it failed, since the head may still scan it. The
     * base buffer is kept for good.
     * @param buf The base buffer or a buffer `create_buffer()` made, which
     * nothing refers to any longer.
     */
    void release_buffer(gfxfb::buffer* buf) override;

private:
    friend struct display_engine_test_access;

    // Whether flips are on with the lit window's notifier bound, which `present()` and `create_buffer()` need first
    bool are_presents_supported() const;

    // The driver task's halves of `create_buffer()` and `release_buffer()`, each given the buffer's `task_operation`
    static void bind_buffer(operation& op);
    static void release_scanout_buffer(operation& op);

    firmware&       m_firmware;
    video_memory&   m_memory;
    instance_memory m_instance_memory;
    display_objects m_objects = {};           // all 0 before `set_up` allocates them and after `release`
    rm::handle      m_core_channel = 0;       // 0 before `set_up` allocates it and after `release`
    dma::buffer     m_core_push_buffer = {};
    rm::handle      m_window_channel = 0;     // the chosen window's, 0 before `set_up` allocates it and after `release`
    dma::buffer     m_window_push_buffer = {};
    vram_range      m_base_buffer = {};       // the chosen output's, of 0 bytes while none is allocated
    vram_range      m_look_up_table = {};     // the look-up table surface, of 0 bytes while the engine keeps none
    output_modes*   m_output_modes = nullptr; // one per output from the monitors' reads until freed, else null

    operation_queue& m_operations;

    // The context DMAs binding the base buffer and the look-up table surface to the window's channel, the table's to
    // the core channel too, each 0 until `set_up` binds it
    rm::handle m_base_buffer_context_dma = 0;
    rm::handle m_look_up_table_context_dma = 0;

    // The core channel's completion notifier, its page allocated and its context DMA bound to the channel by `set_up`,
    // and the wait `send_notified_update` gives it for the shutdown update and the light, 0 in tests that skip it
    completion_notifier m_core_notifier;
    rm::handle          m_notifier_context_dma = 0; // 0 until `set_up` binds the notifier's context DMA
    uint64_t            m_notifier_wait_ns = COMPLETION_NOTIFIER_WAIT_NS;

    bool m_has_sent_update = false; // set once an update is kicked on the core channel

    // The lit head's cursor channel and the chosen output's cursor memory, mapped and bound under a context DMA, 0
    // until bound. The mapping and the wait are set before registration for good
    cursor_channel m_cursor_channel;
    vram_range     m_cursor_memory = {};
    cpu_mapping    m_cursor_mapping = {};
    rm::handle     m_cursor_context_dma = 0;
    uint64_t       m_cursor_wait_ns = gfxfb::GFXFB_PRESENT_TIMEOUT_NS; // the park's wait for room

    // The core channel's queue of methods, built in `m_core_queue_bytes` once the channel is allocated, null until
    // then and after `release`
    display_channel*                 m_core_queue = nullptr;
    alignas(display_channel) uint8_t m_core_queue_bytes[sizeof(display_channel)];

    // The chosen window's channel's queue of methods, built in `m_window_queue_bytes` once that channel is allocated,
    // null until then and after `release`
    display_channel*                 m_window_queue = nullptr;
    alignas(display_channel) uint8_t m_window_queue_bytes[sizeof(display_channel)];

    // What gfxfb reads: `set_up` records all but the two flags before registration, never to change, and the light and
    // `release` write them under `m_output_lock`, which gfxfb's calls take to read them
    uint8_t           m_output_count = 0;             // at most `gfxfb::GFXFB_OUTPUTS_MAX`
    uint32_t          m_boot_outputs = 0;             // a bit for each output an active head showed, bit 0 output 0's
    uint8_t           m_chosen_output = 0;
    gfxfb::gfxfb_mode m_chosen_mode = {};
    gfxfb::buffer     m_userland_base_buffer = {};    // the base buffer as userland maps it, of 0 bytes while unmapped
    bool              m_is_lit = false;               // set by the light, read bare only before the driver task starts
    bool              m_is_released = false;          // set by `release`, failing flushes, presents and buffer creation
    sync::spinlock    m_output_lock = sync::SPINLOCK_INIT;

    // The lit output's vertical blanks, which the light starts and the interrupt handler counts under `m_output_lock`
    uint8_t                 m_lit_head = 0;
    uint32_t                m_frames_per_second = 0;  // the mode's refresh rate rounded, 0 until the light
    uint64_t                m_frame_count = 0;        // the vertical blanks serviced since the light
    uint64_t                m_vertical_blank_ns = 0;  // when the handler serviced the last of them
    uint16_t                m_serviced_frame = 0;     // the head's own frame count at that service
    first_second_statistics m_first_second = {};
    bool                    m_has_logged_first_second = false;

    // For each head, the output the light gave it, kept after `release`, or `NO_OUTPUT`, stored with release under
    // `m_output_lock` and loaded with acquire by the handler. `set_up` records the head count before flips turn on
    uint8_t            m_head_outputs[DISPLAY_HEADS_MAX];
    uint8_t            m_head_count = 0;
    sync::atomic<bool> m_are_flips_on; // stored with release, loaded with acquire

    // The lit window's notifier, its page allocated and its context DMA bound to the window's channel by `set_up`
    window_notifier m_window_notifier;
    rm::handle      m_window_notifier_context_dma = 0; // 0 until `set_up` binds it

    // The lit output's queued present and the latch rules' state, which the light and `present` set and the handler's
    // latch step, the two timers' callbacks and `release` end, all under `m_output_lock`
    uint8_t               m_lit_window = 0;
    mode_times            m_mode_times = {};
    bool                  m_has_queued_present = false;
    uint32_t              m_queued_slot = 0;
    uint64_t              m_queued_deadline_ns = 0;
    uint32_t              m_next_slot = 0;
    uint64_t              m_check_frame = 0;
    uint64_t              m_last_reported_frame = 0;
    uint64_t              m_last_estimate_log_ns = 0; // when the deadline last logged an estimated latch, 0 if never
    sync::atomic<bool>    m_is_display_failed;        // set once for good, stored with release, loaded with acquire
    timer::deadline_timer m_latch_check_timer;
    timer::deadline_timer m_deadline_timer;

    // Where each ended present is reported, gfxfb's `present_done` outside tests
    void (*m_present_done)(gfxfb::display*, uint32_t, const gfxfb::present_result&) = gfxfb::present_done;

    // Counts a vertical blank of the lit output serviced at `now_ns`, under `m_output_lock`, and returns whether it
    // completes the light's first second
    bool count_vertical_blank(uint64_t now_ns, uint32_t position);

    /**
     * @brief Write `buf`'s present for `present`, under `m_output_lock`,
     * setting `write_rc` to the channel's error when a write fails the
     * display, for `present` to log once unlocked.
     * @return As `present` returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t queue_present(uint32_t output, const gfxfb::buffer& buf, int32_t& write_rc);

    // The lit output's flip state as the latch rules read it, under `m_output_lock`
    latch_inputs read_latch_inputs() const;

    /**
     * @brief Apply the latch step to the vertical blank just counted, under
     * `m_output_lock`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void run_latch_step();

    /**
     * @brief End the queued present with `status`, recording a latch's report
     * as the last, under `m_output_lock`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void end_present(uint32_t status, const latch_report& report);

    // The two timers' callbacks, each taking `m_output_lock` elevated
    static void check_latch(timer::deadline_timer* timer);
    static void expire_deadline(timer::deadline_timer* timer);

    // Brings the engine up for the chosen output as `set_up` describes, from the mode set's start to its end
    int32_t bring_up(
        registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const window_owners& owners,
        const active_heads& active,
        bool has_crossbar,
        const display_output& output,
        const dpcd_capabilities& dpcd,
        const first_light& light
    );

    // Writes an update naming the cleared notifier, interlocked with the window channels of `window_interlocks`, kicks
    // the window's channel next if named, waits, reads the notifier's status into `status_word` and stops its writes
    int32_t send_notified_update(uint32_t window_interlocks, uint32_t& status_word);

    // Writes the shutdown update naming the notifier, waits for it and stops later updates from writing the notifier
    int32_t shut_down_heads(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active
    );

    // Allocates the lit head's cursor channel and the chosen output's cursor memory, mapped and bound as `set_up`
    // describes, or returns `OK` with why the head has no cursor logged, else the error of the call that failed
    int32_t set_up_cursor(
        registers& regs,
        const chip_display& display,
        uint32_t present_heads,
        const first_light& light
    );

    // Clears the cursor memory and parks the cursor, giving in `cursor_context_dma` the context DMA the light's cursor
    // methods name, 0 without cursor memory or room to park, and returning `OK` or the park's error, logged
    int32_t prepare_cursor(uint8_t head, rm::handle& cursor_context_dma);

    // Writes the head's cursor methods and the window's usage bounds into the core channel, then the window's first
    // image into its own channel, naming the base buffer and look-up table by context DMAs `set_up` bound, never 0
    int32_t write_first_image(uint8_t window, const head_light& light, rm::handle cursor_context_dma);

    // Clears and parks the cursor with `prepare_cursor`, then lights `light` with `window`'s first image as `set_up`
    // describes and logs what the armed state reads
    int32_t light_head(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const window_owners& owners,
        const head_light& light,
        uint8_t window
    );

    // Writes the shutdown methods for the heads found active, with the windows' heads only when no update was kicked,
    // then those for the lit head, then an update naming no notifier, and waits for GET to reach PUT
    int32_t take_down(
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active,
        const active_heads& lit
    );
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
