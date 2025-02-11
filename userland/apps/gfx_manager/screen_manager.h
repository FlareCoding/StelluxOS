#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H
#include <stella_ui.h>
#include <kstl/vector.h>
#include <ipc/mq.h>

class screen_manager {
public:
    screen_manager();
    ~screen_manager();

    bool initialize();

    void begin_frame();
    void end_frame();

    void composite_windows();
    void draw_screen_overlays();

    void poll_events();

private:
    modules::module_base* m_gfx_module;
    kstl::shared_ptr<stella_ui::canvas> m_screen_canvas;
    kstl::vector<stella_ui::window_base*> m_window_registry;

    ipc::mq_handle_t m_incoming_event_queue;

    bool _create_canvas(psf1_font* font);
    void _draw_mouse_cursor();

    void _process_event(uint8_t* payload, size_t payload_size);

    void _register_window(stella_ui::window_base* wnd);
};

#endif // SCREEN_MANAGER_H

