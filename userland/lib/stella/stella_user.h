#ifndef STELLA_USER_H
#define STELLA_USER_H
#include <string.h>
#include "color.h"

namespace stella_ui {
using window_handle_t = uint64_t;

bool connect_to_compositor();

bool create_window(uint32_t width, uint32_t height, const kstl::string& title);

bool render_content();
} // namespace stella_ui

#endif // STELLA_USER_H

