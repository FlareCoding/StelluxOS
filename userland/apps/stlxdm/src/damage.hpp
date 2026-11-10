#ifndef STLXDM_DAMAGE_HPP
#define STLXDM_DAMAGE_HPP

#include <cstdint>

/* Screen-space damage accumulated between compose ticks, as rects that
 * never overlap, since each is composed on its own. Overflow collapses to
 * a full-screen repaint, so damage is never lost, only coarsened. */
class damage_list {
public:
    static constexpr uint32_t MAX_RECTS = 32;

    /* The most rects `subtract` writes */
    static constexpr uint32_t PARTS_OUTSIDE_MAX = 4;

    struct rect {
        int32_t x = 0, y = 0;
        int32_t w = 0, h = 0;
    };

    /* Adds the parts of a rect that the other rects do not cover, split
     * around the rects it overlaps, and drops the rects it holds. */
    void add(int32_t x, int32_t y, int32_t w, int32_t h);
    void add_full() { m_full = true; }
    void clear();

    bool empty() const { return !m_full && m_count == 0; }
    bool full() const { return m_full; }
    uint32_t count() const { return m_count; }
    const rect& at(uint32_t i) const { return m_rects[i]; }

    /* Clips r against screen bounds, false when nothing remains. */
    static bool clip(rect& r, int32_t screen_w, int32_t screen_h);

    /* The part of `a` inside `b`, all zero when they do not overlap. */
    static rect intersect(const rect& a, const rect& b);

    /* Whether `inner` lies inside `outer`. */
    static bool contains(const rect& outer, const rect& inner);

    /* The smallest rect holding `a` and `b`, ignoring one that is empty. */
    static rect union_of(const rect& a, const rect& b);

    /* Writes to `parts` the at most `PARTS_OUTSIDE_MAX` rects that cover
     * the part of `piece` outside `hole`, and returns their count */
    static uint32_t subtract(const rect& piece, const rect& hole, rect* parts);

private:
    rect m_rects[MAX_RECTS];
    uint32_t m_count = 0;
    bool m_full = false;
};

#endif
