#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H
#include <stella_ui.h>
#include <kstl/vector.h>

class screen_manager {
public:
    screen_manager();
    ~screen_manager();

    bool initialize();

    kstl::shared_ptr<stella_ui::canvas> get_screen_canvas() const;

    void begin_frame();
    void end_frame();

    static void register_window(stella_ui::window_base* wnd);

    inline kstl::vector<stella_ui::window_base*>& get_all_windows() { return s_window_registry; }

private:
    modules::module_base*     m_gfx_module;
    kstl::shared_ptr<stella_ui::canvas>  m_canvas;

    bool _create_canvas(psf1_font* font);

    static kstl::vector<stella_ui::window_base*> s_window_registry;
};

#endif // SCREEN_MANAGER_H

