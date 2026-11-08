#include "compare.hpp"

#include <algorithm>

/* The bytes of an XRGB pixel that show */
constexpr uint32_t RGB_MASK = 0x00FFFFFF;

/* Sorts `spans` and joins those that overlap or touch, dropping empty ones */
static void join_spans(std::vector<span>& spans) {
    std::sort(spans.begin(), spans.end(), [](const span& a, const span& b) { return a.first < b.first; });

    size_t kept = 0;
    for (const span& s : spans) {
        if (s.end <= s.first) {
            continue;
        }

        if (kept > 0 && s.first <= spans[kept - 1].end) {
            spans[kept - 1].end = std::max(spans[kept - 1].end, s.end);
        } else {
            spans[kept++] = s;
        }
    }

    spans.resize(kept);
}

/* The columns of `rects` that cross row `y`, joined */
static void columns_at(const std::vector<damage_list::rect>& rects, int32_t y, std::vector<span>& columns) {
    columns.clear();
    for (const damage_list::rect& r : rects) {
        if (y >= r.y && y < r.y + r.h) {
            columns.push_back({ r.x, r.x + r.w });
        }
    }

    join_spans(columns);
}

static bool is_inside_any(const std::vector<span>& columns, int32_t x) {
    for (const span& s : columns) {
        if (x >= s.first && x < s.end) {
            return true;
        }
    }

    return false;
}

/* Counts the differing pixel at `x`, `y`, naming the first outside the rounding regions over any inside them */
static void count_difference(frame_difference& difference, int32_t x, int32_t y, bool is_inside) {
    bool is_first = difference.outside_count == 0 && (!is_inside || difference.inside_count == 0);
    if (is_first) {
        difference.first_x = x;
        difference.first_y = y;
    }

    if (is_inside) {
        difference.inside_count++;
    } else {
        difference.outside_count++;
    }
}

void covered_rows(const std::vector<damage_list::rect>& rects, std::vector<span>& rows) {
    rows.clear();
    for (const damage_list::rect& r : rects) {
        rows.push_back({ r.y, r.y + r.h });
    }

    join_spans(rows);
}

frame_difference compare_frames(
    const uint8_t* gpu,
    uint32_t gpu_pitch,
    const uint8_t* cpu,
    uint32_t cpu_pitch,
    const std::vector<damage_list::rect>& rects,
    const std::vector<damage_list::rect>& rounding
) {
    frame_difference difference;
    std::vector<span> rows;
    covered_rows(rects, rows);

    std::vector<span> columns;
    std::vector<span> rounding_columns;
    for (const span& run : rows) {
        for (int32_t y = run.first; y < run.end; y++) {
            columns_at(rects, y, columns);
            columns_at(rounding, y, rounding_columns);

            const uint32_t* gpu_row = reinterpret_cast<const uint32_t*>(gpu + static_cast<size_t>(y) * gpu_pitch);
            const uint32_t* cpu_row = reinterpret_cast<const uint32_t*>(cpu + static_cast<size_t>(y) * cpu_pitch);
            for (const span& columns_run : columns) {
                for (int32_t x = columns_run.first; x < columns_run.end; x++) {
                    if (((gpu_row[x] ^ cpu_row[x]) & RGB_MASK) != 0) {
                        count_difference(difference, x, y, is_inside_any(rounding_columns, x));
                    }
                }
            }
        }
    }

    return difference;
}
