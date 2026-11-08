#ifndef STLXDM_COMPARE_HPP
#define STLXDM_COMPARE_HPP

#include "damage.hpp"

#include <cstdint>
#include <vector>

/* The rows or columns from `first` up to `end` */
struct span {
    int32_t first = 0;
    int32_t end = 0;
};

/* Where a GPU frame differs from the CPU compositor's in red, green or
 * blue: the pixels outside the regions where stacked translucent draws may
 * round apart, those inside them, and the first outside in row order,
 * else inside */
struct frame_difference {
    uint32_t outside_count = 0;
    uint32_t inside_count = 0;
    int32_t  first_x = 0;
    int32_t  first_y = 0;
};

/* The rows `rects` cover, in order, rows that rectangles share or that
 * touch joined into one span */
void covered_rows(const std::vector<damage_list::rect>& rects, std::vector<span>& rows);

/* Compares two XRGB frames over the pixels `rects` cover, each pixel once,
 * counting those inside `rounding` apart. `rects` must lie inside
 * both frames. */
frame_difference compare_frames(
    const uint8_t* gpu,
    uint32_t gpu_pitch,
    const uint8_t* cpu,
    uint32_t cpu_pitch,
    const std::vector<damage_list::rect>& rects,
    const std::vector<damage_list::rect>& rounding
);

#endif
