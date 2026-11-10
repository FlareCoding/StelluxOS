#include "layers.hpp"

/* The backdrop, each window's glass, frame strips and content, and above
 * them the two top pictures with the dim between */
constexpr uint32_t LAYERS_PER_WINDOW = layer_window::FRAME_STRIPS + 2;
constexpr uint32_t LAYERS_AROUND_WINDOWS = 4;

static bool overlaps(const damage_list::rect& a, const damage_list::rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static bool contains(const damage_list::rect& outer, const damage_list::rect& inner) {
    bool holds_columns = inner.x >= outer.x && inner.x + inner.w <= outer.x + outer.w;
    bool holds_rows = inner.y >= outer.y && inner.y + inner.h <= outer.y + outer.h;
    return holds_columns && holds_rows;
}

/* Whether `r`, which lies inside `bounds`, stays clear of the corner squares of `bounds`, which round by `radius` */
static bool avoids_corners(const damage_list::rect& bounds, int32_t radius, const damage_list::rect& r) {
    bool is_between_columns = r.x >= bounds.x + radius && r.x + r.w <= bounds.x + bounds.w - radius;
    bool is_between_rows = r.y >= bounds.y + radius && r.y + r.h <= bounds.y + bounds.h - radius;

    return is_between_columns || is_between_rows;
}

/* Opaque content covers `r` clear of its rounded bottom corners, and a premultiplied window by its glass, clear of the
 * glass's rounded corners */
static bool covers(const layer_window& w, const damage_list::rect& r) {
    if (w.is_premultiplied) {
        return contains(w.glass.bounds, r) && avoids_corners(w.glass.bounds, w.glass.radius, r);
    }

    if (!contains(w.content, r)) {
        return false;
    }

    int32_t radius = static_cast<int32_t>(w.radius);
    bool is_above_corners = r.y + r.h <= w.content.y + w.content.h - radius;
    bool is_between_corners = r.x >= w.content.x + radius && r.x + r.w <= w.content.x + w.content.w - radius;

    return is_above_corners || is_between_corners;
}

/* The topmost window that covers `r`, or the window count when none does. A premultiplied window covers only on an
 * output with frost, since without glass beneath its content what lies below shows through. */
static size_t find_covering_window(const layer_scene& scene, const damage_list::rect& r) {
    for (size_t i = scene.windows.size(); i-- > 0;) {
        const layer_window& w = scene.windows[i];
        bool shows_below = w.is_premultiplied && scene.frost.address == 0;
        if (!shows_below && covers(w, r)) {
            return i;
        }
    }

    return scene.windows.size();
}

static layer layer_over(const damage_list::rect& bounds, layer_op op) {
    layer l = {};
    l.x0 = bounds.x;
    l.y0 = bounds.y;
    l.x1 = bounds.x + bounds.w;
    l.y1 = bounds.y + bounds.h;
    l.op = op;

    return l;
}

static void set_source(layer& l, const layer_picture& picture) {
    l.address = picture.address;
    l.origin_x = picture.x;
    l.origin_y = picture.y;
    l.pitch = picture.pitch;
}

static void append_piece(
    std::vector<layer>& list,
    const damage_list::rect& r,
    const layer_piece& piece,
    layer_op op
) {
    if (!overlaps(piece.bounds, r)) {
        return;
    }

    layer l = layer_over(piece.bounds, op);
    set_source(l, piece.picture);
    list.push_back(l);
}

static void append_glass(
    std::vector<layer>& list,
    const damage_list::rect& r,
    const layer_window& w,
    const layer_picture& frost
) {
    if (frost.address == 0 || !overlaps(w.glass.bounds, r)) {
        return;
    }

    layer glass = layer_over(w.glass.bounds, layer_op::glass);
    set_source(glass, frost);
    glass.radius = static_cast<uint32_t>(w.glass.radius);
    glass.rim_fade = static_cast<uint32_t>(w.glass.rim_fade);
    list.push_back(glass);
}

static void append_frame(std::vector<layer>& list, const damage_list::rect& r, const layer_window& w) {
    for (const layer_piece& strip : w.frame) {
        append_piece(list, r, strip, layer_op::blend);
    }
}

static void append_content(
    std::vector<layer>& list,
    const damage_list::rect& r,
    const layer_window& w,
    uint32_t content_color
) {
    if (!overlaps(w.content, r)) {
        return;
    }

    layer_op shown = w.is_premultiplied ? layer_op::blend_premultiplied : layer_op::copy;
    layer content = layer_over(w.content, w.has_picture ? shown : layer_op::fill);
    content.radius = w.radius;
    if (w.has_picture) {
        set_source(content, w.picture);
    } else {
        content.color = content_color;
    }

    list.push_back(content);
}

uint32_t max_layers(size_t windows) {
    return static_cast<uint32_t>(windows) * LAYERS_PER_WINDOW + LAYERS_AROUND_WINDOWS;
}

uint32_t append_layers(const layer_scene& scene, const damage_list::rect& r, std::vector<layer>& list) {
    size_t list_start = list.size();
    damage_list::rect whole = { 0, 0, scene.width, scene.height };

    /* The window covering the rectangle overwrites every layer beneath it, its own glass and frame among them
     * unless its premultiplied content blends over them */
    size_t covering = find_covering_window(scene, r);
    size_t first_framed = 0;
    if (covering == scene.windows.size()) {
        append_piece(list, r, { whole, scene.backdrop }, layer_op::copy);
    } else if (scene.windows[covering].is_premultiplied) {
        first_framed = covering;
    } else {
        append_content(list, r, scene.windows[covering], scene.content_color);
        first_framed = covering + 1;
    }

    for (size_t i = first_framed; i < scene.windows.size(); i++) {
        append_glass(list, r, scene.windows[i], scene.frost);
        append_frame(list, r, scene.windows[i]);
        append_content(list, r, scene.windows[i], scene.content_color);
    }

    append_piece(list, r, scene.lower_top, layer_op::blend);

    if (scene.is_dimmed) {
        layer dim = layer_over(whole, layer_op::dim);
        dim.color = scene.dim_color;
        list.push_back(dim);
    }

    append_piece(list, r, scene.upper_top, layer_op::blend);

    return static_cast<uint32_t>(list.size() - list_start);
}
