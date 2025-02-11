#include "screen_manager.h"
#include <arch/x86/cpuid.h>

extern uint64_t g_mouse_cursor_pos_x;
extern uint64_t g_mouse_cursor_pos_y;

screen_manager::screen_manager()
    : m_gfx_module(nullptr), m_screen_canvas(nullptr) {}

screen_manager::~screen_manager() {
    // TO-DO: resource clean-up
}

bool screen_manager::initialize() {
    psf1_font* font = stella_ui::_load_system_font();
    if (!font) {
        return false;
    }
    
    if (!_create_canvas(font)) {
        return false;
    }

    m_incoming_event_queue = ipc::message_queue::create("gfx_manager_mq");
    if (m_incoming_event_queue == MESSAGE_QUEUE_ID_INVALID) {
        return false;
    }

    return true;
}

void screen_manager::begin_frame() {
    m_screen_canvas->clear();
}

void screen_manager::end_frame() {
    if (!m_gfx_module) {
        return;
    }

    auto& mgr = modules::module_manager::get();
    mgr.send_command(
        m_gfx_module,
        modules::gfx_framebuffer_module::CMD_SWAP_BUFFERS,
        nullptr, 0, nullptr, 0
    );
}

void screen_manager::composite_windows() {
    for (auto window : m_window_list) {
        // Render window content into its canvas
        window->draw();

        // Render window decorations
        window->draw_decorations(m_screen_canvas);

        // Composite the window onto the main screen's canvas
        auto window_canvas = window->get_canvas().get();
        auto window_canvas_pos = window->get_canvas_position();
        m_screen_canvas->composite_canvas(window_canvas_pos.x, window_canvas_pos.y, window_canvas);
    }
}

void screen_manager::draw_screen_overlays() {
    char cpu_vendor_str[16] = { 0 };
    arch::x86::cpuid_read_vendor_id(cpu_vendor_str);

    char cpu_vendor_display_str_buf[128] = { 0 };
    sprintf(cpu_vendor_display_str_buf, 127, "CPU: %s", cpu_vendor_str);

    char time_str_buf[128] = { 0 };
    uint64_t sys_uptime = kernel_timer::get_system_time_in_seconds();
    zeromem(time_str_buf, 127);

    uint64_t hours = sys_uptime / 3600;
    uint64_t minutes = (sys_uptime % 3600) / 60;
    uint64_t seconds = sys_uptime % 60;

    sprintf(time_str_buf, 127, "System Uptime: %lluh %llum %llus", hours, minutes, seconds);

    m_screen_canvas->draw_string(m_screen_canvas->width() - 220, 2, time_str_buf, 0xffffffff);
    m_screen_canvas->draw_string(16, 2, cpu_vendor_display_str_buf, 0xffffffff);

    _draw_mouse_cursor();
}

void screen_manager::poll_events() {
    while (ipc::message_queue::peek_message(m_incoming_event_queue)) {
        ipc::mq_message request;
        if (!ipc::message_queue::get_message(m_incoming_event_queue, &request)) {
            break;
        }

        _process_event(request.payload, request.payload_size);
    }
}

bool screen_manager::_create_canvas(psf1_font* font) {
    // Find the graphics module
    auto& mgr = modules::module_manager::get();
    m_gfx_module = mgr.find_module("gfx_framebuffer_module");
    if (!m_gfx_module) {
        serial::printf("screen_manager: Failed to find gfx_framebuffer_module\n");
        return false;
    }

    // Wait until it's running
    while (m_gfx_module->state() != modules::module_state::running) {
        msleep(100);
    }

    // Request to map the back buffer
    stella_ui::framebuffer_t fb;
    bool result = mgr.send_command(
        m_gfx_module,
        modules::gfx_framebuffer_module::CMD_MAP_BACKBUFFER,
        nullptr, 0,
        &fb, sizeof(fb)
    );

    if (!result || !fb.data) {
        serial::printf("[!] screen_manager: Failed to map back buffer.\n");
        return false;
    }

    // Create the canvas
    m_screen_canvas = kstl::make_shared<stella_ui::canvas>(fb, font);

#if 0
    serial::printf(
        "screen_manager: Successfully initialized canvas: %ux%u pitch=%u bpp=%u\n",
        fb.width, fb.height, fb.pitch, fb.bpp
    );
#endif

    return true;
}

void screen_manager::_draw_mouse_cursor() {
    static const char* cursor_shape[16] = {
        "X                 ",
        "XX                ",
        "X.X               ",
        "X..X              ",
        "X...X             ",
        "X....X            ",
        "X.....X           ",
        "X......X          ",
        "X.......X         ",
        "X........X        ",
        "X...XXXXXXX       ",
        "X..XX             ",
        "X.X               ",
        "XX                ",
        "X                 ",
        "                  "
    };

    const int height = 16;
    const int width = 16;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            if (cursor_shape[row][col] == 'X') {
                m_screen_canvas->fill_rect(g_mouse_cursor_pos_x + col, g_mouse_cursor_pos_y + row, 1, 1, 0xffffffff);
            } else if (cursor_shape[row][col] == '.') {
                m_screen_canvas->fill_rect(g_mouse_cursor_pos_x + col, g_mouse_cursor_pos_y + row, 1, 1, 0x00000000);
            }
        }
    }
}

void screen_manager::_process_event(uint8_t* payload, size_t payload_size) {
    using namespace stella_ui::internal;

    auto hdr = reinterpret_cast<userlib_request_header*>(payload);

    switch (hdr->type) {
    case STELLA_COMMAND_ID_CREATE_SESSION: {
        _establish_user_session(reinterpret_cast<userlib_request_create_session*>(payload));
        break;
    }
    case STELLA_COMMAND_ID_CREATE_WINDOW: {
        auto req = reinterpret_cast<userlib_request_create_window*>(payload);

        stella_ui::window_base* window = new stella_ui::window_base();
        window->position.x = 100;
        window->position.y = 80;
        window->window_size = {
            .width = req->width,
            .height = req->height
        };
        window->title = req->title;
        if (window->setup()) {
            serial::printf("[GFX_MANAGER] Successfully created user window\n");
            m_window_list.push_back(window);
        }
        break;
    }
    default: {
        serial::printf("[GFX_MANAGER] Unknown command received: 0x%llx\n", hdr->type);
        break;
    }
    }
}

bool screen_manager::_establish_user_session(stella_ui::internal::userlib_request_create_session* req) {
    // Connect to the user session message queue
    uint32_t retries = 20;
    ipc::mq_handle_t handle = MESSAGE_QUEUE_ID_INVALID;
    while (handle == MESSAGE_QUEUE_ID_INVALID && retries > 0) {
        handle = ipc::message_queue::open(req->name);

        if (handle == MESSAGE_QUEUE_ID_INVALID) {
            msleep(100);
            --retries;
        }
    }

    if (handle == MESSAGE_QUEUE_ID_INVALID) {
        serial::printf("[GFX_MANAGER] Failed to connect to user session '%s'\n", req->name);
        return false;
    }

    user_session session;
    zeromem(&session, sizeof(user_session));

    session.handle = handle;
    m_user_sessions[handle] = session;

    char ack_str[4] = { 'A', 'C', 'K', '\0' };
    
    ipc::mq_message ack;
    ack.payload_size = 4;
    ack.payload = reinterpret_cast<uint8_t*>(ack_str);

    if (!ipc::message_queue::post_message(handle, &ack)) {
        serial::printf("[GFX_MANAGER] Failed to send ACK to user session '%s'\n", req->name);
        return false;
    }

    serial::printf("[GFX_MANAGER] Connected to user session '%s'\n", req->name);
    return true;
}

