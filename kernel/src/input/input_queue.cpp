#include <input/input_queue.h>
#include <memory/memory.h>
#include <sched/sched.h>

namespace input {
input_queue::input_queue(uint32_t queue_id, size_t capacity)
    : m_queue_id(queue_id), m_capacity(capacity), m_head(0), m_tail(0), m_size(0) {
    m_buffer = new input_event_t[m_capacity];
}

input_queue::~input_queue() {
    delete[] m_buffer;
}

bool input_queue::push_event(const input_event_t& event) {
    if (__atomic_load_n(&m_size, __ATOMIC_RELAXED) >= m_capacity) {
        // Queue is full, event is lost
        return false;
    }
    
    m_buffer[m_tail] = event;
    m_tail = (m_tail + 1) % m_capacity;
    __atomic_fetch_add(&m_size, 1, __ATOMIC_RELEASE);
    return true;
}

bool input_queue::pop_event(input_event_t& event) {
    if (__atomic_load_n(&m_size, __ATOMIC_ACQUIRE) == 0) {
        // Queue is empty
        return false;
    }
    
    event = m_buffer[m_head];
    m_head = (m_head + 1) % m_capacity;
    __atomic_fetch_sub(&m_size, 1, __ATOMIC_RELEASE);
    return true;
}

bool input_queue::has_events() const {
    return __atomic_load_n(&m_size, __ATOMIC_ACQUIRE) > 0;
}

bool input_queue::wait_and_pop(input_event_t& event) {
    while (!has_events()) {
        sched::yield();
    }
    return pop_event(event);
}
} // namespace input
