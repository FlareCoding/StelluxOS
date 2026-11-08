#ifndef STLXDM_PICTURE_HPP
#define STLXDM_PICTURE_HPP

#include "damage.hpp"
#include "decor.hpp"
#include "layers.hpp"

#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct dm_window;

/* Pixels stlxdm draws with the CPU through libstlxgfx and the GPU
 * compositor reads where they lie: system memory that the CPU keeps mapped
 * and the GPU reads coherently, or a twin's video memory, which has no
 * `pixels()`. Its maker frees the memory when the picture drops, which must
 * come before the maker is destroyed. */
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

/* A client's window buffer the GPU compositor imported to read where it
 * lies, never mapped through the import. Its maker frees the import when it
 * drops, which must come before the maker is destroyed. */
class window_import {
public:
    virtual ~window_import() = default;

    window_import(const window_import&) = delete;
    window_import& operator=(const window_import&) = delete;

    uint64_t address() const { return m_address; }  /* the GPU's address of the buffer's first byte */

protected:
    explicit window_import(uint64_t address) : m_address(address) {}

private:
    uint64_t m_address;
};

/* Makes pictures in memory its GPU reads */
class picture_maker {
public:
    virtual ~picture_maker() = default;

    /* A picture of at least `bytes`, or null when no memory is left */
    virtual std::unique_ptr<picture> make_picture(size_t bytes) = 0;

    /* A twin of at least `bytes`: a copy of a picture in video memory that
     * the CPU cannot reach, read by the compose in its place. Null when no
     * memory is left. */
    virtual std::unique_ptr<picture> make_twin(size_t bytes) = 0;
};

/* Bytes at the same offset in a picture and in its twin */
struct picture_copy {
    size_t offset;
    size_t bytes;
};

/* An output's backdrop under the GPU compositor: the wallpaper, and on the
 * main output the panels, drawn by the CPU into a picture, and its twin,
 * which the compose reads. Whoever draws into the surface must mark the
 * rows it changed, which the next compose copies into the twin before
 * reading it. Like its pictures, it must drop before its maker. */
class backdrop_picture {
public:
    /* Makes the picture and its twin at `width` by `height`, every row
     * marked changed, or returns null when memory runs out */
    static std::unique_ptr<backdrop_picture> make(picture_maker& maker, uint32_t width, uint32_t height);

    ~backdrop_picture();

    backdrop_picture(const backdrop_picture&) = delete;
    backdrop_picture& operator=(const backdrop_picture&) = delete;

    /* An opaque surface over the picture, which the CPU draws the
     * backdrop into */
    stlxgfx_surface_t* surface() const { return m_surface; }

    const picture& memory() const { return *m_memory; }
    const picture& twin() const { return *m_twin; }

    /* The twin as the compose reads it, its first pixel at the
     * output's top-left */
    layer_picture twin_layer() const;

    /* Marks `count` rows from row `y` changed, ignoring those outside
     * the picture */
    void mark_rows(int32_t y, int32_t count);

    /* Appends the copies that bring the twin up to date and forgets the
     * changed rows */
    void take_copies(std::vector<picture_copy>& copies);

private:
    /* The rows from `first` up to `end` */
    struct row_span {
        int32_t first;
        int32_t end;
    };

    backdrop_picture() = default;

    std::unique_ptr<picture> m_memory;
    std::unique_ptr<picture> m_twin;
    stlxgfx_surface_t*       m_surface = nullptr;
    std::vector<row_span>    m_changed;   /* sorted, no two touching */
};

/* One of an output's two top pictures: what stlxdm draws above the windows,
 * in straight alpha over transparent pixels, which the compose reads only
 * over the bounds of the draws made into it.
 *
 * A compose calls `begin`, records the bounds of each draw it will make,
 * and in each damaged rectangle calls `clear` once before drawing there. A
 * draw must stay inside its bounds, and the damage must cover a draw that
 * moved or vanished, as on the CPU compositor, so that only this compose's
 * draws hold pixels. Like its picture, it must drop before its maker. */
class top_picture {
public:
    /* Makes a transparent picture at `width` by `height`, or returns null
     * when memory runs out */
    static std::unique_ptr<top_picture> make(picture_maker& maker, uint32_t width, uint32_t height);

    ~top_picture();

    top_picture(const top_picture&) = delete;
    top_picture& operator=(const top_picture&) = delete;

    /* A straight alpha surface over the picture, which the draws go into */
    stlxgfx_surface_t* surface() const { return m_surface; }

    /* Starts a compose, whose draws the next `add_bounds` calls record */
    void begin();

    /* Records that a draw of this compose lies inside `bounds` */
    void add_bounds(const damage_list::rect& bounds);

    /* Clears the last compose's and this compose's draws inside `r`
     * to transparent */
    void clear(const damage_list::rect& r);

    /* The picture over the bounding box of this compose's draws */
    layer_piece layer() const;

private:
    top_picture() = default;

    std::unique_ptr<picture>       m_memory;
    stlxgfx_surface_t*             m_surface = nullptr;
    std::vector<damage_list::rect> m_drawn;
    std::vector<damage_list::rect> m_last_drawn;
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
