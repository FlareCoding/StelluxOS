#include <ipc/mq.h>
#include <serial/serial.h>
#include <time/time.h>

struct test_msg {
    uint64_t secret;
};

int main() {
    ipc::mq_handle_t queue = MESSAGE_QUEUE_ID_INVALID;
    while (queue == MESSAGE_QUEUE_ID_INVALID) {
        msleep(500);
        queue = ipc::message_queue::open("gfx_manager_mq");
    }

    serial::printf("[EXAMPLE_APP] Connected to gfx manager message queue!\n");

    test_msg content;
    content.secret = 4554;

    ipc::mq_message msg;
    msg.payload_size = sizeof(test_msg);
    msg.payload = (uint8_t*)&content;

    ipc::message_queue::post_message(queue, &msg);

    return 0;
}
