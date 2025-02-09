#ifndef LAYOUT_H
#define LAYOUT_H
#include <types.h>

namespace stella_ui {
struct size {
    uint32_t width;
    uint32_t height;
};

struct point {
    int32_t x;
    int32_t y;
    int32_t z;
};
} // namespace stella_ui

#endif // LAYOUT_H

