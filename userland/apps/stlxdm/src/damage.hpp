#ifndef STLXDM_DAMAGE_HPP
#define STLXDM_DAMAGE_HPP

#include <cstdint>

/* Screen-space damage accumulated between compose ticks. Overflow
 * collapses to a full-screen repaint rather than dropping rects, so
 * damage is never lost, only coarsened. */
class damage_list {
public:
    static constexpr uint32_t MAX_RECTS = 32;

    struct rect {
        int32_t x = 0, y = 0;
        int32_t w = 0, h = 0;
    };

    void add(int32_t x, int32_t y, int32_t w, int32_t h);
    void add_full() { m_full = true; }
    void clear();

    bool empty() const { return !m_full && m_count == 0; }
    bool full() const { return m_full; }
    uint32_t count() const { return m_count; }
    const rect& at(uint32_t i) const { return m_rects[i]; }

    /* Clips r against screen bounds, false when nothing remains. */
    static bool clip(rect& r, int32_t screen_w, int32_t screen_h);

private:
    rect m_rects[MAX_RECTS];
    uint32_t m_count = 0;
    bool m_full = false;
};

#endif
