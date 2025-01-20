#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H
#include "canvas.h"

class screen_manager {
public:
    screen_manager();
    ~screen_manager();

    bool initialize();

    kstl::shared_ptr<canvas> get_canvas() const;

    void begin_frame();
    void end_frame();

private:
    modules::module_base*     m_gfx_module;
    kstl::shared_ptr<canvas>  m_canvas;

    bool _create_canvas(psf1_font* font);
    psf1_font* _load_font();
};

#endif // SCREEN_MANAGER_H

