#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>

#include <internal/commands.h>

int main() {
    ipc::mq_handle_t queue = MESSAGE_QUEUE_ID_INVALID;
    while (queue == MESSAGE_QUEUE_ID_INVALID) {
        msleep(500);
        queue = ipc::message_queue::open("gfx_manager_mq");
    }

    serial::printf("[EXAMPLE_APP] Connected to gfx manager message queue!\n");
    sleep(2);

    stella_ui::internal::userlib_request_create_window req;
    zeromem(&req, sizeof(stella_ui::internal::userlib_request_create_window));

    req.header.type = STELLA_COMMAND_ID_CREATE_WINDOW;
    req.header.size = sizeof(stella_ui::internal::userlib_request_create_window);
    req.width = 480;
    req.height = 360;
    strcpy(req.title, "Demo");

    ipc::mq_message msg;
    msg.payload_size = sizeof(stella_ui::internal::userlib_request_create_window);
    msg.payload = reinterpret_cast<uint8_t*>(&req);

    ipc::message_queue::post_message(queue, &msg);

    return 0;
}
