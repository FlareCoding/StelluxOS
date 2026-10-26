#ifndef STLXDM_PRESENTER_HPP
#define STLXDM_PRESENTER_HPP

#include "damage.hpp"

#include <cstdint>

/* The presentation layer. The compositor composes into the target this
 * interface hands out and presents damage through it, and the main
 * loop composes again only while `present_pending()` is false, so a
 * backend changes nothing above these calls.
 *
 * The display can take the primary output dark, light it again at
 * another mode or hand the primary to another output, and reports each
 * change of an output as an output event. The main loop reads output
 * events through `event_fd()` and `take_events()`, and on
 * `output_changed()` calls `update_output()`, which reads the primary
 * output again for the presents that follow.
 *
 * Target age counts presents since this buffer was last composed:
 * age 1 means it still holds the previous frame, so only new damage
 * needs repainting, age 0 means the content is undefined. A backend
 * that composes into each of two buffers it flips between returns age
 * 2, and the damage engine unions the history.
 *
 * The compositor may show the pointer on the primary output's hardware
 * cursor (a small picture the display draws over every frame) through
 * `set_cursor()` and `move_cursor()`, and leaves the pointer out of
 * its frames while those calls succeed. A cursor call that reaches the
 * display returns once the display has taken or refused it. */
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

    /* Where frames go after `update_output()`. */
    enum class output_state {
        UNCHANGED, // to the primary output they went to, which did not change
        DARK,      // nowhere: no output is primary and lit, or the presenter could not follow the change
        CHANGED,   // to the primary output read again, which may show none of the earlier frames
    };

    virtual ~presenter() = default;

    /* Closes /dev/gfxfb and frees what the backend's init mapped and
     * allocated. */
    virtual void shutdown() = 0;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    /* The primary output's refresh rate in millihertz as of `init()`
     * or the last `CHANGED`, or 0 when unknown or reading it fails. */
    virtual uint64_t refresh_millihertz() const = 0;

    /* The descriptor that polls readable while the display holds an event
     * for the presenter: a present's end or an output event. */
    virtual int event_fd() const = 0;

    /* Whether a present waits for its end. */
    virtual bool present_pending() const = 0;

    /* Reads the events `event_fd()` holds. Callers must first see
     * `event_fd()` poll readable, since the read otherwise waits for an
     * event, and a signal during the read leaves the events for the next
     * call. A present's end clears `present_pending()`, and an output
     * event sets `output_changed()`. */
    virtual void take_events() = 0;

    /* Whether an output event was read since `update_output()` last read
     * the primary output, so frames may no longer reach the screen. */
    virtual bool output_changed() const = 0;

    /* Reads the primary output again after an output changed, first
     * waiting for a pending present's end, and clears
     * `output_changed()`. After `CHANGED`, `width()`, `height()` and
     * `refresh_millihertz()` give the primary output's mode, a target of
     * another size has age 0 and the hardware cursor may show no picture
     * until callers set one. After `DARK`, the hardware cursor is hidden
     * unless the display refused a lit output's hide, the cursor calls
     * return false, and callers must not call `acquire()` or `present()`
     * until a later `update_output()` returns `CHANGED`, since a backend
     * may hold no frame to compose into. */
    virtual output_state update_output() = 0;

    /* The vertical blanks presents missed since init, or -1 when the
     * backend cannot tell. A present is due at the first vertical blank
     * more than half a refresh period after both acquire() handing out
     * its frame's target and the last latch read, so a frame begun just
     * before a vertical blank is not charged with it. The present misses
     * each vertical blank before its own latch from the due one on, and
     * none when the display refused it or when its frame count or time is
     * not past the last latch's. After init, after a present that ended
     * without latching, after an end that could not be read or after an
     * `update_output()` returning `CHANGED`, no latch is kept to count
     * from, so the next latch counts no misses. */
    virtual int64_t missed_frames() const = 0;

    /* The buffer to compose the next frame into. */
    virtual target acquire() = 0;

    /* Pushes the damaged regions of the acquired target to the screen,
     * first waiting for the last present's end when it has not come.
     * `damage` must cover every pixel the frame changed in the target,
     * since a backend may copy only those. It can return before the
     * frame reaches the screen, and `event_fd()` then reports when
     * this present ends. While `output_changed()` is set it shows
     * nothing, since the primary output may differ from the one the
     * frame was composed for. */
    virtual void present(const damage_list& damage) = 0;

    /* Shows a copy of `picture` on the primary output's hardware cursor,
     * its hotspot at the last `move_cursor()` position. Returns whether the
     * display took it, false without a cursor that holds the picture. The
     * display's first refusal of a cursor call, and its first after each
     * picture it takes, prints a line, but a dark output's refusal prints
     * nothing, since its output event follows. */
    virtual bool set_cursor(const cursor_picture& picture) = 0;

    /* Hides the hardware cursor, which stays shown when the display
     * refuses the hide on a lit output. */
    virtual void hide_cursor() = 0;

    /* Places the hardware cursor's hotspot at (x, y) on the primary
     * output, or records it for the next `set_cursor()` while the cursor
     * is hidden. Returns whether the display took it. */
    virtual bool move_cursor(int32_t x, int32_t y) = 0;
};

/* Composes into one persistent buffer and row-copies damage into the
 * framebuffer mapping, flushing it on displays that need a flush. */
class memcpy_presenter final : public presenter {
public:
    /* Opens and maps the framebuffer and reads the primary output's refresh
     * rate, first waiting for output events while no output is primary and
     * lit. Returns 0, or -1 without a framebuffer. The wait never ends on a
     * display that lights no output. */
    int init();
    void shutdown() override;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }

    /* A present copies within its call and brings no present event, so
     * the events are output events. */
    int event_fd() const override { return m_fd; }
    bool present_pending() const override { return false; }
    void take_events() override;
    bool output_changed() const override { return m_output_changed; }

    /* Reads the framebuffer's layout and maps it again, since the primary
     * output may hold other pixels after any output event, and returns
     * `CHANGED`. Returns `DARK`, keeping the earlier mapping, while no
     * output is primary and lit, and when reading the layout, mapping the
     * pixels or allocating the composed frame fails, printing why for
     * those. */
    output_state update_output() override;

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
 * on the primary output, so while the display works the screen never
 * shows a partly copied frame. Each present the display accepts ends with
 * an event on /dev/gfxfb within `GFXFB_PRESENT_TIMEOUT_NS` of its call
 * returning, not counting time spent waiting for the CPU, and the buffer
 * of a present that latched stays on screen until the output shows
 * another buffer or goes dark. */
class flip_presenter final : public presenter {
public:
    /* Opens /dev/gfxfb and creates and maps two buffers at the primary
     * output's mode, first waiting for output events while no output is
     * primary and lit. Returns 0, or a negative errno: -EBUSY while
     * another program's file controls the display's buffers or reserves
     * control, -ENOTTY on a display that cannot create buffers, as
     * gfxfb's boot display cannot, and otherwise the errno of the step
     * that failed. Every failure but -EBUSY prints its cause, and the
     * wait never ends on a display that lights no output. */
    int init();
    void shutdown() override;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }
    int event_fd() const override { return m_fd; }
    bool present_pending() const override { return m_present_pending; }
    void take_events() override;
    bool output_changed() const override { return m_output_changed; }

    /* Returns `CHANGED` when the primary output is another output, at
     * another size or named by an output event since the last read, or lit
     * after `DARK`, creating the buffers again at its mode after a size
     * change or `DARK`, and `UNCHANGED` otherwise. Returns `DARK` while no
     * output is primary and lit, and when reading the outputs or creating
     * the buffers fails, printing why for those. After a move of the
     * primary, the output that lost it shows no cursor unless the display
     * refused a lit output's hide. */
    output_state update_output() override;

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
    void end_present(uint32_t status, uint64_t frame, uint64_t latch_ns);
    output_state go_dark();
    void count_missed_frames(uint64_t frame, uint64_t latch_ns);
    bool cursor_call(uint32_t request, void* args, const char* call);
    bool has_cursor() const {
        return m_cursor_max_width > 0 && m_cursor_max_height > 0;
    }

    int            m_fd = -1;
    uint32_t       m_output = 0;
    uint32_t       m_width = 0;
    uint32_t       m_height = 0;
    uint64_t       m_refresh_millihertz = 0;
    uint8_t*       m_composed = nullptr;
    display_buffer m_buffers[BUFFER_COUNT];

    /* The primary output's largest cursor picture, 0 by 0 without a
     * hardware cursor and after `DARK`. */
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

    /* Whether an output event was read since `update_output()` last read
     * the primary output, and one bit per output those events named. */
    bool           m_output_changed = false;
    uint32_t       m_changed_outputs = 0;

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
