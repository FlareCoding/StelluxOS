#include "stella_user.h"
#include "internal/commands.h"
#include <ipc/mq.h>
#include <time/time.h>
#include <sched/sched.h>
#include <serial/serial.h>

namespace stella_ui {
ipc::mq_handle_t g_outbound_connection_id = MESSAGE_QUEUE_ID_INVALID;
ipc::mq_handle_t g_inbound_connection_id = MESSAGE_QUEUE_ID_INVALID;

bool _send_compositor_request(void* req, size_t size);
bool _get_compositor_response(void* resp);
bool _wait_for_ack_response();

bool connect_to_compositor() {
    // Connect to the gfx_manager_process message queue
    uint32_t retries = 20;
    while (g_outbound_connection_id == MESSAGE_QUEUE_ID_INVALID && retries > 0) {
        g_outbound_connection_id = ipc::message_queue::open("gfx_manager_mq");

        if (g_outbound_connection_id == MESSAGE_QUEUE_ID_INVALID) {
            msleep(100);
            --retries;
        }
    }

    // Now that we have connected, we can create a session message queue
    kstl::string session_name = "stella_session:";
    session_name += kstl::to_string(static_cast<uint32_t>(current->pid));

    // Create an inbound message queue
    g_inbound_connection_id = ipc::message_queue::create(session_name);
    if (g_inbound_connection_id == MESSAGE_QUEUE_ID_INVALID) {
        return false;
    }

    // Now we need to notify the gfx manager process to connect to the session mq
    internal::userlib_request_create_session req;
    zeromem(&req, sizeof(internal::userlib_request_create_session));

    req.header.type = STELLA_COMMAND_ID_CREATE_SESSION;
    strcpy(req.name, session_name.c_str());

    if (!_send_compositor_request(&req, sizeof(internal::userlib_request_create_session))) {
        return false;
    }

    // At this point we've sent the request and need to wait for the ACK message
    if (!_wait_for_ack_response()) {
        return false;
    }

    // If all checks passed, the connection is now fully established and active
    return true;
}

bool create_window(uint32_t width, uint32_t height, const kstl::string& title) {
    internal::userlib_request_create_window req;
    zeromem(&req, sizeof(internal::userlib_request_create_window));

    req.header.type = STELLA_COMMAND_ID_CREATE_WINDOW;
    req.width = width;
    req.height = height;
    memcpy(req.title, title.data(), kstl::min(sizeof(req.title) - 1, title.length()));

    return _send_compositor_request(&req, sizeof(internal::userlib_request_create_window));
}

bool render_content() {
    internal::userlib_request_render_content req;
    zeromem(&req, sizeof(internal::userlib_request_header));

    req.header.type = STELLA_COMMAND_ID_RENDER_CONTENT;

    return _send_compositor_request(&req, sizeof(internal::userlib_request_header));
}

bool _send_compositor_request(void* req, size_t size) {
    reinterpret_cast<internal::userlib_request_header*>(req)->session_id = g_inbound_connection_id;

    ipc::mq_message msg;
    msg.payload_size = size;
    msg.payload = reinterpret_cast<uint8_t*>(req);

    return ipc::message_queue::post_message(g_outbound_connection_id, &msg);
}

bool _get_compositor_response(ipc::mq_message& resp) {
    uint32_t retries = 20;
    while (retries > 0) {
        if (!ipc::message_queue::peek_message(g_inbound_connection_id)) {
            msleep(100);
            --retries;
            continue;
        }

        break;
    }

    // Read and verify the ACK
    if (!ipc::message_queue::get_message(g_inbound_connection_id, &resp)) {
        return false;
    }

    return true;
}

bool _wait_for_ack_response() {
    ipc::mq_message resp;
    if (!_get_compositor_response(resp)) {
        return false;
    }

    if (resp.payload_size != 4) {
        return false;
    }

    if (strcmp((const char*)resp.payload, "ACK") != 0) {
        return false;
    }

    return true;
}
} // namespace stella_ui
