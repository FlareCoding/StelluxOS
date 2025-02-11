#ifndef STELLA_USER_H
#define STELLA_USER_H
#include <string.h>
#include "color.h"
#include "canvas.h"

namespace stella_ui {
enum class compositor_event : uint64_t {
    invalid         = 0,
    comp_evt_paint  = 1
};

using window_handle_t = uint64_t;

bool connect_to_compositor();

bool create_window(
    uint32_t width,
    uint32_t height,
    const kstl::string& title,
    const color& bg_color = color::dark_gray
);

bool request_map_window_canvas(kstl::shared_ptr<canvas>& out_canvas);

bool peek_compositor_events();

compositor_event get_compositor_event();
} // namespace stella_ui

#endif // STELLA_USER_H

