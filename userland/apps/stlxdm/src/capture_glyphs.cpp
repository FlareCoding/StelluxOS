#include "capture_glyphs.hpp"

#include "shell/draw.hpp"

#include <cmath>
#include <vector>

/* A trash can on the 16 unit grid: its lid with a handle, and its body open at the top with two ribs */
static std::vector<std::vector<glyph_point>> discard_contours() {
    return {
        { { 2.0f, 3.5f }, { 14.0f, 3.5f }, { 14.0f, 4.75f }, { 2.0f, 4.75f } },
        { { 5.5f, 1.25f }, { 10.5f, 1.25f }, { 10.5f, 3.5f }, { 5.5f, 3.5f } },
        { { 6.75f, 2.5f }, { 9.25f, 2.5f }, { 9.25f, 3.5f }, { 6.75f, 3.5f } },
        { { 3.25f, 5.75f }, { 12.75f, 5.75f }, { 12.0f, 14.75f }, { 4.0f, 14.75f } },
        { { 4.5f, 5.75f }, { 11.5f, 5.75f }, { 10.85f, 13.5f }, { 5.15f, 13.5f } },
        { { 6.4f, 7.5f }, { 7.6f, 7.5f }, { 7.6f, 11.75f }, { 6.4f, 11.75f } },
        { { 8.4f, 7.5f }, { 9.6f, 7.5f }, { 9.6f, 11.75f }, { 8.4f, 11.75f } },
    };
}

/* An almond on the 16 unit grid between two parabolas meeting at its tips, `reach` from its middle */
static std::vector<glyph_point> almond(float reach, float rise) {
    constexpr int32_t STEPS = 12;
    std::vector<glyph_point> outline;
    for (int32_t i = 0; i <= STEPS; i++) {
        float t = -1.0f + 2.0f * static_cast<float>(i) / STEPS;
        outline.push_back({ 8.0f + reach * t, 8.0f - rise * (1.0f - t * t) });
    }

    for (int32_t i = STEPS - 1; i > 0; i--) {
        float t = -1.0f + 2.0f * static_cast<float>(i) / STEPS;
        outline.push_back({ 8.0f + reach * t, 8.0f + rise * (1.0f - t * t) });
    }

    return outline;
}

/* An eye on the 16 unit grid: an almond's outline around its pupil */
static std::vector<std::vector<glyph_point>> preview_contours() {
    constexpr int32_t SIDES = 16;
    constexpr float PUPIL = 2.4f;
    std::vector<glyph_point> pupil;
    for (int32_t i = 0; i < SIDES; i++) {
        float angle = 6.2831853f * static_cast<float>(i) / SIDES;
        pupil.push_back({ 8.0f + PUPIL * std::cos(angle), 8.0f + PUPIL * std::sin(angle) });
    }

    return { almond(7.0f, 5.0f), almond(5.6f, 3.6f), pupil };
}

/* A disc on the 16 unit grid with an exclamation mark cut out of it */
static std::vector<std::vector<glyph_point>> warning_contours() {
    constexpr int32_t SIDES = 24;
    constexpr float RADIUS = 6.5f;
    std::vector<glyph_point> disc;
    for (int32_t i = 0; i < SIDES; i++) {
        float angle = 6.2831853f * static_cast<float>(i) / SIDES;
        disc.push_back({ 8.0f + RADIUS * std::cos(angle), 8.0f + RADIUS * std::sin(angle) });
    }

    return {
        disc,
        { { 7.25f, 4.0f }, { 8.75f, 4.0f }, { 8.5f, 9.0f }, { 7.5f, 9.0f } },
        { { 7.25f, 10.5f }, { 8.75f, 10.5f }, { 8.75f, 12.0f }, { 7.25f, 12.0f } },
    };
}

/* A cross on the 16 unit grid as one polygon, a plus turned an eighth, so the evenodd rule keeps its middle */
static std::vector<std::vector<glyph_point>> close_contours() {
    constexpr float HALF = 0.75f;
    constexpr float REACH = 6.0f;
    constexpr float TURN = 0.70710678f;
    const float plus[12][2] = { { HALF, REACH },  { HALF, HALF },    { REACH, HALF },   { REACH, -HALF },
                                { HALF, -HALF },  { HALF, -REACH },  { -HALF, -REACH }, { -HALF, -HALF },
                                { -REACH, -HALF }, { -REACH, HALF }, { -HALF, HALF },   { -HALF, REACH } };

    std::vector<glyph_point> cross;
    for (const auto& corner : plus) {
        cross.push_back({ 8.0f + (corner[0] - corner[1]) * TURN, 8.0f + (corner[0] + corner[1]) * TURN });
    }

    return { cross };
}

stlxgfx_surface_t* make_capture_glyph(capture_glyph glyph, int32_t size, uint32_t color) {
    switch (glyph) {
    case capture_glyph::preview:
        return make_shape_glyph(size, color, preview_contours());
    case capture_glyph::discard:
        return make_shape_glyph(size, color, discard_contours());
    case capture_glyph::close:
        return make_shape_glyph(size, color, close_contours());
    case capture_glyph::warning:
        return make_shape_glyph(size, color, warning_contours());
    }

    return nullptr;
}
