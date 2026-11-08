#ifndef STLXDM_PICTURE_HPP
#define STLXDM_PICTURE_HPP

#include "damage.hpp"
#include "decor.hpp"
#include "layers.hpp"

#include <stlxwin/proto.h>

#include <cstddef>
#include <cstdint>
#include <memory>

struct dm_window;

/* Pixels stlxdm draws with the CPU through libstlxgfx and the GPU
 * compositor reads where they lie: system memory that the CPU keeps mapped
 * and the GPU reads coherently. Its maker frees the memory when the picture
 * drops, which must come before the maker is destroyed. */
class picture {
public:
    virtual ~picture() = default;

    picture(const picture&) = delete;
    picture& operator=(const picture&) = delete;

    uint8_t* pixels() const { return m_pixels; }
    uint64_t address() const { return m_address; }  /* the GPU's address of `pixels()` */
    size_t bytes() const { return m_bytes; }

protected:
    picture(uint8_t* pixels, uint64_t address, size_t bytes);

private:
    uint8_t* m_pixels;
    uint64_t m_address;
    size_t   m_bytes;
};

/* Makes pictures in memory its GPU reads */
class picture_maker {
public:
    virtual ~picture_maker() = default;

    /* A picture of at least `bytes`, or null when no memory is left */
    virtual std::unique_ptr<picture> make_picture(size_t bytes) = 0;
};

/* A decorated window's frame as four strips of one picture: the top with
 * the title bar, the two sides, and the bottom with the content's last
 * rows, whose rounded corners the content's copy leaves showing. Each strip
 * holds the frame drawn in straight alpha over transparent pixels, so
 * blending it over what lies beneath gives what drawing the frame there
 * gives, but for rounding where two translucent draws stack. */
class frame_picture {
public:
    static constexpr uint32_t STRIPS = layer_window::FRAME_STRIPS;

    /* Draws the frame of `w`, which must be decorated and have a current
     * buffer, when its size, its title or `st` differs from the last draw,
     * making the strips' memory first when they outgrew it. Returns false
     * when memory runs out, and `place` then waits for an update that
     * returns true. */
    bool update(picture_maker& maker, const dm_window& w, const decor::chrome_state& st);

    /* Puts the strips at `w`'s place into `shown`, in the pixels of an
     * output whose top-left pixel lies at (`origin_x`, `origin_y`) on the
     * desktop. The last update must have returned true. */
    void place(const dm_window& w, int32_t origin_x, int32_t origin_y, layer_window& shown) const;

private:
    std::unique_ptr<picture> m_memory;
    damage_list::rect        m_strips[STRIPS];   /* from the content's top-left pixel */
    size_t                   m_offsets[STRIPS] = {};

    bool                m_is_drawn = false;
    uint32_t            m_width = 0;
    uint32_t            m_height = 0;
    decor::chrome_state m_state;
    char                m_title[SWP_TITLE_MAX] = {};
};

#endif
