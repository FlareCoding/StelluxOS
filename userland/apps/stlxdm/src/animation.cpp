#include "animation.hpp"
#include "easing.hpp"
#include "server.hpp"

#include <stlxgfx/blend.h>

#include <algorithm>

/* Maximize and restore ease over this, a move to or from the dock over this */
constexpr uint64_t RESIZE_NS = 180000000ull;
constexpr uint64_t DOCK_NS = 220000000ull;

/* As in compose.comp, a step counts 65536ths of a source pixel per output pixel, a position 256ths of a source
 * pixel, and an output pixel's four bilinear weights sum to 65536 */
constexpr uint32_t STEP_BITS = 16;
constexpr uint32_t POSITION_BITS = 8;
constexpr int32_t POSITION_ONE = 1 << POSITION_BITS;
constexpr uint32_t WEIGHT_BITS = 2 * POSITION_BITS;
constexpr uint32_t WEIGHT_HALF = 1u << (WEIGHT_BITS - 1);

constexpr uint32_t COLOR_MASK = 0x00FFFFFF;
constexpr uint32_t CHANNEL_SHIFTS[] = { 16, 8, 0 };
constexpr uint32_t CHANNELS = sizeof(CHANNEL_SHIFTS) / sizeof(CHANNEL_SHIFTS[0]);
constexpr uint32_t BYTES_PER_PIXEL = 4;

/* A row mix holds red and blue in the two 16 bit lanes of one word and alpha and green in those of another, as
 * `decor::LANE_CHANNELS` masks them, since a weighted sum of two pixels' channels never carries across */
constexpr uint32_t LANE_BITS = 16;
constexpr uint32_t LANE_MASK = 0xFFFF;

namespace {

/* An output pixel's two source pixels along one axis, and the second's weight in 256ths, below `POSITION_ONE` */
struct axis_sample {
    int32_t  first = 0;
    int32_t  second = 0;
    uint32_t weight = 0;
};

/* The bilinear sums of an output pixel's source pixels: alpha by weight, and each channel by alpha and weight, as
 * compose.comp's `add_tap` sums a frame piece's */
struct sample_sums {
    uint32_t alpha = 0;
    uint32_t channels[CHANNELS] = {};
};

/* One source row of a window's content mixed across the output columns by their bilinear weights, unrounded */
struct row_mix {
    int32_t               row = -1;
    std::vector<uint32_t> red_blue;
    std::vector<uint32_t> alpha_green;
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
    uint32_t weighted = weight * (pixel >> decor::PIXEL_ALPHA_SHIFT);

    sums.alpha += weighted;
    for (uint32_t i = 0; i < CHANNELS; i++) {
        sums.channels[i] += weighted * ((pixel >> CHANNEL_SHIFTS[i]) & decor::CHANNEL_MAX);
    }
}

static bool is_in_bottom_corner(const damage_list::rect& r, int32_t radius, int32_t x, int32_t y) {
    bool is_bottom_row = y >= r.y + r.h - radius;
    return is_bottom_row && (x < r.x + radius || x >= r.x + r.w - radius);
}

static uint32_t shown_alpha(const layer_scaled& piece, uint32_t alpha, int32_t x, int32_t y) {
    int32_t radius = static_cast<int32_t>(piece.radius);
    alpha = stlxgfx_div255(alpha * piece.opacity);
    if (radius != 0 && is_in_bottom_corner(piece.bounds, radius, x, y)) {
        alpha = stlxgfx_div255(alpha * decor::rounded_coverage(piece.bounds, radius, x, y));
    }

    return alpha;
}

/* The premultiplied `pixel` at `opacity`, its alpha and each channel faded alike */
static uint32_t fade_premultiplied(uint32_t pixel, uint32_t opacity) {
    uint32_t faded = stlxgfx_div255((pixel >> decor::PIXEL_ALPHA_SHIFT) * opacity) << decor::PIXEL_ALPHA_SHIFT;
    for (uint32_t shift : CHANNEL_SHIFTS) {
        faded |= stlxgfx_div255(((pixel >> shift) & decor::CHANNEL_MAX) * opacity) << shift;
    }

    return faded;
}

static uint32_t blend_faded(uint32_t below, uint32_t source, uint32_t opacity) {
    uint32_t faded = opacity == decor::CHANNEL_MAX ? source : fade_premultiplied(source, opacity);
    return decor::blend_premultiplied(below, faded);
}

/* compose.comp's `scale_premultiplied` of the resampled premultiplied `source` at (x, y) over `below` */
static uint32_t blend_premultiplied_content(
    uint32_t below,
    const layer_scaled& piece,
    uint32_t source,
    int32_t x,
    int32_t y
) {
    uint32_t blended = blend_faded(below, source, piece.opacity);
    int32_t radius = static_cast<int32_t>(piece.radius);
    if (radius != 0 && is_in_bottom_corner(piece.bounds, radius, x, y)) {
        return decor::blend_over(below, blended, decor::rounded_coverage(piece.bounds, radius, x, y));
    }

    return blended;
}

/* compose.comp's `scale` of a frame piece for the output pixel at (x, y) over `below` */
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
    for (uint32_t i = 0; i < CHANNELS; i++) {
        uint32_t value = (sums.channels[i] + (sums.alpha >> 1)) / sums.alpha;
        sample |= value << CHANNEL_SHIFTS[i];
    }

    uint32_t alpha = (sums.alpha + WEIGHT_HALF) >> WEIGHT_BITS;

    return decor::blend_over(below, sample, shown_alpha(piece, alpha, x, y));
}

/* `scale_pixel` where both weights are 0, so the first tap alone counts and its weighted mean is itself */
static uint32_t blend_tap(
    uint32_t below,
    const layer_scaled& piece,
    const axis_sample& column,
    const axis_sample& row,
    int32_t x,
    int32_t y
) {
    const uint8_t* line = piece.source.pixels + static_cast<size_t>(row.first) * piece.source.pitch;
    uint32_t pixel = reinterpret_cast<const uint32_t*>(line)[column.first];
    uint32_t alpha = pixel >> decor::PIXEL_ALPHA_SHIFT;
    if (alpha == 0) {
        return below;
    }

    return decor::blend_over(below, pixel & COLOR_MASK, shown_alpha(piece, alpha, x, y));
}

static void mix_row(const layer_scaled& piece, const std::vector<axis_sample>& columns, int32_t row, row_mix& mix) {
    const uint8_t* line = piece.source.pixels + static_cast<size_t>(row) * piece.source.pitch;
    const uint32_t* source = reinterpret_cast<const uint32_t*>(line);
    mix.row = row;
    mix.red_blue.resize(columns.size());
    mix.alpha_green.resize(columns.size());

    for (size_t i = 0; i < columns.size(); i++) {
        const axis_sample& column = columns[i];
        uint32_t first = source[column.first];
        uint32_t second = source[column.second];
        uint32_t rest = POSITION_ONE - column.weight;
        uint32_t first_alpha_green = (first >> decor::CHANNEL_BITS) & decor::LANE_CHANNELS;
        uint32_t second_alpha_green = (second >> decor::CHANNEL_BITS) & decor::LANE_CHANNELS;

        mix.red_blue[i] = (first & decor::LANE_CHANNELS) * rest + (second & decor::LANE_CHANNELS) * column.weight;
        mix.alpha_green[i] = first_alpha_green * rest + second_alpha_green * column.weight;
    }
}

/* Keeps `top` and `bottom` holding the mixes of `row`'s two source rows, reusing those an earlier output row mixed */
static void mix_rows(
    const layer_scaled& piece,
    const std::vector<axis_sample>& columns,
    const axis_sample& row,
    row_mix& top,
    row_mix& bottom
) {
    if (top.row != row.first && bottom.row == row.first) {
        std::swap(top, bottom);
    }

    if (top.row != row.first) {
        mix_row(piece, columns, row.first, top);
    }

    if (bottom.row != row.second) {
        mix_row(piece, columns, row.second, bottom);
    }
}

static uint32_t mix_vertically(uint32_t top, uint32_t bottom, const axis_sample& row) {
    return ((top * (POSITION_ONE - row.weight) + bottom * row.weight) + WEIGHT_HALF) >> WEIGHT_BITS;
}

static uint32_t mix_color(const row_mix& top, const row_mix& bottom, size_t i, const axis_sample& row) {
    uint32_t upper = top.red_blue[i];
    uint32_t lower = bottom.red_blue[i];
    uint32_t red = mix_vertically(upper >> LANE_BITS, lower >> LANE_BITS, row);
    uint32_t green = mix_vertically(top.alpha_green[i] & LANE_MASK, bottom.alpha_green[i] & LANE_MASK, row);
    uint32_t blue = mix_vertically(upper & LANE_MASK, lower & LANE_MASK, row);

    return red << CHANNEL_SHIFTS[0] | green << CHANNEL_SHIFTS[1] | blue << CHANNEL_SHIFTS[2];
}

static uint32_t mix_alpha(const row_mix& top, const row_mix& bottom, size_t i, const axis_sample& row) {
    return mix_vertically(top.alpha_green[i] >> LANE_BITS, bottom.alpha_green[i] >> LANE_BITS, row);
}

static void blend_opaque_row(
    uint32_t* pixels,
    const layer_scaled& piece,
    const row_mix& top,
    const row_mix& bottom,
    const axis_sample& row,
    int32_t x,
    int32_t y,
    bool is_corner_row
) {
    for (size_t i = 0; i < top.red_blue.size(); i++) {
        int32_t column_x = x + static_cast<int32_t>(i);
        uint32_t alpha = is_corner_row ? shown_alpha(piece, decor::CHANNEL_MAX, column_x, y) : piece.opacity;
        pixels[i] = decor::blend_over(pixels[i], mix_color(top, bottom, i, row), alpha);
    }
}

static void blend_premultiplied_row(
    uint32_t* pixels,
    const layer_scaled& piece,
    const row_mix& top,
    const row_mix& bottom,
    const axis_sample& row,
    int32_t x,
    int32_t y,
    bool is_corner_row
) {
    uint32_t opacity = piece.opacity;
    for (size_t i = 0; i < top.red_blue.size(); i++) {
        uint32_t source = mix_alpha(top, bottom, i, row) << decor::PIXEL_ALPHA_SHIFT | mix_color(top, bottom, i, row);
        int32_t column_x = x + static_cast<int32_t>(i);
        pixels[i] = is_corner_row ? blend_premultiplied_content(pixels[i], piece, source, column_x, y)
                                  : blend_faded(pixels[i], source, opacity);
    }
}

/* compose.comp's `scale` of a window's content over the rows of `area`, its four taps summed as a mix along each row
 * and one down the column, which sum to the same and round once */
static void draw_content(
    const decor::canvas& target,
    const layer_scaled& piece,
    const damage_list::rect& area,
    const std::vector<axis_sample>& columns
) {
    row_mix top;
    row_mix bottom;
    int32_t corner_y = piece.bounds.y + piece.bounds.h - static_cast<int32_t>(piece.radius);

    for (int32_t y = area.y; y < area.y + area.h; y++) {
        axis_sample row = sample_axis(y - piece.bounds.y, piece.bounds.h, piece.source.height);
        mix_rows(piece, columns, row, top, bottom);

        uint8_t* line = target.surface->pixels + static_cast<size_t>(y - target.y) * target.surface->pitch;
        uint32_t* pixels = reinterpret_cast<uint32_t*>(line) + (area.x - target.x);
        bool is_corner_row = piece.radius != 0 && y >= corner_y;
        if (piece.is_premultiplied) {
            blend_premultiplied_row(pixels, piece, top, bottom, row, area.x, y, is_corner_row);
        } else {
            blend_opaque_row(pixels, piece, top, bottom, row, area.x, y, is_corner_row);
        }
    }
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

material::surface frame_glass(const damage_list::rect& frame) {
    return { frame, decor::CORNER_R, material::WINDOW_RIM_FADE };
}

/* Glass draws at full strength, so only a resize at full opacity shows it beneath the window */
material::surface animation_glass(const window_animation& a, int32_t origin_x, int32_t origin_y) {
    bool is_resize = a.transition == window_transition::resize;
    bool is_full_opacity = a.opacity == decor::CHANNEL_MAX;
    if (!is_resize || !is_full_opacity || !decor::decorated(*a.window)) {
        return {};
    }

    damage_list::rect frame = { a.rect.x - origin_x, a.rect.y - origin_y, a.rect.w, a.rect.h };

    return frame_glass(frame);
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
    uint32_t width = frame ? frame->drawn_width() : b.width;
    uint32_t height = frame ? frame->drawn_height() : b.height;
    damage_list::rect content = { 0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height) };
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
                              static_cast<int32_t>(b.width), static_cast<int32_t>(b.height) };
    damage_list::rect placed = map_rect(columns, rows, content);
    int32_t radius = keeps_ends ? decor::CONTENT_R : decor::CONTENT_R * placed.w / content.w;
    size_t first = pieces.size();
    append_piece(pieces, pixels, placed, a.opacity);
    if (pieces.size() > first) {
        pieces.back().is_content = true;
        pieces.back().is_premultiplied = b.is_premultiplied;
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

    if (piece.is_content) {
        draw_content(target, piece, area, columns);
        return;
    }

    for (int32_t y = area.y; y < area.y + area.h; y++) {
        axis_sample row = sample_axis(y - piece.bounds.y, piece.bounds.h, piece.source.height);
        uint8_t* line = target.surface->pixels + static_cast<size_t>(y - target.y) * target.surface->pitch;
        uint32_t* pixels = reinterpret_cast<uint32_t*>(line) + (area.x - target.x);
        for (int32_t i = 0; i < area.w; i++) {
            const axis_sample& column = columns[static_cast<size_t>(i)];
            bool is_single_tap = column.weight == 0 && row.weight == 0;
            pixels[i] = is_single_tap ? blend_tap(pixels[i], piece, column, row, area.x + i, y)
                                      : scale_pixel(pixels[i], piece, column, row, area.x + i, y);
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
