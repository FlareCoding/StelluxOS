#ifndef STLXDM_LAYOUT_HPP
#define STLXDM_LAYOUT_HPP

#include "damage.hpp"

#include <cstdint>

/* A point in desktop coordinates, which count monitor pixels right and
 * down from the top-left of the main output (the output carrying the
 * bar and the dock). */
struct desktop_point {
    int32_t x = 0;
    int32_t y = 0;
};

/* The desktop's layout: a place in desktop coordinates for each member,
 * and which member is the main output. The members are the outputs stlxdm
 * serves and, at their last size, those kept through a relight or a
 * client's mode request. While there are none, the layout stays as it was.
 *
 * A layout is valid when no two places overlap, every place connects to
 * the main output's through edges shared over at least one pixel, the
 * main output's top-left is (0, 0) and every corner lies within
 * `DESKTOP_LIMIT` of the origin on both axes, so every sum of coordinates
 * stays far inside 32 bits.
 *
 * A toplevel is in reach while its grip, the square of its title bar
 * above its content's top-left corner, lies in the work areas: the main
 * output's place less the bar's and the dock's rows, and every other
 * member's whole place. */
class layout {
public:
    static constexpr uint32_t MEMBERS_MAX = 16;
    static constexpr int32_t DESKTOP_LIMIT = 1 << 20;

    /* An output to place and its size: its mode's while it is served,
     * else its last place's. */
    struct member {
        uint32_t output = 0;
        int32_t  width = 0;
        int32_t  height = 0;
    };

    struct place {
        uint32_t          output = 0;
        damage_list::rect rect;
    };

    /* A member's place in a stored layout or a request: its top-left and
     * the size it was stored at, which a request leaves 0 by 0. */
    struct entry {
        uint32_t output = 0;
        int32_t  x = 0;
        int32_t  y = 0;
        int32_t  width = 0;
        int32_t  height = 0;
    };

    /* A stored layout or a request: its main output and entries. */
    struct arrangement {
        uint32_t main = 0;
        uint32_t count = 0;
        entry    entries[MEMBERS_MAX];
    };

    /* What `rebuild` did: nothing for no members, or placed them by the
     * rules, or put the default in place of an invalid result. */
    enum class outcome { kept, placed, defaulted };

    /* Why `apply` refused a request. */
    enum class fault { none, invalid, overlap, apart };

    /* Places `members`, which must be distinct outputs in ascending index,
     * at most `MEMBERS_MAX`, each 1 to `DESKTOP_LIMIT` wide and high.
     * `stored`, whose main must be a member, gives the main output and
     * places, shifted where a size differs. Else the main output stays
     * while a member, else `primary` when one, else the lowest, and a
     * default layout or none gives the default, any other the shift, drop
     * and append rules. The main output then moves to (0, 0), the default
     * replacing an invalid result. Returns `kept` for no members. */
    outcome rebuild(const member* members, uint32_t count, uint32_t primary, const arrangement* stored);

    /* Puts every member at `request`'s top-left for it, at its own size,
     * with `request`'s main output moved to (0, 0), and returns `none`. A
     * refused request changes nothing: `invalid` when its entries are not
     * the members, each once, with a member as main, or a corner passes
     * the limit, `overlap` when two places overlap and `apart` when one
     * does not connect to the main output, checked in that order. */
    fault apply(const arrangement& request);

    uint32_t count() const { return m_count; }
    const place& at(uint32_t index) const { return m_places[index]; }
    uint32_t main_output() const { return m_main; }

    /* `output`'s place, or null when it is no member. */
    const place* find(uint32_t output) const;

    /* The output whose place holds (`x`, `y`), or -1 when none does. */
    int32_t output_at(int64_t x, int64_t y) const;

    bool is_valid() const;

    /* Leaves out of the main output's work area the bar's rows above
     * `top_rows` and the dock's rows from `bottom_y` down, both counted
     * from that output's top. */
    void set_shell_rows(int32_t top_rows, int32_t bottom_y);

    /* `output`'s work area, empty when it is no member. */
    damage_list::rect work_area(uint32_t output) const;

    /* Whether the members' work areas cover every pixel of `r`. */
    bool covers(const damage_list::rect& r) const;

    /* The member holding the largest area of `frame`, the lowest output
     * on a tie, counting only members of `among` too when it is not null.
     * Returns -1 when no such member holds any of it. */
    int32_t output_holding(const damage_list::rect& frame, const layout* among) const;

    /* The point of a place nearest to `point`, the lowest output's on a
     * tie. It is `point` itself when a place holds it. */
    desktop_point nearest_point(desktop_point point) const;

    /* Where the pointer at `from`, which must lie on a place, lands after
     * a straight motion of (`dx`, `dy`): it leaves a place only through
     * an edge another member shares there, entering that one, and
     * otherwise stops at the place's boundary and slides along it. */
    desktop_point move_pointer(desktop_point from, int32_t dx, int32_t dy) const;

    /* The point in this layout for `point` in `before`: the same offset
     * from the top-left of the output whose place in `before` holds it,
     * clamped to that output's place, or the main output's center when
     * no place of `before` holds it or its output is no member here. */
    desktop_point follow_point(desktop_point point, const layout& before) const;

    /* The move keeping in reach a toplevel on `output` whose content's
     * top-left corner is at, or was promised, each of `corners`, with a
     * grip `grip_size` pixels square above each corner. Zero while the
     * work areas cover every grip or `output` has no work area, else the
     * least move putting the grips at the smallest and the largest x and y
     * inside `output`'s work area, or the largest's alone on an axis
     * where they spread wider. */
    desktop_point reach_move(uint32_t output, const desktop_point* corners, uint32_t count, int32_t grip_size) const;

private:
    uint32_t choose_main(const member* members, uint32_t count, uint32_t primary) const;
    int32_t index_at(int64_t x, int64_t y) const;

    /* Whether the places are the default for their own outputs and sizes
     * with the current main output, as they are while there are none. */
    bool is_default() const;

    place    m_places[MEMBERS_MAX];
    uint32_t m_count = 0;
    uint32_t m_main = 0;

    int32_t  m_shell_top_rows = 0;
    int32_t  m_shell_bottom_y = 0;
};

#endif
