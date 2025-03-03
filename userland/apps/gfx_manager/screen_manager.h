#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H
#include <stella_ui.h>
#include <kstl/vector.h>
#include <ipc/mq.h>

#include <stella_user.h>
#include <internal/commands.h>

struct user_session {
    ipc::mq_handle_t handle;
    stella_ui::window_base* window;
};

enum class screen_manager_mode {
    console,
    compositor
};

class screen_manager {
public:
    screen_manager();
    ~screen_manager();

    bool initialize();

    void set_background_color(const stella_ui::color& color);

    void begin_frame();
    void end_frame();

    void composite_windows();
    void draw_screen_overlays();

    void draw_kernel_log_console();

    void poll_events();

    screen_manager_mode active_mode = screen_manager_mode::console;

private:
    modules::module_base* m_gfx_module;
    kstl::shared_ptr<stella_ui::canvas> m_screen_canvas;
    kstl::hashmap<ipc::mq_handle_t, user_session> m_user_sessions;
    kstl::vector<stella_ui::window_base*> m_window_list;

    ipc::mq_handle_t m_incoming_event_queue;

    char* m_console_log_buffer;
    uint32_t m_max_displayable_console_lines;

    bool _create_canvas(psf1_font* font);
    void _draw_mouse_cursor();

    void _process_event(uint8_t* payload, size_t payload_size);

    bool _establish_user_session(stella_ui::internal::userlib_request_create_session* req);
    bool _send_ack_to_session(ipc::mq_handle_t session_id);
    bool _send_nack_to_session(ipc::mq_handle_t session_id);
};

#endif // SCREEN_MANAGER_H

