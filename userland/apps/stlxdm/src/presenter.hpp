#ifndef STLXDM_PRESENTER_HPP
#define STLXDM_PRESENTER_HPP

#include "damage.hpp"

#include <cstdint>

/* The presentation layer. The compositor composes into the target this
 * interface hands out and presents damage through it, and the main
 * loop composes again only once end_fd() shows the last present ended,
 * so a backend changes nothing above these calls.
 *
 * Target age counts presents since this buffer was last composed:
 * age 1 means it still holds the previous frame, so only new damage
 * needs repainting, age 0 means the content is undefined. A backend
 * that composes into each of two buffers it flips between returns age
 * 2, and the damage engine unions the history. */
class presenter {
public:
    struct target {
        uint32_t* pixels = nullptr;
        uint32_t  stride = 0;      /* bytes per row */
        uint32_t  age = 0;
    };

    virtual ~presenter() = default;

    /* Closes /dev/gfxfb and frees what the backend's init mapped and
     * allocated. */
    virtual void shutdown() = 0;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    /* The primary output's refresh rate in millihertz, or 0 when it is
     * unknown, no output is primary or the rate cannot be read. */
    virtual uint64_t refresh_millihertz() const = 0;

    /* The descriptor that polls readable when the last present has ended,
     * or -1 when no present is waiting for its end. */
    virtual int end_fd() const = 0;

    /* Reads the last present's end. Callers must first see end_fd() poll
     * readable, since the read otherwise waits for that end, and a signal
     * during the read leaves end_fd() set. */
    virtual void take_end() = 0;

    /* The buffer to compose the next frame into. */
    virtual target acquire() = 0;

    /* Pushes the damaged regions of the acquired target to the screen,
     * first waiting for the last present's end when it has not come.
     * `damage` must cover every pixel the frame changed in the target,
     * since a backend may copy only those. It can return before the
     * frame reaches the screen, and end_fd() then reports when this
     * present ends. */
    virtual void present(const damage_list& damage) = 0;
};

/* Composes into one persistent buffer and row-copies damage into the
 * framebuffer mapping, flushing it on displays that need a flush. */
class memcpy_presenter final : public presenter {
public:
    /* Opens and maps the framebuffer and reads the primary output's refresh
     * rate. Returns 0, or -1 without a framebuffer. */
    int init();
    void shutdown() override;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }

    /* A present ends within its call, so none waits for its end. */
    int end_fd() const override { return -1; }
    void take_end() override {}

    target acquire() override;
    void present(const damage_list& damage) override;

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
     * output's mode. Returns 0, or a negative errno: -EBUSY while another
     * program's file controls the display's buffers or reserves control,
     * -ENOTTY on a display that cannot create buffers, as gfxfb's boot
     * display cannot, and otherwise the errno of the step that failed
     * (-EAGAIN with no primary output). Every failure but -EBUSY prints
     * its cause. */
    int init();
    void shutdown() override;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    uint64_t refresh_millihertz() const override { return m_refresh_millihertz; }
    int end_fd() const override { return m_present_pending ? m_fd : -1; }
    void take_end() override;
    target acquire() override;
    void present(const damage_list& damage) override;

private:
    struct display_buffer {
        uint32_t handle = 0;
        uint32_t pitch = 0;
        uint64_t size = 0;
        uint8_t* pixels = nullptr;
    };

    static constexpr uint32_t BUFFER_COUNT = 2;

    int create_buffer(display_buffer& buffer);
    bool present_buffer(const display_buffer& buffer, const damage_list& changed);

    int            m_fd = -1;
    uint32_t       m_output = 0;
    uint32_t       m_width = 0;
    uint32_t       m_height = 0;
    uint64_t       m_refresh_millihertz = 0;
    uint8_t*       m_composed = nullptr;
    display_buffer m_buffers[BUFFER_COUNT];

    /* The regions each buffer's next present names: those the composed
     * frame changed since that buffer's last accepted present, the whole
     * buffer before its first. */
    damage_list    m_unpresented[BUFFER_COUNT];
    uint32_t       m_back_index = 0;
    bool           m_first_acquire = true;
    bool           m_present_pending = false;
    bool           m_last_present_failed = false;
};

#endif
