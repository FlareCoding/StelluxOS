#ifndef STLXDM_PRESENTER_HPP
#define STLXDM_PRESENTER_HPP

#include "damage.hpp"

#include <stlx/gfxfb_abi.h>

#include <cstdint>

class display;

/* The presentation layer for one output. The compositor composes into
 * the target this interface hands out and presents damage through it,
 * and the main loop composes again only while `present_pending()` is
 * false. `display` makes and drops presenters as it follows output
 * events, and hands each present's end to its output's presenter.
 *
 * Target age counts presents since this buffer was last composed: age 1
 * means it still holds the previous frame, a larger age an older frame,
 * and age 0 that its content is undefined.
 *
 * The compositor may show the pointer on the output's hardware cursor (a
 * small picture the display draws over every frame) through `set_cursor()`
 * and `move_cursor()`, leaving it out of its frames while those calls
 * succeed. A cursor call that reaches the display returns once the
 * display has taken or refused it. */
class presenter {
public:
    struct target {
        uint32_t* pixels = nullptr;
        uint32_t  stride = 0;      /* bytes per row */
        uint32_t  age = 0;
    };

    /* A hardware cursor picture: `width` by `height` pixels in unpadded
     * rows and the hotspot (the pixel that sits at the cursor's
     * position), which must lie inside the picture. Each pixel holds
     * alpha in bits 31 to 24 and red, green and blue as a frame's pixel
     * does, none multiplied by alpha. */
    struct cursor_picture {
        const uint32_t* pixels = nullptr;
        uint32_t        width = 0;
        uint32_t        height = 0;
        uint32_t        hot_x = 0;
        uint32_t        hot_y = 0;
    };

    virtual ~presenter() = default;

    /* Hides the hardware cursor and frees what the backend mapped and
     * allocated, leaving /dev/gfxfb open for `display`. */
    virtual void shutdown() = 0;

    /* The output the presenter shows its frames on. */
    virtual uint32_t output() const = 0;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    /* The output's refresh rate in millihertz, 0 when unknown. */
    virtual uint64_t refresh_millihertz() const = 0;

    /* Whether a present waits for its end. */
    virtual bool present_pending() const = 0;

    /* Marks that an output event named the output, or any output for the
     * memcpy presenter, so frames may no longer reach its screen until
     * `display` follows the event. */
    virtual void note_output_event() = 0;

    /* The vertical blanks presents missed since init, or -1 when the
     * backend cannot tell. A present is due at the first vertical blank
     * more than half a refresh period after both acquire() handing out its
     * target and the last latch read, and misses each vertical blank
     * before its own latch from the due one on. It misses none when the
     * display refused it or its latch is not past the last one, nor after
     * init, `relit()` or an end that latched nothing or could not be read,
     * since those leave no latch to count from. */
    virtual int64_t missed_frames() const = 0;

    /* The buffer to compose the next frame into. */
    virtual target acquire() = 0;

    /* Pushes the damaged regions of the acquired target to the screen,
     * first waiting for the last present's end when it has not come.
     * `damage` must cover every pixel the frame changed in the target,
     * since a backend may copy only those. It can return before the
     * frame reaches the screen, `present_pending()` staying true until
     * `display` hands over the present's end. After `note_output_event()`
     * it shows nothing until `display` follows the event, since the output
     * may differ from the one the frame was composed for. */
    virtual void present(const damage_list& damage) = 0;

    /* Shows a copy of `picture` on the output's hardware cursor, its
     * hotspot at the last `move_cursor()` position. Returns whether the
     * display took it, false without a cursor that holds the picture. The
     * display's first refusal of a cursor call, and its first after each
     * picture it takes, prints a line, but a dark output's refusal prints
     * nothing, since its output event follows. */
    virtual bool set_cursor(const cursor_picture& picture) = 0;

    /* Hides the hardware cursor, which stays shown when the display
     * refuses the hide on a lit output. */
    virtual void hide_cursor() = 0;

    /* Places the hardware cursor's hotspot at (x, y) in the output's
     * pixels, even where the picture then lies partly off the output, or
     * records it for the next `set_cursor()` while the cursor is hidden.
     * Returns whether the display took it. */
    virtual bool move_cursor(int32_t x, int32_t y) = 0;
};

/* Composes into one persistent buffer and row-copies damage into the
 * framebuffer mapping of whichever output is primary, flushing it on
 * displays that need a flush. */
class memcpy_presenter final : public presenter {
public:
    /* Maps the primary output's framebuffer through `fd`, which `display`
     * keeps open. Returns what `update_output()` returns. */
    int init(int fd);
    void shutdown() override;

    uint32_t output() const override { return m_output; }
    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }

    /* A present copies within its call and brings no present event. */
    bool present_pending() const override { return false; }
    void note_output_event() override { m_output_changed = true; }

    /* Reads the primary output's state and the framebuffer's layout and
     * maps it again, since the primary output may hold other pixels after
     * any output event. Returns 0, else keeps the earlier mapping and
     * returns -EAGAIN while no output is primary and lit or
     * `GFXFB_GET_INFO` fails with EAGAIN, or -1 after printing why a read,
     * the mapping or the composed frame's allocation failed. */
    int update_output();

    /* The framebuffer reports no vertical blanks to count. */
    int64_t missed_frames() const override { return -1; }

    target acquire() override;
    void present(const damage_list& damage) override;

    /* The boot display has no hardware cursor, and a cursor call on another
     * display would take control of its buffers, so these refuse. */
    bool set_cursor(const cursor_picture&) override { return false; }
    void hide_cursor() override {}
    bool move_cursor(int32_t, int32_t) override { return false; }

private:
    void flush_display(const damage_list::rect* rects, uint32_t count);

    int       m_fd = -1;
    uint32_t  m_output = 0;
    uint8_t*  m_scanout = nullptr;
    uint8_t*  m_back = nullptr;
    uint32_t  m_width = 0;
    uint32_t  m_height = 0;
    uint32_t  m_pitch = 0;
    uint64_t  m_size = 0;
    uint64_t  m_refresh_millihertz = 0;
    bool      m_first_acquire = true;
    bool      m_needs_flush = false;
    bool      m_last_flush_failed = false;
    bool      m_output_changed = false;
};

/* Composes into one cached buffer, copies each frame's changes into the
 * one of two display buffers that is off screen and presents that buffer
 * on its output, so while the display works the screen never shows a
 * partly copied frame. Each present the display accepts ends with an
 * event on /dev/gfxfb within `GFXFB_PRESENT_TIMEOUT_NS` of its call
 * returning, not counting time spent waiting for the CPU, and the buffer
 * of a present that latched stays on screen until the output shows
 * another buffer or goes dark. */
class flip_presenter final : public presenter {
public:
    /* Creates and maps two buffers at the mode `info` gives the lit
     * output, through `owner`'s descriptor, and keeps `owner`, which must
     * outlive the presenter, to wait on its events. Returns 0, or a
     * negative errno: -EBUSY while another program's file controls the
     * display's buffers or reserves control, -ENOTTY on a display that
     * cannot create buffers, as gfxfb's boot display cannot, and
     * otherwise the errno of the step that failed, printing why for all
     * but -EBUSY. */
    int init(display& owner, const gfxfb::gfxfb_output_info& info);
    void shutdown() override;

    uint32_t output() const override { return m_output; }
    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }
    bool present_pending() const override { return m_present_pending; }
    void note_output_event() override { m_output_changed = true; }

    /* Follows an output event that leaves the output served at its size:
     * forgets the last latch, so the next one counts no misses, names the
     * whole frame in both buffers' next presents and reads the refresh
     * rate and cursor limits again. */
    void relit(const gfxfb::gfxfb_output_info& info);

    /* Ends the pending present with the display's `status`, frame count
     * and latch time. `abandon_present()` ends it with no latch kept,
     * after an event read failed. */
    void end_present(uint32_t status, uint64_t frame, uint64_t latch_ns);
    void abandon_present();

    int64_t missed_frames() const override { return static_cast<int64_t>(m_missed_frames); }
    target acquire() override;
    void present(const damage_list& damage) override;
    bool set_cursor(const cursor_picture& picture) override;
    void hide_cursor() override;
    bool move_cursor(int32_t x, int32_t y) override;

private:
    struct display_buffer {
        uint32_t handle = 0;
        uint32_t pitch = 0;
        uint64_t size = 0;
        uint8_t* pixels = nullptr;
    };

    static constexpr uint32_t BUFFER_COUNT = 2;

    int create_buffers();
    int create_buffer(display_buffer& buffer);
    void release_buffers();
    bool present_buffer(const display_buffer& buffer, const damage_list& changed);
    void count_missed_frames(uint64_t frame, uint64_t latch_ns);
    bool cursor_call(uint32_t request, void* args, const char* call);
    bool has_cursor() const {
        return m_cursor_max_width > 0 && m_cursor_max_height > 0;
    }

    display*       m_display = nullptr;
    int            m_fd = -1;
    uint32_t       m_output = 0;
    uint32_t       m_width = 0;
    uint32_t       m_height = 0;
    uint64_t       m_refresh_millihertz = 0;
    uint8_t*       m_composed = nullptr;
    display_buffer m_buffers[BUFFER_COUNT];

    /* The largest cursor picture, 0 by 0 without a hardware cursor. */
    uint32_t       m_cursor_max_width = 0;
    uint32_t       m_cursor_max_height = 0;

    /* Whether a cursor call failed for any reason but a dark output and
     * the display took no picture after it, so a run of failures prints
     * one line. */
    bool           m_cursor_failing = false;

    /* The regions each buffer's next present names: those the composed
     * frame changed since that buffer's last accepted present, the whole
     * buffer before its first. */
    damage_list    m_unpresented[BUFFER_COUNT];
    uint32_t       m_back_index = 0;
    bool           m_first_acquire = true;
    bool           m_present_pending = false;
    bool           m_last_present_failed = false;

    /* Set by `note_output_event()` until `display` follows the event. */
    bool           m_output_changed = false;

    /* When acquire() handed out the target of the frame being composed and
     * of the pending one, whether a latch is kept to count from and its
     * frame count and time, and the vertical blanks missed since init. */
    uint64_t       m_frame_start_ns = 0;
    uint64_t       m_pending_frame_start_ns = 0;
    bool           m_has_latch = false;
    uint64_t       m_latch_frame = 0;
    uint64_t       m_latch_ns = 0;
    uint64_t       m_missed_frames = 0;
};

#endif
