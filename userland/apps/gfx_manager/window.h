#ifndef WINDOW_H
#define WINDOW_H
#include "canvas.h"

class window {
public:
    static window* create_window(uint32_t width, uint32_t height, const char* title);

    window();
    
    bool init_graphics_ctx();
    
    uint64_t id;
    uint32_t width = 400;
    uint32_t height = 300;
    uint32_t xpos = 60;
    uint32_t ypos = 60;
    kstl::string title;

    inline kstl::shared_ptr<canvas> get_canvas() { return m_canvas; }

private:
    kstl::shared_ptr<canvas> m_canvas;
};

#endif // WINDOW_H
