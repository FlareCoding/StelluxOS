#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H
#include "window.h"
#include <kstl/vector.h>

class screen_manager {
public:
    screen_manager();
    ~screen_manager();

    static psf1_font* get_global_system_font();

    bool initialize();

    kstl::shared_ptr<canvas> get_canvas() const;

    void begin_frame();
    void end_frame();

    static void register_window(window* wnd);

    inline kstl::vector<window*>& get_all_windows() { return s_window_registry; }

private:
    modules::module_base*     m_gfx_module;
    kstl::shared_ptr<canvas>  m_canvas;

    bool _create_canvas(psf1_font* font);
    psf1_font* _load_font();

    static psf1_font* s_global_font;
    static kstl::vector<window*> s_window_registry;
};

#endif // SCREEN_MANAGER_H

