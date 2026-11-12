#ifndef STLXDM_DRAG_LOOK_HPP
#define STLXDM_DRAG_LOOK_HPP

#include "damage.hpp"
#include "decor.hpp"
#include "dnd.hpp"
#include "layers.hpp"
#include "picture.hpp"

#include <cstdint>
#include <memory>
#include <vector>

/* What a drag shows at the pointer, each part drawn once per change into a picture both compositors read: the
 * source's picture with the count badge, the shadow it casts, and the chip. It must drop before its pictures' maker. */
class drag_look {
public:
    /* Draws `session`'s picture, with the badge from two items, and its shadow for `maker`'s compositor, dropping the
     * chip. False when memory runs out, which leaves the drag without a picture. */
    bool draw_picture(picture_maker& maker, const dnd_session& session);

    /* Draws `chip`, which shows nothing without a glyph. False when memory runs out, which leaves no chip. */
    bool draw_chip(const dnd_chip& chip);

    /* The maker of the pictures, null before the first draw */
    picture_maker* maker() const { return m_maker; }

    const dnd_chip& chip() const { return m_chip; }

    /* What the parts cover, in desktop coordinates, for the pointer at (`x`, `y`) on `output` */
    damage_list::rect bounds(int32_t x, int32_t y, const damage_list::rect& output) const;

    /* The shadow and the picture as `scale` layers at `opacity`, and the chip as a blend, in desktop coordinates */
    void pieces(int32_t x, int32_t y, const damage_list::rect& output, uint32_t opacity,
                std::vector<layer_scaled>& scaled, layer_piece& chip) const;

    /* Draws the parts over `canvas` inside `clip`, which lies in desktop coordinates inside the canvas, as a GPU
     * compose of `pieces` draws them */
    void draw(const decor::canvas& canvas, int32_t x, int32_t y, const damage_list::rect& output, uint32_t opacity,
              const damage_list::rect& clip) const;

private:
    bool draw_shadow(picture_maker& maker);
    damage_list::rect picture_rect(int32_t x, int32_t y) const;
    damage_list::rect shadow_rect(int32_t x, int32_t y) const;
    damage_list::rect chip_rect(int32_t x, int32_t y, const damage_list::rect& output) const;

    picture_maker*                   m_maker = nullptr;
    std::unique_ptr<twinned_picture> m_picture;       /* the source's pixels and the badge, premultiplied */
    std::unique_ptr<twinned_picture> m_shadow;        /* ink in straight alpha */
    std::unique_ptr<twinned_picture> m_chip_picture;  /* the chip with its shadow in straight alpha */
    dnd_chip                         m_chip;

    int32_t           m_hot_x = 0;        /* the picture's pixel under the pointer */
    int32_t           m_hot_y = 0;
    int32_t           m_shadow_x = 0;     /* the shadow's top-left from the picture's */
    int32_t           m_shadow_y = 0;
    damage_list::rect m_chip_body;        /* within the chip's picture */
};

#endif
