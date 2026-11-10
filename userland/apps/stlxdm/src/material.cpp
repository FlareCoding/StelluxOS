#include "material.hpp"
#include "decor.hpp"

#include <stlxgfx/blend.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cmath>
#include <vector>

/* Glass levels as fractions of fully clear glass */
constexpr float LEVEL_SOLID = 0.0f;
constexpr float LEVEL_SUBTLE = 0.33f;
constexpr float LEVEL_GLASS = 0.66f;
constexpr float LEVEL_CLEAR = 1.0f;

/* Opacities per unit of glass level. The tint loses TINT_FADE of its opacity, the veil runs from VEIL_TOP at the top
 * edge to VEIL_BOTTOM at the bottom, the rim light reaches RIM_LIGHT, and the grain fades out from GRAIN at solid. */
constexpr float TINT_FADE = 0.62f;
constexpr float VEIL_TOP = 0.50f;
constexpr float VEIL_BOTTOM = 0.15f;
constexpr float RIM_LIGHT = 0.30f;
constexpr float GRAIN = 0.12f;

/* The frost is blurred at a quarter of the output's resolution by three box blurs, which together come close to a
 * Gaussian of sigma 24 output pixels */
constexpr int32_t FROST_SCALE = 4;
constexpr int32_t FROST_BOXES[] = { 11, 11, 13 };

/* Growing the frost back weighs the two nearest frost pixels along each axis by shares of TAP_WHOLE, on red, green
 * and blue at once in the 16 bit lanes of one word */
constexpr int32_t TAP_WHOLE = 2 * FROST_SCALE;
constexpr int32_t TAP_SHIFT = 6;
constexpr uint32_t LANE_BITS = 16;
constexpr uint64_t LANE_MASK = 0xFFFF;
constexpr uint64_t TAP_ROUNDING = (1ull << (TAP_SHIFT - 1)) * ((1ull << (2 * LANE_BITS)) + (1ull << LANE_BITS) + 1);

/* Until the knee, a frost channel is FROST_ONE times its block's average, which for a whole block is the block's sum */
constexpr int32_t FROST_ONE = FROST_SCALE * FROST_SCALE;
constexpr int32_t FROST_MAX = 255 * FROST_ONE;

/* The frost's saturation beneath each tint, and the luma weights of SVG's saturate matrix that applies it */
constexpr float WARM_SATURATION = 0.6f;
constexpr float NEUTRAL_SATURATION = 1.3f;
constexpr float LUMA_WEIGHTS[] = { 0.213f, 0.715f, 0.072f };
constexpr int32_t SATURATION_SHIFT = 12;

/* The knee caps the frost's brightness near a quarter, so text on any glass keeps its contrast */
constexpr float KNEE[] = { 0.0f, 0.18f, 0.23f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
constexpr int32_t KNEE_SPANS = sizeof(KNEE) / sizeof(KNEE[0]) - 1;

/* The grain is SVG's fractal noise stitched into a tile, whose red channel r gives each speck the opacity
 * GRAIN_GAIN r + GRAIN_OFFSET */
constexpr double GRAIN_FREQUENCY = 0.9;
constexpr int32_t GRAIN_OCTAVES = 2;
constexpr int64_t GRAIN_SEED = 7;
constexpr double GRAIN_GAIN = 2.2;
constexpr double GRAIN_OFFSET = -1.14;

/* SVG's turbulence lattice and the random numbers that fill it */
constexpr int32_t LATTICE_SIZE = 0x100;
constexpr int32_t LATTICE_MASK = 0xFF;
constexpr int32_t NOISE_OFFSET = 0x1000;
constexpr int32_t NOISE_CHANNELS = 4;
constexpr int64_t RANDOM_MODULUS = 2147483647;
constexpr int64_t RANDOM_MULTIPLIER = 16807;
constexpr int64_t RANDOM_QUOTIENT = 127773;
constexpr int64_t RANDOM_REMAINDER = 2836;

/* Fraction bits of a veil entry's scale and offsets */
constexpr int32_t VEIL_SHIFT = 16;
constexpr uint64_t VEIL_ONE = 1ull << VEIL_SHIFT;

/* Distances from a surface's edge run in eighth pixels, as decor measures them */
constexpr int32_t EIGHTHS_PER_PIXEL = 8;

/* Where the turbulence wraps so the tile's opposite edges meet */
struct noise_stitch {
    int32_t width;
    int32_t height;
    int32_t wrap_x;
    int32_t wrap_y;
};

/* The frost at 1 / FROST_SCALE of its output's resolution, as three planes of channel values, red first */
struct frost_planes {
    int32_t              width = 0;
    int32_t              height = 0;
    std::vector<int32_t> values;
};

/* The two frost pixels nearest an output pixel's center along one axis, and the second one's weight out of TAP_WHOLE */
struct frost_tap {
    int32_t  first;
    int32_t  second;
    uint64_t weight;
};

/* The color a glass tint lays over the frost, and the saturation the frost takes beneath it */
struct tint_look {
    uint32_t color;
    float    saturation;
};

static material::tables g_tables;
static uint8_t g_grain_noise[material::GRAIN_TILE * material::GRAIN_TILE];
static bool g_has_grain_noise = false;

static int32_t g_lattice[LATTICE_SIZE];
static double g_gradients[LATTICE_SIZE][2];

static int64_t next_random(int64_t seed) {
    int64_t result = RANDOM_MULTIPLIER * (seed % RANDOM_QUOTIENT) - RANDOM_REMAINDER * (seed / RANDOM_QUOTIENT);
    if (result <= 0) {
        result += RANDOM_MODULUS;
    }

    return result;
}

/* SVG's lattice for the red channel, drawing every channel's numbers so the shuffle sees the same sequence */
static void fill_lattice() {
    int64_t seed = GRAIN_SEED;
    for (int32_t channel = 0; channel < NOISE_CHANNELS; channel++) {
        for (int32_t i = 0; i < LATTICE_SIZE; i++) {
            g_lattice[i] = i;
            double gradient[2];
            for (double& axis : gradient) {
                seed = next_random(seed);
                axis = static_cast<double>(seed % (2 * LATTICE_SIZE) - LATTICE_SIZE) / LATTICE_SIZE;
            }

            double length = std::sqrt(gradient[0] * gradient[0] + gradient[1] * gradient[1]);
            if (channel == 0 && length > 0.0) {
                g_gradients[i][0] = gradient[0] / length;
                g_gradients[i][1] = gradient[1] / length;
            }
        }
    }

    for (int32_t i = LATTICE_SIZE - 1; i > 0; i--) {
        seed = next_random(seed);
        int32_t j = static_cast<int32_t>(seed % LATTICE_SIZE);
        int32_t held = g_lattice[i];
        g_lattice[i] = g_lattice[j];
        g_lattice[j] = held;
    }
}

static int32_t stitched(int32_t lattice_point, int32_t wrap, int32_t width) {
    return lattice_point >= wrap ? lattice_point - width : lattice_point;
}

static double smooth_step(double t) {
    return t * t * (3.0 - 2.0 * t);
}

static double gradient_dot(int32_t index, double x, double y) {
    const double* gradient = g_gradients[g_lattice[index & LATTICE_MASK]];
    return x * gradient[0] + y * gradient[1];
}

static double noise_at(double x, double y, const noise_stitch& stitch) {
    double shifted_x = x + NOISE_OFFSET;
    double shifted_y = y + NOISE_OFFSET;
    int32_t x0 = static_cast<int32_t>(shifted_x);
    int32_t y0 = static_cast<int32_t>(shifted_y);
    double fraction_x = shifted_x - x0;
    double fraction_y = shifted_y - y0;

    int32_t x1 = stitched(x0 + 1, stitch.wrap_x, stitch.width) & LATTICE_MASK;
    int32_t y1 = stitched(y0 + 1, stitch.wrap_y, stitch.height) & LATTICE_MASK;
    x0 = stitched(x0, stitch.wrap_x, stitch.width) & LATTICE_MASK;
    y0 = stitched(y0, stitch.wrap_y, stitch.height) & LATTICE_MASK;

    int32_t column0 = g_lattice[x0];
    int32_t column1 = g_lattice[x1];
    double blend_x = smooth_step(fraction_x);
    double blend_y = smooth_step(fraction_y);

    double top0 = gradient_dot(column0 + y0, fraction_x, fraction_y);
    double top1 = gradient_dot(column1 + y0, fraction_x - 1.0, fraction_y);
    double bottom0 = gradient_dot(column0 + y1, fraction_x, fraction_y - 1.0);
    double bottom1 = gradient_dot(column1 + y1, fraction_x - 1.0, fraction_y - 1.0);
    double top = top0 + blend_x * (top1 - top0);
    double bottom = bottom0 + blend_x * (bottom1 - bottom0);

    return top + blend_y * (bottom - top);
}

/* The fractal noise of the tile's pixel (x, y), from 0 to 1 */
static double fractal_noise(int32_t x, int32_t y) {
    int32_t periods = static_cast<int32_t>(std::lround(material::GRAIN_TILE * GRAIN_FREQUENCY));
    noise_stitch stitch = { periods, periods, NOISE_OFFSET + periods, NOISE_OFFSET + periods };
    double frequency = static_cast<double>(periods) / material::GRAIN_TILE;
    double sample_x = x * frequency;
    double sample_y = y * frequency;

    double sum = 0.0;
    double weight = 1.0;
    for (int32_t octave = 0; octave < GRAIN_OCTAVES; octave++) {
        sum += noise_at(sample_x, sample_y, stitch) * weight;
        sample_x *= 2.0;
        sample_y *= 2.0;
        weight /= 2.0;

        stitch.width *= 2;
        stitch.height *= 2;
        stitch.wrap_x = 2 * stitch.wrap_x - NOISE_OFFSET;
        stitch.wrap_y = 2 * stitch.wrap_y - NOISE_OFFSET;
    }

    return std::fmin(std::fmax((sum + 1.0) / 2.0, 0.0), 1.0);
}

static void fill_grain_noise() {
    fill_lattice();
    for (int32_t y = 0; y < material::GRAIN_TILE; y++) {
        for (int32_t x = 0; x < material::GRAIN_TILE; x++) {
            double red = std::round(fractal_noise(x, y) * 255.0) / 255.0;
            double opacity = std::fmin(std::fmax(GRAIN_GAIN * red + GRAIN_OFFSET, 0.0), 1.0);
            g_grain_noise[y * material::GRAIN_TILE + x] = static_cast<uint8_t>(std::lround(opacity * 255.0));
        }
    }

    g_has_grain_noise = true;
}

static float level_fraction(stlxconf_glass_level_t level) {
    switch (level) {
    case STLXCONF_GLASS_LEVEL_SOLID:
        return LEVEL_SOLID;
    case STLXCONF_GLASS_LEVEL_SUBTLE:
        return LEVEL_SUBTLE;
    case STLXCONF_GLASS_LEVEL_GLASS:
        return LEVEL_GLASS;
    case STLXCONF_GLASS_LEVEL_CLEAR:
        return LEVEL_CLEAR;
    }

    return LEVEL_GLASS;
}

static tint_look look_of(stlxconf_glass_tint_t tint) {
    switch (tint) {
    case STLXCONF_GLASS_TINT_WARM:
        return { STLXTHEME_GLASS_TINT_WARM, WARM_SATURATION };
    case STLXCONF_GLASS_TINT_NEUTRAL:
        return { STLXTHEME_GLASS_TINT_NEUTRAL, NEUTRAL_SATURATION };
    }

    return { STLXTHEME_GLASS_TINT_WARM, WARM_SATURATION };
}

static uint32_t opacity_byte(float opacity) {
    return static_cast<uint32_t>(std::lround(opacity * 255.0f));
}

static uint32_t channel_of(uint32_t color, uint32_t shift) {
    return (color >> shift) & 0xFF;
}

/* The entry that lays a veil of opacity `veil` over a frost channel, then `tint` at `tint_alpha` over both */
static void fill_veil_entry(uint32_t (&entry)[4], uint32_t veil, uint32_t tint, uint32_t tint_alpha) {
    uint64_t shown = 255 - tint_alpha;
    uint64_t whole = 255 * 255;
    entry[0] = static_cast<uint32_t>(((255 - veil) * shown * VEIL_ONE + whole / 2) / whole);

    for (uint32_t i = 0; i < 3; i++) {
        uint32_t shift = 8 * i;
        uint64_t veil_part = channel_of(STLXTHEME_GLASS_VEIL, shift) * veil * shown;
        uint64_t tint_part = channel_of(tint, shift) * tint_alpha * 255;
        entry[1 + i] = static_cast<uint32_t>(((veil_part + tint_part) * VEIL_ONE + whole / 2) / whole + VEIL_ONE / 2);
    }
}

static const uint32_t* row_of(const stlxgfx_surface_t* surface, int32_t y) {
    return reinterpret_cast<const uint32_t*>(surface->pixels + static_cast<size_t>(y) * surface->pitch);
}

static uint32_t* row_of(stlxgfx_surface_t* surface, int32_t y) {
    return reinterpret_cast<uint32_t*>(surface->pixels + static_cast<size_t>(y) * surface->pitch);
}

static int32_t clamp_index(int32_t index, int32_t count) {
    return std::min(std::max(index, 0), count - 1);
}

static int32_t* plane_row(frost_planes& planes, int32_t plane, int32_t y) {
    return &planes.values[(static_cast<size_t>(plane) * planes.height + y) * planes.width];
}

/* Averages the wallpaper's blocks of FROST_SCALE pixels a side into the three planes, red first, in FROST_ONE units */
static void shrink_wallpaper(const stlxgfx_surface_t* wallpaper, frost_planes& planes) {
    int32_t width = static_cast<int32_t>(wallpaper->width);
    int32_t height = static_cast<int32_t>(wallpaper->height);
    planes.values.assign(3 * static_cast<size_t>(planes.width) * planes.height, 0);

    for (int32_t y = 0; y < height; y++) {
        const uint32_t* row = row_of(wallpaper, y);
        int32_t* red = plane_row(planes, 0, y / FROST_SCALE);
        int32_t* green = plane_row(planes, 1, y / FROST_SCALE);
        int32_t* blue = plane_row(planes, 2, y / FROST_SCALE);
        for (int32_t x = 0; x < width; x++) {
            red[x / FROST_SCALE] += static_cast<int32_t>(channel_of(row[x], 16));
            green[x / FROST_SCALE] += static_cast<int32_t>(channel_of(row[x], 8));
            blue[x / FROST_SCALE] += static_cast<int32_t>(channel_of(row[x], 0));
        }
    }

    for (int32_t y = 0; y < planes.height; y++) {
        int32_t rows = std::min(FROST_SCALE, height - y * FROST_SCALE);
        for (int32_t x = 0; x < planes.width; x++) {
            int32_t pixels = rows * std::min(FROST_SCALE, width - x * FROST_SCALE);
            for (int32_t plane = 0; plane < 3; plane++) {
                int32_t& sum = plane_row(planes, plane, y)[x];
                sum = (sum * FROST_ONE + pixels / 2) / pixels;
            }
        }
    }
}

/* Averages `count` values `stride` apart over a centered box of `width`, the end values repeating past the ends */
static void box_blur(int32_t* values, int32_t count, int32_t stride, int32_t width, std::vector<int32_t>& line) {
    line.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; i++) {
        line[i] = values[i * stride];
    }

    int32_t reach = width / 2;
    int32_t sum = 0;
    for (int32_t i = -reach; i <= reach; i++) {
        sum += line[clamp_index(i, count)];
    }

    for (int32_t i = 0; i < count; i++) {
        values[i * stride] = (sum + reach) / width;
        sum += line[clamp_index(i + reach + 1, count)] - line[clamp_index(i - reach, count)];
    }
}

static void blur_planes(frost_planes& planes) {
    std::vector<int32_t> line;
    for (int32_t plane = 0; plane < 3; plane++) {
        for (int32_t width : FROST_BOXES) {
            for (int32_t y = 0; y < planes.height; y++) {
                box_blur(plane_row(planes, plane, y), planes.width, 1, width, line);
            }

            for (int32_t x = 0; x < planes.width; x++) {
                box_blur(plane_row(planes, plane, 0) + x, planes.height, planes.width, width, line);
            }
        }
    }
}

/* SVG's table transfer through KNEE, from FROST_ONE units to a channel byte */
static std::vector<uint8_t> knee_table() {
    std::vector<uint8_t> table(FROST_MAX + 1);
    for (int32_t value = 0; value <= FROST_MAX; value++) {
        float position = static_cast<float>(value) * KNEE_SPANS / FROST_MAX;
        int32_t span = std::min(static_cast<int32_t>(position), KNEE_SPANS - 1);
        float level = KNEE[span] + (position - span) * (KNEE[span + 1] - KNEE[span]);
        table[value] = static_cast<uint8_t>(std::lround(level * 255.0f));
    }

    return table;
}

static int32_t saturate_and_knee_channel(const int32_t (&weights)[3], const int32_t (&color)[3],
                                         const std::vector<uint8_t>& knee) {
    int32_t mixed = weights[0] * color[0] + weights[1] * color[1] + weights[2] * color[2];
    int32_t value = (mixed + (1 << (SATURATION_SHIFT - 1))) >> SATURATION_SHIFT;

    return knee[std::min(std::max(value, 0), FROST_MAX)];
}

/* Saturates each pixel of the planes by SVG's saturate matrix, then puts it through the knee */
static void saturate_and_knee(frost_planes& planes, float saturation) {
    int32_t matrix[3][3];
    for (int32_t out = 0; out < 3; out++) {
        for (int32_t in = 0; in < 3; in++) {
            float weight = LUMA_WEIGHTS[in] * (1.0f - saturation) + (in == out ? saturation : 0.0f);
            matrix[out][in] = static_cast<int32_t>(std::lround(weight * (1 << SATURATION_SHIFT)));
        }
    }

    std::vector<uint8_t> knee = knee_table();
    size_t count = static_cast<size_t>(planes.width) * planes.height;
    int32_t* red = plane_row(planes, 0, 0);
    int32_t* green = plane_row(planes, 1, 0);
    int32_t* blue = plane_row(planes, 2, 0);
    for (size_t i = 0; i < count; i++) {
        int32_t color[3] = { red[i], green[i], blue[i] };
        red[i] = saturate_and_knee_channel(matrix[0], color, knee);
        green[i] = saturate_and_knee_channel(matrix[1], color, knee);
        blue[i] = saturate_and_knee_channel(matrix[2], color, knee);
    }
}

static frost_tap tap_of(int32_t output_pixel, int32_t frost_pixels) {
    int32_t center = 2 * output_pixel + 1 - FROST_SCALE;
    int32_t first = center >= 0 ? center / TAP_WHOLE : -((TAP_WHOLE - 1 - center) / TAP_WHOLE);
    uint64_t weight = static_cast<uint64_t>(center - first * TAP_WHOLE);

    return { clamp_index(first, frost_pixels), clamp_index(first + 1, frost_pixels), weight };
}

static void widen_row(const std::vector<uint64_t>& packed, int32_t frost_width, int32_t row,
                      const std::vector<frost_tap>& columns, std::vector<uint64_t>& widened) {
    const uint64_t* source = &packed[static_cast<size_t>(row) * frost_width];
    for (size_t x = 0; x < columns.size(); x++) {
        const frost_tap& tap = columns[x];
        widened[x] = source[tap.first] * (TAP_WHOLE - tap.weight) + source[tap.second] * tap.weight;
    }
}

/* Grows the planes back to the frost's size, weighing each output pixel from the four frost pixels around it */
static void grow_frost(frost_planes& planes, stlxgfx_surface_t* frost) {
    int32_t width = static_cast<int32_t>(frost->width);
    int32_t height = static_cast<int32_t>(frost->height);
    size_t count = static_cast<size_t>(planes.width) * planes.height;

    const int32_t* reds = plane_row(planes, 0, 0);
    const int32_t* greens = plane_row(planes, 1, 0);
    const int32_t* blues = plane_row(planes, 2, 0);
    std::vector<uint64_t> packed(count);
    for (size_t i = 0; i < count; i++) {
        uint64_t red = static_cast<uint64_t>(reds[i]);
        uint64_t green = static_cast<uint64_t>(greens[i]);
        packed[i] = (red << (2 * LANE_BITS)) | (green << LANE_BITS) | static_cast<uint64_t>(blues[i]);
    }

    std::vector<frost_tap> columns(static_cast<size_t>(width));
    for (int32_t x = 0; x < width; x++) {
        columns[x] = tap_of(x, planes.width);
    }

    /* Output rows use the frost rows in increasing order, so each widened frost row is kept for every output row
     * that weighs it */
    std::vector<uint64_t> upper(static_cast<size_t>(width));
    std::vector<uint64_t> lower(static_cast<size_t>(width));
    int32_t upper_row = -1;
    int32_t lower_row = -1;
    for (int32_t y = 0; y < height; y++) {
        frost_tap rows = tap_of(y, planes.height);
        if (rows.first == lower_row && rows.first != upper_row) {
            std::swap(upper, lower);
            std::swap(upper_row, lower_row);
        }

        if (rows.first != upper_row) {
            widen_row(packed, planes.width, rows.first, columns, upper);
            upper_row = rows.first;
        }

        if (rows.second != lower_row) {
            widen_row(packed, planes.width, rows.second, columns, lower);
            lower_row = rows.second;
        }

        uint32_t* out = row_of(frost, y);
        for (int32_t x = 0; x < width; x++) {
            uint64_t mixed = upper[x] * (TAP_WHOLE - rows.weight) + lower[x] * rows.weight + TAP_ROUNDING;
            uint64_t red = ((mixed >> (2 * LANE_BITS)) & LANE_MASK) >> TAP_SHIFT;
            uint64_t green = ((mixed >> LANE_BITS) & LANE_MASK) >> TAP_SHIFT;
            uint64_t blue = (mixed & LANE_MASK) >> TAP_SHIFT;
            out[x] = static_cast<uint32_t>(0xFF000000u | (red << 16) | (green << 8) | blue);
        }
    }
}

static uint32_t veil_alpha(int32_t row, int32_t height) {
    uint32_t below = static_cast<uint32_t>(2 * row + 1);
    uint32_t span = static_cast<uint32_t>(2 * height);

    return (g_tables.veil_top * (span - below) + g_tables.veil_bottom * below + span / 2) / span;
}

static uint32_t veiled(uint32_t frost, const uint32_t (&veil)[4]) {
    uint32_t blue = (channel_of(frost, 0) * veil[0] + veil[1]) >> VEIL_SHIFT;
    uint32_t green = (channel_of(frost, 8) * veil[0] + veil[2]) >> VEIL_SHIFT;
    uint32_t red = (channel_of(frost, 16) * veil[0] + veil[3]) >> VEIL_SHIFT;

    return (red << 16) | (green << 8) | blue;
}

/* The rim light's opacity on `row`, full on the top row and fading out over `fade` rows */
static uint32_t rim_alpha(int32_t row, int32_t fade) {
    if (row >= fade) {
        return 0;
    }

    if (row == 0) {
        return g_tables.rim_alpha;
    }

    uint32_t span = static_cast<uint32_t>(2 * fade - 2);
    uint32_t left = static_cast<uint32_t>(2 * fade - 2 * row - 1);

    return (g_tables.rim_alpha * left + span / 2) / span;
}

/* `lit` shown at `cover` over the target's pixel `below` */
static uint32_t covered(uint32_t below, uint32_t lit, uint32_t cover) {
    if (cover == 0) {
        return below;
    }

    return decor::blend_over(below, lit, cover);
}

namespace material {

void configure(stlxconf_glass_level_t level, stlxconf_glass_tint_t tint) {
    if (!g_has_grain_noise) {
        fill_grain_noise();
    }

    float glass = level_fraction(level);
    tint_look look = look_of(tint);
    uint32_t tint_alpha = opacity_byte(1.0f - TINT_FADE * glass);
    uint32_t grain_alpha = opacity_byte(GRAIN * (1.0f - glass));

    g_tables.rim_alpha = opacity_byte(RIM_LIGHT * glass);
    g_tables.light = STLXTHEME_GLASS_LIGHT;
    g_tables.veil_top = opacity_byte(VEIL_TOP * glass);
    g_tables.veil_bottom = opacity_byte(VEIL_BOTTOM * glass);

    for (uint32_t veil = 0; veil < VEIL_STEPS; veil++) {
        fill_veil_entry(g_tables.veil[veil], veil, look.color, tint_alpha);
    }

    uint8_t* grain = reinterpret_cast<uint8_t*>(g_tables.grain);
    for (int32_t i = 0; i < GRAIN_TILE * GRAIN_TILE; i++) {
        grain[i] = static_cast<uint8_t>(stlxgfx_div255(g_grain_noise[i] * grain_alpha));
    }
}

stlxgfx_surface_t* build_frost(const stlxgfx_surface_t* wallpaper, stlxconf_glass_tint_t tint) {
    stlxgfx_surface_t* frost = stlxgfx_create_surface(wallpaper->width, wallpaper->height, 32, 16, 8, 0);
    if (!frost) {
        return nullptr;
    }

    frost_planes planes;
    planes.width = (static_cast<int32_t>(wallpaper->width) + FROST_SCALE - 1) / FROST_SCALE;
    planes.height = (static_cast<int32_t>(wallpaper->height) + FROST_SCALE - 1) / FROST_SCALE;
    shrink_wallpaper(wallpaper, planes);
    blur_planes(planes);

    saturate_and_knee(planes, look_of(tint).saturation);
    grow_frost(planes, frost);

    return frost;
}

void draw(stlxgfx_surface_t* target, int32_t x, int32_t y, const stlxgfx_surface_t* frost, const surface& glass,
          const damage_list::rect& clip) {
    damage_list::rect frost_bounds = { 0, 0, static_cast<int32_t>(frost->width), static_cast<int32_t>(frost->height) };
    damage_list::rect area = damage_list::intersect(damage_list::intersect(clip, glass.bounds), frost_bounds);
    if (area.w <= 0 || area.h <= 0) {
        return;
    }

    /* Pixels this close to an edge may lie on the rim or outside a rounded corner */
    int32_t edge = std::max(glass.radius, 1);
    const uint8_t* grain = reinterpret_cast<const uint8_t*>(g_tables.grain);
    int32_t first_column = area.x - glass.bounds.x;

    for (int32_t py = area.y; py < area.y + area.h; py++) {
        int32_t row = py - glass.bounds.y;
        const uint32_t* frost_row = row_of(frost, py);
        uint32_t* target_row = row_of(target, py - y);

        const uint32_t (&veil)[4] = g_tables.veil[veil_alpha(row, glass.bounds.h)];
        const uint8_t* grain_row = grain + (row % GRAIN_TILE) * GRAIN_TILE;
        uint32_t rim = rim_alpha(row, glass.rim_fade);
        bool is_edge_row = row < edge || row >= glass.bounds.h - edge;

        int32_t grain_column = first_column % GRAIN_TILE;
        for (int32_t px = area.x; px < area.x + area.w; px++) {
            int32_t column = px - glass.bounds.x;
            uint32_t lit = decor::blend_over(veiled(frost_row[px], veil), g_tables.light, grain_row[grain_column]);
            grain_column = grain_column + 1 == GRAIN_TILE ? 0 : grain_column + 1;

            bool is_edge = is_edge_row || column < edge || column >= glass.bounds.w - edge;
            if (!is_edge) {
                target_row[px - x] = lit;
                continue;
            }

            int32_t distance = decor::rounded_rect_distance(glass.bounds, glass.radius, px, py);
            uint32_t cover = decor::distance_coverage(distance);
            uint32_t ring = cover - decor::distance_coverage(distance + EIGHTHS_PER_PIXEL);
            lit = decor::blend_over(lit, g_tables.light, stlxgfx_div255(rim * ring));
            target_row[px - x] = covered(target_row[px - x], lit, cover);
        }
    }
}

} // namespace material
