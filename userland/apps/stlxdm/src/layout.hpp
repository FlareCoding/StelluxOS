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

/* The desktop's layout. Its members are the outputs stlxdm serves, or
 * the ones it last served while it serves none, and each member's place
 * is the rectangle it shows in desktop coordinates, at its mode's size.
 * One member is the main output.
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

    /* An output to place and its mode's size. */
    struct member {
        uint32_t output = 0;
        int32_t  width = 0;
        int32_t  height = 0;
    };

    struct place {
        uint32_t          output = 0;
        damage_list::rect rect;
    };

    /* Places `members`, which must name distinct outputs in ascending
     * index, at most `MEMBERS_MAX`, each from 1 by 1 to `DESKTOP_LIMIT`
     * by `DESKTOP_LIMIT`. The main output stays main while it is a member,
     * else `primary` is main when it is a member, else the lowest member.
     * The main output sits at (0, 0), and the other members form a row
     * to its right in ascending index, each starting at the previous
     * one's right edge, tops at 0. Returns false for no members, keeping
     * the layout as it last was. */
    bool rebuild(const member* members, uint32_t count, uint32_t primary);

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

    place    m_places[MEMBERS_MAX];
    uint32_t m_count = 0;
    uint32_t m_main = 0;

    int32_t  m_shell_top_rows = 0;
    int32_t  m_shell_bottom_y = 0;
};

#endif
