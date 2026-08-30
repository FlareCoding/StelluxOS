#ifndef STLXDM_PRESENTER_HPP
#define STLXDM_PRESENTER_HPP

#include "damage.hpp"

#include <cstdint>

/* The presentation layer. The compositor composes into the target this
 * interface hands out and presents damage through it, so a page-flip
 * or hardware-cursor backend replaces the implementation without
 * touching anything above.
 *
 * Target age counts presents since this buffer was last composed:
 * age 1 means it still holds the previous frame, so only new damage
 * needs repainting, age 0 means the content is undefined. A flipping
 * backend returns age 2 and the damage engine unions the history. */
class presenter {
public:
    struct target {
        uint32_t* pixels = nullptr;
        uint32_t  stride = 0;      /* bytes per row */
        uint32_t  age = 0;
    };

    virtual ~presenter() = default;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    /* The buffer to compose the next frame into. */
    virtual target acquire() = 0;

    /* Pushes the damaged regions of the acquired target to the screen. */
    virtual void present(const damage_list& damage) = 0;
};

/* Composes into one persistent buffer and row-copies damage into the
 * write-combining scanout mapping, the only mode today's hardware has. */
class memcpy_presenter final : public presenter {
public:
    /* Opens and maps the framebuffer. Returns 0, or -1 without one. */
    int init();
    void shutdown();

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    target acquire() override;
    void present(const damage_list& damage) override;

private:
    void copy_rect(const damage_list::rect& r);

    int       m_fd = -1;
    uint8_t*  m_scanout = nullptr;
    uint8_t*  m_back = nullptr;
    uint32_t  m_width = 0;
    uint32_t  m_height = 0;
    uint32_t  m_pitch = 0;
    uint64_t  m_size = 0;
    bool      m_first_acquire = true;
};

#endif
