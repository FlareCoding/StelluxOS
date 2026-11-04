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
 * What powering down each output's link takes, read from its connector and its
 * monitor's DPCD.
 */
struct power_down_facts {
    uint32_t               d3_outputs;             // a bit for each output whose DPCD read gives revision 1.1 or later
    displayport::link_rate rates[DISPLAY_IDS_MAX]; // each output's highest link rate, named by its zero-lane training
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
 * other output reads as dark. No output relights, and `flush()` returns at
 * once on the lit output, since its head scans the base buffer itself.
 * `create_buffer()` makes `scanout_buffer`s, which the driver task binds to
 * the lit window's channel, if any, and `present()` takes those and the base
 * buffer. `release_buffer()` keeps the base buffer, a bound surface, for good.
 * `read_output()`, `base_buffer()` and `flush()` read the chosen output's
 * state under its spinlock, which no holder keeps across a wait, so they never
 * sleep or wait on the card.
 *
 * The lit output has a hardware cursor of up to 256 by 256 pixels unless its
 * light found none for its head, when it reports a cursor maximum of 0 by 0,
 * and every other output reports 256 by 256. `move_cursor()` places the
 * cursor's hotspot at the position, only recording it while the cursor is
 * hidden, and `set_cursor()` shows the picture with its hotspot there. A
 * cursor call whose write finds no room in the cursor channel returns
 * `gfxfb::ERR_REFUSED` once a try `gfxfb::GFXFB_PRESENT_TIMEOUT_NS` past the
 * first refusal, or past a picture's read back, is refused too, so it returns
 * within a picture's copy and read back, that wait and one wait for the CPU
 * after it when its task waited for the CPU. Measured on one card, a copy took
 * up to 0.51 ms, holding the output's spinlock for one row at a time, up to
 * 0.19 ms. A cursor call's write finding the free register all ones, as a card
 * gone reads, fails the display.
 *
 * Once the display fails, as `present()` says, or `release` runs, `flush()`
 * and a supported `present()`, `create_buffer()` or cursor call fail with
 * `gfxfb::ERR_DEVICE`. After `release`, `read_output()` reads every output as
 * dark too, but gfxfb keeps its record of the base buffer, since `detach()`
 * runs on the boot thread and only a task may report an output gone dark,
 * while BAR1 stops translating that buffer's address once `detach()` restores
 * BAR1's block register.
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
     * @brief Light one connected monitor with a black base buffer at the first
     * mode the bandwidth check accepts, powering down the link of each other
     * output the boot firmware lit once the shutdown update stops its heads.
     * Call it once, outside interrupts and spinlocks, while nothing else uses
     * video memory or the firmware, with `regs` mapped until `release` returns.
     * @return `OK`, also when nothing lights, else the logged error that ended
     * the set-up: a refused binding, before the choice a refused request it
     * needs or no memory, or a request or wait failing other than by a refusal
     * or, before the core channel's idle wait, no room, but no failed update.
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
     * `read_output()` reads every output as dark and `flush()`, `present()`,
     * `create_buffer()` and the cursor calls fail, then drop the channels'
     * queues of methods, so nothing writes a released channel's area, free the
     * outputs' modes if `set_up` kept them, then release the lit head's cursor
     * channel as `cursor_channel::release` describes, then the chosen window's
     * channel and then the core channel, each once `set_up` allocated it or
     * the firmware may hold it after a failed allocation, as
     * `release_display_channel` describes, then the display's objects, once
     * `set_up` allocated them, as `release_display_objects` describes. While
     * no update has been kicked, the window's and the core's are released with
     * the hardware kept as it is, so the heads the boot firmware lit keep
     * scanning out the boot framebuffer, and once one has, without keeping it,
     * since the update changed the boot firmware's state. Its requests spin on
     * the calling CPU, each waiting for its reply as `firmware` describes.
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
     * only for the output's spinlock. The interrupt handler or a timer ends
     * the present by the latch rules (latch_rules.h), or `release` or a cursor
     * call failing the display ends it failed.
     * @return gfxfb's errors in its order: `ERR_UNSUPPORTED` with flips off or
     * no window notifier, `ERR_DEVICE` once released or failed (a refused GET,
     * a missed deadline or a cursor write finding the free register all ones
     * fails the display), `ERR_DARK` off the lit output, `ERR_INVALID_ARG` off
     * its mode's size, and `ERR_REFUSED` for a buffer not bound to the
     * window's channel or a full push buffer.
     */
    int32_t present(uint32_t output, gfxfb::buffer* buf, const gfxfb::gfxfb_rect* regions, uint32_t count) override;

    int32_t set_cursor(uint32_t output, const gfxfb::cursor_image& image) override;
    int32_t move_cursor(uint32_t output, int32_t x, int32_t y) override;

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
    // until bound. The mapping and the wait change only while no cursor call runs, so the calls read them bare
    cursor_channel m_cursor_channel;
    vram_range     m_cursor_memory = {};
    cpu_mapping    m_cursor_mapping = {};
    rm::handle     m_cursor_context_dma = 0;
    uint64_t       m_cursor_wait_ns = gfxfb::GFXFB_PRESENT_TIMEOUT_NS; // the park and cursor calls' wait for room

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
    bool              m_is_released = false;          // set by `release`, failing the calls the class comment names
    sync::spinlock    m_output_lock = sync::SPINLOCK_INIT;

    // The chosen output's cursor, which each light and the cursor calls write under `m_output_lock`
    bool     m_has_cursor = false;
    bool     m_is_cursor_shown = false;
    int32_t  m_cursor_x = 0;
    int32_t  m_cursor_y = 0;
    uint32_t m_cursor_hot_x = 0;
    uint32_t m_cursor_hot_y = 0;
    uint32_t m_light_generation = 0;              // raised by each light, so a picture's copy can tell a relight
    bool     m_has_logged_cursor_refusal = false; // set by a logged refusal, cleared by a cursor write succeeding
    bool     m_is_cursor_update_owed = false;     // an `UPDATE` refused after its position, owed until one is written
    uint32_t m_owed_cursor_position = 0;          // the position the owed `UPDATE` follows

    // Run between a cursor call's locked steps, after each row of a picture and each yield, null outside tests, which
    // set it before their calls, so the calls read it bare
    void (*m_cursor_step_seam)(display_engine& engine) = nullptr;

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

    // Brings the engine up for the chosen output, from the mode set's start to its end
    int32_t bring_up(
        registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const window_owners& owners,
        const active_heads& active,
        bool has_crossbar,
        const display_outputs& outputs,
        const power_down_facts& power_down,
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

    // Allocates the lit head's cursor channel and the chosen output's cursor memory, mapped for the CPU and bound to
    // the core channel, or returns `OK`, logged, for an absent head, a refused channel or no room, else the error
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

    // Parks any cursor, then lights `light` with `window`'s first image, in two updates when the engine's first update
    // changes a window's owner, a window serving no head counting, logs the armed state and sets `shows_cursor_memory`
    int32_t light_head(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const window_owners& owners,
        const head_light& light,
        uint8_t window,
        bool& shows_cursor_memory
    );

    // Writes the shutdown methods for the heads found active, with the windows' heads only when no update was kicked,
    // then those for the lit head, then an update naming no notifier, and waits for GET to reach PUT
    int32_t take_down(
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active,
        const active_heads& lit
    );

    // Hides the cursor as `set_cursor` does for a width of 0, parking it while it shows
    int32_t hide_cursor(uint32_t output);

    // Copies `image` into the cursor memory a row at a time, then shows it at the recorded position
    int32_t show_cursor_picture(uint32_t output, const gfxfb::cursor_image& image);

    // gfxfb's refusal of a cursor call on `output`, else `gfxfb::OK`. The caller must hold `m_output_lock`
    int32_t check_cursor_call(uint32_t output) const;

    // gfxfb's refusal of a picture's next step, the display failed or the output not lit at `generation`, else
    // `gfxfb::OK`. The caller must hold `m_output_lock`
    int32_t check_picture(uint32_t output, uint32_t generation) const;

    /**
     * @brief Write `position` and then `UPDATE`, `UPDATE` alone when it is
     * owed for `position`, failing the display and its queued present when
     * the free register reads all ones. The caller must hold `m_output_lock`.
     * @return As `cursor_channel::write` returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t write_cursor_position(uint32_t position);

    // Whether a cursor call tries a write refused at `tried_ns` again, after a yield: until a try past `deadline_ns`,
    // which a deadline of 0 sets to `m_cursor_wait_ns` past this first refusal
    bool retries_cursor_write(uint64_t tried_ns, uint64_t& deadline_ns);

    // gfxfb's result for the cursor `call` on `output` whose checks gave `rc` and whose last write gave `write_rc`,
    // a refusal logged once until a cursor write succeeds and a card gone logged
    int32_t end_cursor_call(uint32_t output, const char* call, int32_t rc, int32_t write_rc);
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
