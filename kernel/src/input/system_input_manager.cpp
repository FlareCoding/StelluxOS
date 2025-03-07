#include <input/system_input_manager.h>

namespace input {
system_input_manager g_system_input_manager;

system_input_manager& system_input_manager::get() {
    return g_system_input_manager;
}

void system_input_manager::init() {
    // Initialize the hashmap object
    m_input_queues = kstl::hashmap<uint32_t, input_queue*>();

    // Initialize default input queues
    create_queue(INPUT_QUEUE_ID_KBD, 128);
    create_queue(INPUT_QUEUE_ID_POINTER, 128);
}

bool system_input_manager::create_queue(uint32_t queue_id, size_t capacity) {
    if (m_input_queues.find(queue_id)) {
        return false; // Queue already exists
    }

    m_input_queues[queue_id] = new input_queue(queue_id, capacity);
    return true;
}

input_queue* system_input_manager::get_queue(uint32_t queue_id) {
    if (!m_input_queues.find(queue_id)) {
        return nullptr;
    }
    return m_input_queues[queue_id];
}

bool system_input_manager::push_event(uint32_t queue_id, const input_event_t& event) {
    input_queue* queue = get_queue(queue_id);
    if (!queue) {
        return false; // Queue does not exist
    }
    return queue->push_event(event);
}   
} // namespace input
