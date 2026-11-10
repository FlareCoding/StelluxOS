#include "animation.hpp"
#include "server.hpp"

#include <stlxgfx/blend.h>

#include <algorithm>

/* Maximize and restore ease over this, a move to or from the dock over this */
constexpr uint64_t RESIZE_NS = 180000000ull;
constexpr uint64_t DOCK_NS = 220000000ull;

/* Eased progress runs from 0 to this */
constexpr int64_t PROGRESS_ONE = 1 << 16;
constexpr uint32_t PROGRESS_BITS = 16;

/* As in compose.comp, a step counts 65536ths of a source pixel per output pixel, a position 256ths of a source
 * pixel, and an output pixel's four bilinear weights sum to 65536 */
constexpr uint32_t STEP_BITS = 16;
constexpr uint32_t POSITION_BITS = 8;
constexpr int32_t POSITION_ONE = 1 << POSITION_BITS;
constexpr uint32_t WEIGHT_BITS = 2 * POSITION_BITS;
constexpr uint32_t WEIGHT_HALF = 1u << (WEIGHT_BITS - 1);

constexpr uint32_t CHANNEL_SHIFTS[] = { 16, 8, 0 };
constexpr uint32_t CHANNELS = sizeof(CHANNEL_SHIFTS) / sizeof(CHANNEL_SHIFTS[0]);
constexpr uint32_t BYTES_PER_PIXEL = 4;

namespace {

/* An output pixel's two source pixels along one axis, and the second's weight in 256ths, below `POSITION_ONE` */
struct axis_sample {
    int32_t  first = 0;
    int32_t  second = 0;
    uint32_t weight = 0;
};

/* The bilinear sums of an output pixel's source pixels: the weighted alpha and per channel the weighted alpha times
 * the channel, or for content the weights and the weighted channels */
struct sample_sums {
    uint32_t alpha = 0;
    uint32_t channels[CHANNELS] = {};
};

/* How one axis of a window's frame maps into its animated rect, the shadow beyond it included: alike along the
 * whole span, or keeping all before `body_first` and from `body_end` at its size and scaling the body between */
struct axis_map {
    int32_t frame_first = 0;
    int32_t frame_end = 0;
    int32_t body_first = 0;
    int32_t body_end = 0;
    int32_t rect_first = 0;
    int32_t rect_end = 0;
    bool    keeps_ends = false;
};

} // namespace

/* Cubic ease out of the elapsed share of `duration`, in `PROGRESS_ONE` units */
static int64_t ease_out(uint64_t elapsed, uint64_t duration) {
    if (elapsed >= duration) {
        return PROGRESS_ONE;
    }

    int64_t remaining = PROGRESS_ONE - static_cast<int64_t>(elapsed * PROGRESS_ONE / duration);
    return PROGRESS_ONE - ((remaining * remaining >> PROGRESS_BITS) * remaining >> PROGRESS_BITS);
}

static int32_t interpolate(int32_t from, int32_t to, int64_t progress) {
    return from + static_cast<int32_t>(static_cast<int64_t>(to - from) * progress >> PROGRESS_BITS);
}

/* compose.comp's `scale_position` split into two source pixels and a weight as its `scale` splits it: where pixel
 * `offset` of `length` output pixels samples a span of `source_length` pixels */
static axis_sample sample_axis(int32_t offset, int32_t length, int32_t source_length) {
    uint32_t step = (static_cast<uint32_t>(source_length) << STEP_BITS) / static_cast<uint32_t>(length);
    uint32_t center = (static_cast<uint32_t>(offset) * step + (step >> 1)) >> (STEP_BITS - POSITION_BITS);
    int32_t position = std::clamp(static_cast<int32_t>(center) - POSITION_ONE / 2, 0,
                                  (source_length - 1) * POSITION_ONE);

    axis_sample sample;
    sample.first = position >> POSITION_BITS;
    sample.second = std::min(sample.first + 1, source_length - 1);
    sample.weight = static_cast<uint32_t>(position & (POSITION_ONE - 1));

    return sample;
}

static void add_tap(const layer_scaled& piece, int32_t x, int32_t y, uint32_t weight, sample_sums& sums) {
    const uint8_t* row = piece.source.pixels + static_cast<size_t>(y) * piece.source.pitch;
    uint32_t pixel = reinterpret_cast<const uint32_t*>(row)[x];
    uint32_t weighted = piece.is_content ? weight : weight * (pixel >> decor::PIXEL_ALPHA_SHIFT);

    sums.alpha += weighted;
    for (uint32_t i = 0; i < CHANNELS; i++) {
        sums.channels[i] += weighted * ((pixel >> CHANNEL_SHIFTS[i]) & decor::CHANNEL_MAX);
    }
}

static bool is_in_bottom_corner(const damage_list::rect& r, int32_t radius, int32_t x, int32_t y) {
    bool is_bottom_row = y >= r.y + r.h - radius;
    return is_bottom_row && (x < r.x + radius || x >= r.x + r.w - radius);
}

/* compose.comp's `scale` for the output pixel at (x, y) over `below` */
static uint32_t scale_pixel(
    uint32_t below,
    const layer_scaled& piece,
    const axis_sample& column,
    const axis_sample& row,
    int32_t x,
    int32_t y
) {
    uint32_t column_rest = POSITION_ONE - column.weight;
    uint32_t row_rest = POSITION_ONE - row.weight;

    sample_sums sums;
    add_tap(piece, column.first, row.first, column_rest * row_rest, sums);
    add_tap(piece, column.second, row.first, column.weight * row_rest, sums);
    add_tap(piece, column.first, row.second, column_rest * row.weight, sums);
    add_tap(piece, column.second, row.second, column.weight * row.weight, sums);
    if (sums.alpha == 0) {
        return below;
    }

    uint32_t sample = 0;
    uint32_t alpha = decor::CHANNEL_MAX;
    for (uint32_t i = 0; i < CHANNELS; i++) {
        uint32_t value = piece.is_content ? (sums.channels[i] + WEIGHT_HALF) >> WEIGHT_BITS
                                          : (sums.channels[i] + (sums.alpha >> 1)) / sums.alpha;
        sample |= value << CHANNEL_SHIFTS[i];
    }

    if (!piece.is_content) {
        alpha = (sums.alpha + WEIGHT_HALF) >> WEIGHT_BITS;
    }

    alpha = stlxgfx_div255(alpha * piece.opacity);
    if (piece.radius != 0 && is_in_bottom_corner(piece.bounds, static_cast<int32_t>(piece.radius), x, y)) {
        alpha = stlxgfx_div255(alpha * decor::rounded_coverage(piece.bounds, static_cast<int32_t>(piece.radius), x, y));
    }

    return decor::blend_over(below, sample, alpha);
}

/* Rounds down, so the shadow before the frame scales as evenly as what follows */
static int64_t divide_down(int64_t dividend, int64_t divisor) {
    int64_t quotient = dividend / divisor;
    bool rounded_up = dividend % divisor != 0 && (dividend < 0) != (divisor < 0);

    return rounded_up ? quotient - 1 : quotient;
}

static int32_t map_coordinate(const axis_map& axis, int32_t at) {
    int64_t frame_length = axis.frame_end - axis.frame_first;
    int64_t rect_length = axis.rect_end - axis.rect_first;
    if (!axis.keeps_ends) {
        return axis.rect_first + static_cast<int32_t>(divide_down((at - axis.frame_first) * rect_length, frame_length));
    }

    if (at <= axis.body_first) {
        return axis.rect_first + (at - axis.frame_first);
    }

    if (at >= axis.body_end) {
        return axis.rect_end - (axis.frame_end - at);
    }

    int64_t body_length = axis.body_end - axis.body_first;
    int64_t rect_body_length = rect_length - (axis.body_first - axis.frame_first) - (axis.frame_end - axis.body_end);
    int32_t into_body = static_cast<int32_t>((at - axis.body_first) * rect_body_length / body_length);

    return axis.rect_first + (axis.body_first - axis.frame_first) + into_body;
}

static damage_list::rect map_rect(const axis_map& columns, const axis_map& rows, const damage_list::rect& r) {
    int32_t x = map_coordinate(columns, r.x);
    int32_t y = map_coordinate(rows, r.y);

    return { x, y, map_coordinate(columns, r.x + r.w) - x, map_coordinate(rows, r.y + r.h) - y };
}

static picture_region slice_region(const picture_region& region, bool is_rows, int32_t first, int32_t length) {
    picture_region slice = region;
    size_t offset = static_cast<size_t>(first) * (is_rows ? region.pitch : BYTES_PER_PIXEL);
    slice.pixels = region.pixels ? region.pixels + offset : nullptr;
    slice.address = region.address != 0 ? region.address + offset : 0;
    if (is_rows) {
        slice.height = length;
    } else {
        slice.width = length;
    }

    return slice;
}

static damage_list::rect slice_rect(const damage_list::rect& r, bool is_rows, int32_t first, int32_t length) {
    if (is_rows) {
        return { r.x, r.y + first, r.w, length };
    }

    return { r.x + first, r.y, length, r.h };
}

static void append_piece(
    std::vector<layer_scaled>& pieces,
    const picture_region& source,
    const damage_list::rect& bounds,
    uint32_t opacity
) {
    if (bounds.w <= 0 || bounds.h <= 0 || source.width <= 0 || source.height <= 0) {
        return;
    }

    layer_scaled piece;
    piece.bounds = bounds;
    piece.source = source;
    piece.opacity = opacity;
    pieces.push_back(piece);
}

/* Shows `strip` at `shown`, which differs from it in length along one axis at most, with the strip's halves at their
 * size along it and its middle line stretched between them, or the halves cut short where `shown` is shorter */
static void append_kept_ends(
    std::vector<layer_scaled>& pieces,
    const picture_region& strip,
    const damage_list::rect& shown,
    uint32_t opacity
) {
    bool is_rows = shown.h != strip.height;
    int32_t length = is_rows ? strip.height : strip.width;
    int32_t shown_length = is_rows ? shown.h : shown.w;
    if (shown_length == length) {
        append_piece(pieces, strip, shown, opacity);
        return;
    }

    bool is_longer = shown_length > length;
    int32_t head = (is_longer ? length : shown_length) / 2;
    int32_t tail = is_longer ? length - head - 1 : shown_length - head;
    int32_t middle = shown_length - head - tail;

    append_piece(pieces, slice_region(strip, is_rows, 0, head), slice_rect(shown, is_rows, 0, head), opacity);
    if (is_longer) {
        append_piece(pieces, slice_region(strip, is_rows, head, 1), slice_rect(shown, is_rows, head, middle), opacity);
    }

    append_piece(pieces, slice_region(strip, is_rows, length - tail, tail),
                 slice_rect(shown, is_rows, shown_length - tail, tail), opacity);
}

bool step_animation(window_animation& a, uint64_t now_ns) {
    uint64_t duration = a.transition == window_transition::resize ? RESIZE_NS : DOCK_NS;
    uint64_t elapsed = now_ns > a.start_ns ? now_ns - a.start_ns : 0;
    int64_t progress = ease_out(elapsed, duration);

    a.rect = {
        interpolate(a.from.x, a.to.x, progress),
        interpolate(a.from.y, a.to.y, progress),
        interpolate(a.from.w, a.to.w, progress),
        interpolate(a.from.h, a.to.h, progress),
    };

    a.opacity = static_cast<uint32_t>(interpolate(static_cast<int32_t>(a.from_opacity),
                                                  static_cast<int32_t>(a.to_opacity), progress));

    return elapsed >= duration;
}

damage_list::rect shown_frame(const dm_window& w) {
    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];
    return decor::frame_rect(w, w.x, w.y, static_cast<int32_t>(b.width), static_cast<int32_t>(b.height));
}

damage_list::rect painted_rect(const dm_window& w, const damage_list::rect& frame) {
    damage_list::rect at_rest = decor::frame_rect(w, 0, 0, 0, 0);
    damage_list::rect painted = decor::damage_rect(w, 0, 0, 0, 0);
    int32_t left = at_rest.x - painted.x;
    int32_t top = at_rest.y - painted.y;

    return { frame.x - left, frame.y - top, frame.w + painted.w - at_rest.w, frame.h + painted.h - at_rest.h };
}

/* A resize keeps the frame strips' thickness, the shadow's included, and their halves at their size, and a move to
 * or from the dock scales the whole window alike, its content's corners with it */
void animation_pieces(
    const window_animation& a,
    const dm_window& w,
    const frame_picture* frame,
    std::vector<layer_scaled>& pieces
) {
    const dm_buffer& b = w.buffers[static_cast<size_t>(w.current)];
    damage_list::rect content = { 0, 0, static_cast<int32_t>(b.width), static_cast<int32_t>(b.height) };
    damage_list::rect shown = decor::frame_rect(w, 0, 0, content.w, content.h);
    int32_t corner_y = content.h - decor::CONTENT_R;

    bool keeps_ends = a.transition == window_transition::resize;
    axis_map columns = { shown.x, shown.x + shown.w, 0, content.w, a.rect.x, a.rect.x + a.rect.w, keeps_ends };
    axis_map rows = { shown.y, shown.y + shown.h, 0, corner_y, a.rect.y, a.rect.y + a.rect.h, keeps_ends };

    for (uint32_t i = 0; frame && i < frame_picture::STRIPS; i++) {
        const damage_list::rect& strip = frame->strip_rect(i);
        if (strip.w <= 0 || strip.h <= 0) {
            continue;
        }

        damage_list::rect placed = map_rect(columns, rows, strip);
        if (keeps_ends) {
            append_kept_ends(pieces, frame->strip_pixels(i), placed, a.opacity);
        } else {
            append_piece(pieces, frame->strip_pixels(i), placed, a.opacity);
        }
    }

    picture_region pixels = { reinterpret_cast<const uint8_t*>(b.pixels), b.gpu_address, b.pitch,
                              content.w, content.h };
    damage_list::rect placed = map_rect(columns, rows, content);
    int32_t radius = keeps_ends ? decor::CONTENT_R : decor::CONTENT_R * placed.w / content.w;
    size_t first = pieces.size();
    append_piece(pieces, pixels, placed, a.opacity);
    if (pieces.size() > first) {
        pieces.back().is_content = true;
        pieces.back().radius = static_cast<uint32_t>(radius);
    }
}

void draw_scaled(const decor::canvas& target, const layer_scaled& piece, const damage_list::rect& clip) {
    damage_list::rect area = damage_list::intersect(piece.bounds, clip);
    if (area.w <= 0 || area.h <= 0 || !piece.source.pixels) {
        return;
    }

    std::vector<axis_sample> columns(static_cast<size_t>(area.w));
    for (int32_t i = 0; i < area.w; i++) {
        columns[static_cast<size_t>(i)] = sample_axis(area.x + i - piece.bounds.x, piece.bounds.w, piece.source.width);
    }

    for (int32_t y = area.y; y < area.y + area.h; y++) {
        axis_sample row = sample_axis(y - piece.bounds.y, piece.bounds.h, piece.source.height);
        uint8_t* line = target.surface->pixels + static_cast<size_t>(y - target.y) * target.surface->pitch;
        uint32_t* pixels = reinterpret_cast<uint32_t*>(line) + (area.x - target.x);
        for (int32_t i = 0; i < area.w; i++) {
            pixels[i] = scale_pixel(pixels[i], piece, columns[static_cast<size_t>(i)], row, area.x + i, y);
        }
    }
}

damage_list::rect fit_rect(const damage_list::rect& shape, const damage_list::rect& tile) {
    int64_t width = tile.w;
    int64_t height = tile.h;
    if (static_cast<int64_t>(shape.w) * tile.h > static_cast<int64_t>(shape.h) * tile.w) {
        height = static_cast<int64_t>(shape.h) * tile.w / shape.w;
    } else {
        width = static_cast<int64_t>(shape.w) * tile.h / shape.h;
    }

    int32_t fitted_w = static_cast<int32_t>(width);
    int32_t fitted_h = static_cast<int32_t>(height);

    return { tile.x + (tile.w - fitted_w) / 2, tile.y + (tile.h - fitted_h) / 2, fitted_w, fitted_h };
}
