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

/* Where a drag's look shows: the picture's rect, which its shadow follows at the same scale, the opacities of both,
 * and the chip beside the pointer at (`pointer_x`, `pointer_y`) on `output` unless `has_chip` is clear */
struct drag_pose {
    damage_list::rect picture;
    uint32_t          opacity = 0;
    uint32_t          shadow_opacity = 0;
    bool              has_chip = false;
    int32_t           pointer_x = 0;
    int32_t           pointer_y = 0;
    damage_list::rect output;
};

/* What a drag shows, each part drawn once per change into a picture both compositors read: the source's picture with
 * the count badge, the shadow it casts, and the chip. It must drop before its pictures' maker. */
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

    /* The picture's rect with its hotspot at (`x`, `y`), empty without a picture */
    damage_list::rect picture_at(int32_t x, int32_t y) const;

    /* What the parts cover in `pose`, in desktop coordinates */
    damage_list::rect bounds(const drag_pose& pose) const;

    /* The shadow and the picture as `scale` layers, and the chip as a blend, in `pose` in desktop coordinates */
    void pieces(const drag_pose& pose, std::vector<layer_scaled>& scaled, layer_piece& chip) const;

    /* Draws the parts in `pose` over `canvas` inside `clip`, which lies in desktop coordinates inside the canvas, as a
     * GPU compose of `pieces` draws them */
    void draw(const decor::canvas& canvas, const drag_pose& pose, const damage_list::rect& clip) const;

private:
    bool draw_shadow(picture_maker& maker);
    damage_list::rect shadow_rect(const damage_list::rect& picture) const;
    damage_list::rect chip_rect(const drag_pose& pose) const;

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
