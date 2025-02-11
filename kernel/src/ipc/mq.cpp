#include <ipc/mq.h>

namespace ipc {
mq_handle_t message_queue::s_available_mq_id = 0;
mutex message_queue::s_queue_map_lock = mutex();

kstl::hashmap<kstl::string, mq_handle_t> message_queue::s_mq_name_mappings;
kstl::hashmap<mq_handle_t, kstl::shared_ptr<message_queue>> message_queue::s_message_queue_map;

mq_handle_t message_queue::create(const kstl::string& name) {
    mutex_guard guard(s_queue_map_lock);

    if (s_available_mq_id == 0) {
        s_mq_name_mappings = kstl::hashmap<kstl::string, mq_handle_t>();
        s_message_queue_map = kstl::hashmap<mq_handle_t, kstl::shared_ptr<message_queue>>();
        
        s_available_mq_id++;
    }

    if (s_mq_name_mappings.find(name)) {
        return MESSAGE_QUEUE_ID_INVALID;
    }

    mq_handle_t id = s_available_mq_id++;
    s_message_queue_map[id] = kstl::make_shared<message_queue>();
    s_mq_name_mappings[name] = id;

    return id;
}

mq_handle_t message_queue::open(const kstl::string& name) {
    mutex_guard guard(s_queue_map_lock);

    if (!s_mq_name_mappings.find(name)) {
        return MESSAGE_QUEUE_ID_INVALID;
    }

    return s_mq_name_mappings[name];
}

bool message_queue::post_message(mq_handle_t handle, mq_message* message) {
    // Retrieve the message queue object by handle
    message_queue* queue = _get_mq_object(handle);
    if (!queue) {
        return false; // Invalid handle
    }

    // Delegate to the private member function
    return queue->_post_message(message);
}

bool message_queue::peek_message(mq_handle_t handle) {
    // Retrieve the message queue object by handle
    message_queue* queue = _get_mq_object(handle);
    if (!queue) {
        return false; // Invalid handle
    }

    // Delegate to the private member function
    return queue->_peek_message();
}

bool message_queue::get_message(mq_handle_t handle, mq_message* out_message) {
    // Retrieve the message queue object by handle
    message_queue* queue = _get_mq_object(handle);
    if (!queue) {
        return false; // Invalid handle
    }

    // Delegate to the private member function
    return queue->_get_message(out_message);
}

message_queue* message_queue::_get_mq_object(mq_handle_t handle) {
    mutex_guard guard(s_queue_map_lock);

    if (!s_message_queue_map.find(handle)) {
        return nullptr;
    }

    return s_message_queue_map[handle].get();
}

bool message_queue::_post_message(mq_message* message) {
    // Allocate memory for the new message
    mq_message* new_message = new mq_message;
    if (!new_message) {
        return false;  // Allocation failed
    }

    // Allocate memory for the payload
    new_message->payload_size = message->payload_size;
    new_message->payload = new uint8_t[message->payload_size];
    if (!new_message->payload) {
        delete new_message;
        return false;  // Allocation failed
    }

    // Copy the message data and payload
    new_message->message_id = m_next_message_id++;
    memcpy(new_message->payload, message->payload, message->payload_size);

    // Create a new node to hold the message
    mq_node* new_node = new mq_node;
    if (!new_node) {
        delete[] new_message->payload;
        delete new_message;
        return false;
    }

    new_node->message = new_message;
    new_node->next = nullptr;

    // Lock the queue before modifying it
    mutex_guard guard(m_lock);

    // Append the new node to the end of the queue
    if (!m_tail) {
        m_head = new_node;
        m_tail = new_node;
    } else {
        m_tail->next = new_node;
        m_tail = new_node;
    }

    return true;
}

bool message_queue::_peek_message() {
    mutex_guard guard(m_lock);

    // Return true if the queue is not empty
    return (m_head != nullptr);
}

bool message_queue::_get_message(mq_message* out_message) {
    mutex_guard guard(m_lock);

    if (!m_head) {
        return false;  // Queue is empty
    }

    // Get the node at the head of the queue
    mq_node* node = m_head;
    m_head = node->next;

    if (!m_head) {
        m_tail = nullptr;  // Queue is now empty
    }

    // Copy the message metadata
    out_message->message_id = node->message->message_id;
    out_message->payload_size = node->message->payload_size;

    // Allocate memory for the payload in the output message
    out_message->payload = new uint8_t[node->message->payload_size];
    if (!out_message->payload) {
        delete node->message->payload;
        delete node->message;
        delete node;
        return false;  // Allocation failed
    }

    // Copy the payload
    memcpy(out_message->payload, node->message->payload, node->message->payload_size);

    // Free the message and node memory
    delete[] node->message->payload;
    delete node->message;
    delete node;

    return true;
}

} // namespace ipc
