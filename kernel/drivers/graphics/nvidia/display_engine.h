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
 * A head as the display engine lights it: the channel of its lowest window and
 * its cursor channel, kept from their allocation in `set_up` until `release`,
 * the output it lights with that output's base buffer and cursor memory, and
 * that output's cursor and presents, guarded by the engine's output lock once
 * the head is lit.
 */
struct engine_head {
    display_engine*   engine;                  // set by the engine's constructor, so timer callbacks read it bare
    uint8_t           window;
    rm::handle        window_channel;          // 0 until allocated and after `release`
    dma::buffer       window_push_buffer;
    display_channel*  window_queue;            // built in `window_queue_bytes` with the channel, else null
    uint8_t           output;
    gfxfb::gfxfb_mode mode;
    vram_range        base_buffer;             // of 0 bytes while none is allocated
    rm::handle        base_buffer_context_dma; // 0 until bound
    gfxfb::buffer     userland_base_buffer;    // the base buffer as userland maps it, of 0 bytes while unmapped
    bool              is_lit;                  // set by the light under the engine's output lock

    // The window's notifier, its page allocated and its context DMA bound to the window's channel before the light
    window_notifier notifier;
    rm::handle      notifier_context_dma; // 0 until bound, before flips turn on, so readers after flips read it bare

    // The head's cursor channel and its output's cursor memory, mapped for the CPU and bound to the core channel, the
    // mapping changing only while no cursor call runs, so the calls read it bare
    cursor_channel cursor;
    vram_range     cursor_memory;      // of 0 bytes while none is allocated
    cpu_mapping    cursor_mapping;
    rm::handle     cursor_context_dma; // 0 until bound

    // The output's cursor, which each light and the cursor calls write
    bool     has_cursor;
    bool     is_cursor_shown;
    int32_t  cursor_x;
    int32_t  cursor_y;
    uint32_t cursor_hot_x;
    uint32_t cursor_hot_y;
    uint32_t light_generation;          // raised by each light, so a picture's copy can tell a relight
    bool     has_logged_cursor_refusal; // set by a logged refusal, cleared by a cursor write succeeding
    bool     is_cursor_update_owed;     // an `UPDATE` refused after its position, owed until one is written
    uint32_t owed_cursor_position;      // the position the owed `UPDATE` follows

    // The output's vertical blanks, which the light starts and the interrupt handler counts
    uint32_t                frames_per_second; // the mode's refresh rate rounded, 0 until the light
    uint64_t                frame_count;       // the vertical blanks serviced since the light
    uint64_t                vertical_blank_ns; // when the handler serviced the last of them
    uint16_t                serviced_frame;    // the head's own frame count at that service
    first_second_statistics first_second;
    bool                    has_logged_first_second;

    // The queued present and the latch rules' state, which the light and `present` set and the handler's latch step,
    // the two timers' callbacks, `release` and a cursor write failing the display end
    mode_times            times;
    bool                  has_queued_present;
    uint32_t              queued_slot;
    uint64_t              queued_deadline_ns;
    uint32_t              next_slot;
    uint64_t              check_frame;
    uint64_t              last_reported_frame;
    uint64_t              last_estimate_log_ns; // when the deadline last logged an estimated latch, 0 if never
    timer::deadline_timer latch_check_timer;
    timer::deadline_timer deadline_timer;

    alignas(display_channel) uint8_t window_queue_bytes[sizeof(display_channel)];
};

/**
 * The card's display engine, the part of the GPU that reads images from memory
 * and sends them to monitors, as gfxfb's display. The driver sets it up in
 * `attach()` when the chip's fuses leave the display enabled and releases it in
 * `detach()` before the GSP's firmware unloads, from one thread at a time.
 *
 * It owns the display's instance memory, the firmware's display objects, the
 * core channel, one look-up table surface, and for each output it chose to
 * light the window channel, window notifier, base buffer, cursor channel and
 * cursor memory it allocates. It never frees memory the display engine may
 * still read, such as a push buffer, a notifier's page or a bound surface.
 *
 * A lit output reads at its mode with its base buffer, mapped write-combining,
 * and a cursor of up to 256 by 256 pixels, 0 by 0 when its light found none,
 * and every other output reads dark. One spinlock, the output lock, guards
 * every head's state and is never held across a wait. After `release`, every
 * output reads dark and `flush()`, `present()`, `create_buffer()` and the
 * cursor calls fail, while gfxfb keeps the base buffers, which BAR1 stops
 * translating once `detach()` restores BAR1's block register.
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
     * @brief Release each cursor channel, then each window channel, then the
     * core channel, then the display's objects, each whatever the releases
     * before it return, and a second call releases nothing. From the start,
     * every output reads dark, `flush()`, `present()`, `create_buffer()` and
     * the cursor calls fail, and each queued present ends failed, failing the
     * display. While no update has been sent, the window and core channels
     * are released with the hardware kept as it is, so the boot firmware's
     * heads keep scanning out. Its requests spin on the calling CPU.
     * @return `OK`, else the logged error of the first release to fail.
     */
    int32_t release();

    /**
     * @brief Whether an output is lit, as gfxfb reads it, which holds from
     * when the light lights one until `release`. It must be called only
     * before the driver task starts, while the boot thread alone lights
     * outputs, since it takes no lock.
     * @return true while an output is lit.
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
     * the card's interrupt handler. Each is cleared, and one on a lit head
     * counts a frame and runs the latch step, ending the head's queued present
     * latched or scheduling its latch check, none after `release`, which
     * leaves the lit heads' interrupts enabled. Takes the output lock and
     * never sleeps.
     * @return true at a service completing a lit head's first second.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool service_vertical_blanks(registers& regs, uint32_t pending_heads);

    // Logs each lit head's first second of vertical blanks at most once, after its last service, from the driver task
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
     * @brief Write an update showing `buf` into the window channel of the head
     * lighting `output`, for that head to take at a vertical blank, waiting
     * only for the output lock. The present ends as the latch rules decide, or
     * failed by `release` or a cursor call failing the display.
     * @return gfxfb's errors in its order: `ERR_UNSUPPORTED` with flips off or
     * no window notifier, `ERR_DEVICE` once released or failed, `ERR_DARK` off
     * a lit output, `ERR_INVALID_ARG` off its mode's size and `ERR_REFUSED` for
     * a buffer not bound to that channel or a full push buffer.
     */
    int32_t present(uint32_t output, gfxfb::buffer* buf, const gfxfb::gfxfb_rect* regions, uint32_t count) override;

    int32_t set_cursor(uint32_t output, const gfxfb::cursor_image& image) override;
    int32_t move_cursor(uint32_t output, int32_t x, int32_t y) override;

    /**
     * @brief Queue the driver task's release of a buffer `create_buffer()`
     * made and return without waiting. The task unbinds, unmaps and frees it,
     * or keeps it all for good once the display failed, the engine was
     * released or unbinding it failed, since a head may still scan it. An
     * output's base buffer is kept for good.
     * @param buf An output's base buffer or a buffer `create_buffer()` made,
     * which nothing refers to any longer.
     */
    void release_buffer(gfxfb::buffer* buf) override;

private:
    friend struct display_engine_test_access;

    // Whether flips are on with a window's notifier bound, which `present()` and `create_buffer()` need first
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
    vram_range      m_look_up_table = {};     // the look-up table surface, of 0 bytes while the engine keeps none
    output_modes*   m_output_modes = nullptr; // one per output from the monitors' reads until freed, else null

    operation_queue& m_operations;

    // The context DMA binding the look-up table surface to the core channel and each chosen window's channel, 0 until
    // `set_up` binds it
    rm::handle m_look_up_table_context_dma = 0;

    // Entry i for head i, each lit output on its own head
    engine_head m_heads[DISPLAY_HEADS_MAX] = {};

    // The core channel's completion notifier, its page allocated and its context DMA bound to the channel by `set_up`,
    // and the wait `send_notified_update` gives it for the shutdown update and the light, 0 in tests that skip it
    completion_notifier m_core_notifier;
    rm::handle          m_notifier_context_dma = 0; // 0 until `set_up` binds the notifier's context DMA
    uint64_t            m_notifier_wait_ns = COMPLETION_NOTIFIER_WAIT_NS;

    bool m_has_sent_update = false; // set once an update is kicked on the core channel

    // The park's and the cursor calls' wait for room, changed only while no cursor call runs, so the calls read it bare
    uint64_t m_cursor_wait_ns = gfxfb::GFXFB_PRESENT_TIMEOUT_NS;

    // The core channel's queue of methods, built in `m_core_queue_bytes` once the channel is allocated, null until
    // then and after `release`
    display_channel*                 m_core_queue = nullptr;
    alignas(display_channel) uint8_t m_core_queue_bytes[sizeof(display_channel)];

    // What gfxfb and `has_lit_output()` read beside each head's output: `set_up` records the output count and boot
    // outputs before registration, never to change, and the light and `release` write the flags under `m_output_lock`
    uint8_t        m_output_count = 0;    // at most `gfxfb::GFXFB_OUTPUTS_MAX`
    uint32_t       m_boot_outputs = 0;    // a bit for each output an active head showed, bit 0 output 0's
    bool           m_is_lit = false;      // set once an output lights, read bare only before the driver task starts
    bool           m_is_released = false; // set by `release`, failing the calls the class comment names
    sync::spinlock m_output_lock = sync::SPINLOCK_INIT;

    // Run between a cursor call's locked steps, after each row of a picture and each yield, null outside tests, which
    // set it before their calls, so the calls read it bare
    void (*m_cursor_step_seam)(display_engine& engine) = nullptr;

    // For each head, the output the light gave it, kept after `release`, or `NO_OUTPUT`, stored with release under
    // `m_output_lock` and loaded with acquire by the handler. `set_up` records the head count before flips turn on
    uint8_t            m_head_outputs[DISPLAY_HEADS_MAX];
    uint8_t            m_head_count = 0;
    sync::atomic<bool> m_are_flips_on;      // stored with release, loaded with acquire
    sync::atomic<bool> m_is_display_failed; // set once for good, stored with release, loaded with acquire

    // Where each ended present is reported, gfxfb's `present_done` outside tests
    void (*m_present_done)(gfxfb::display*, uint32_t, const gfxfb::present_result&) = gfxfb::present_done;

    /**
     * @brief Write `buf`'s present for `present`, under `m_output_lock`,
     * setting `window` to the window it names and `write_rc` to the channel's
     * error when a write fails the display, for `present` to log unlocked.
     * @return As `present` returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t queue_present(
        uint32_t output,
        const gfxfb::buffer& buf,
        uint8_t& window,
        int32_t& write_rc
    );

    /**
     * @brief Apply the latch step to the vertical blank just counted on
     * `head`, under `m_output_lock`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void run_latch_step(engine_head& head);

    /**
     * @brief End `head`'s queued present with `status`, recording a latch's
     * report as the last, under `m_output_lock`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void end_present(engine_head& head, uint32_t status, const latch_report& report);

    /**
     * @brief Cancel each head's timers and end its queued present failed,
     * failing the display. The caller must hold `m_output_lock`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void fail_queued_presents();

    // The two timers' callbacks, each taking `m_output_lock` elevated
    static void check_latch(timer::deadline_timer* timer);
    static void expire_deadline(timer::deadline_timer* timer);

    // Marks `light`'s head lit with `has_cursor` and its cursor hidden, and starts its output's vertical blanks: its
    // mode's latch times, its head table entry and its interrupt, taking `m_output_lock` itself
    void mark_head_lit(registers& regs, const head_light& light, bool has_cursor);

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

    // Writes an update naming the cleared notifier, interlocked with the window channels of `window_interlocks`, then
    // kicks each of those channels, waits, reads the notifier's status into `status_word` and stops its writes
    int32_t send_notified_update(uint32_t window_interlocks, uint32_t& status_word);

    // Writes the shutdown update naming the notifier, waits for it and stops later updates from writing the notifier
    int32_t shut_down_heads(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const active_heads& active
    );

    // The lit head showing `output`, or null, for a caller holding `m_output_lock`
    engine_head* find_lit_head(uint32_t output);

    // Kicks the queue of each window channel `window_interlocks` names
    void kick_window_queues(uint32_t window_interlocks);

    // Allocates the cursor channel of `light`'s head and its output's cursor memory, mapped for the CPU and bound to
    // the core channel, or returns `OK`, logged, for an absent head, a refused channel or no room, else the error
    int32_t set_up_cursor(
        registers& regs,
        const chip_display& display,
        uint32_t present_heads,
        const first_light& light
    );

    // Clears the head's cursor memory and parks its cursor, giving in `cursor_context_dma` the context DMA the light's
    // cursor methods name, 0 without cursor memory or room to park, and returning `OK` or the park's error, logged
    int32_t prepare_cursor(uint8_t head, rm::handle& cursor_context_dma);

    // Writes the head's cursor methods and its window's usage bounds into the core channel, then the window's first
    // image into its own channel, naming the head's base buffer and the look-up table by their context DMAs, never 0
    int32_t write_first_image(const head_light& light, rm::handle cursor_context_dma);

    // Parks any cursor, lights `light` with its window's first image, in two updates when the engine's first update
    // changes a window's owner, a window serving no head counting, logs the armed state and sets `shows_cursor_memory`
    int32_t light_head(
        const registers& regs,
        const display_heads& heads,
        uint32_t present_windows,
        const window_owners& owners,
        const head_light& light,
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

    // gfxfb's refusal of a cursor call on `output`, else `gfxfb::OK` with `head` the lit head showing it. The caller
    // must hold `m_output_lock`
    int32_t check_cursor_call(uint32_t output, engine_head*& head);

    // gfxfb's refusal of a picture's next step on `head`, the display failed or `output` not lit there at `generation`,
    // else `gfxfb::OK`. The caller must hold `m_output_lock`
    int32_t check_picture(const engine_head& head, uint32_t output, uint32_t generation) const;

    /**
     * @brief Write `position` and then `UPDATE` into `head`'s cursor channel,
     * `UPDATE` alone when it is owed for `position`, failing the display and
     * the head's queued present when the free register reads all ones. The
     * caller must hold `m_output_lock`.
     * @return As `cursor_channel::write` returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t write_cursor_position(engine_head& head, uint32_t position);

    // Whether a cursor call tries a write refused at `tried_ns` again, after a yield: until a try past `deadline_ns`,
    // which a deadline of 0 sets to `m_cursor_wait_ns` past this first refusal
    bool retries_cursor_write(uint64_t tried_ns, uint64_t& deadline_ns);

    // gfxfb's result for the cursor `call` on `output` whose checks gave `rc` and whose last write on `head` gave
    // `write_rc`, a refusal logged once until a cursor write succeeds and a card gone logged
    int32_t end_cursor_call(uint32_t output, engine_head* head, const char* call, int32_t rc, int32_t write_rc);
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_DISPLAY_ENGINE_H
