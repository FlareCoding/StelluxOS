#ifndef WINDOW_H
#define WINDOW_H
#include "canvas.h"
#include "layout.h"

namespace stella_ui {
class window_base {
public:
    window_base() = default;
    virtual ~window_base() = default;

    bool setup();

    void draw_decorations(kstl::shared_ptr<canvas>& cvs);

    virtual void draw() {}

    point position;
    size window_size;
    size real_window_size;
    kstl::string title;
    color background_color;

    // Get the canvas position accounting for decorations
    point get_canvas_position() const;

    inline kstl::shared_ptr<canvas>& get_canvas() { return m_canvas; }

protected:
    kstl::shared_ptr<canvas> m_canvas;

    const uint32_t window_border_thickness = 2;
    const uint32_t title_bar_height = 24;
};
} // namespace stella_ui

#endif // WINDOW_H
