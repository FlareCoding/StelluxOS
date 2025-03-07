#ifndef INPUT_QUEUE_H
#define INPUT_QUEUE_H
#include "input_event.h"

namespace input {
/** 
 * @brief A fixed-size ring buffer for storing input events.
 */
class input_queue {
public:
    /** 
     * @brief Constructs an input queue with a specified size.
     * @param queue_id The unique ID associated with this queue.
     * @param capacity The maximum number of events this queue can hold.
     */
    input_queue(uint32_t queue_id, size_t capacity);

    /**
     * @brief Class destructor
     */
    ~input_queue();

    /** 
     * @brief Pushes a new event into the queue.
     * @param event The event to push.
     * @return True if the event was successfully pushed, false if the queue was full.
     */
    bool push_event(const input_event_t& event);

    /** 
     * @brief Pops an event from the queue.
     * @param event Reference to store the popped event.
     * @return True if an event was popped, false if the queue was empty.
     */
    bool pop_event(input_event_t& event);

    /** 
     * @brief Checks if the queue has any pending events.
     * @return True if there are events available, false otherwise.
     */
    bool has_events() const;

    /** 
     * @brief Blocks until an event is available and then pops it.
     * @param event Reference to store the popped event.
     * @return True if event was retrieved successfully, false otherwise.
     */
    bool wait_and_pop(input_event_t& event);

    /** 
     * @brief Gets the unique ID of this input queue.
     * @return The queue ID.
     */
    uint32_t get_queue_id() const { return m_queue_id; }

private:
    uint32_t m_queue_id;       // Unique identifier for this queue.
    size_t m_capacity;         // Maximum number of events the queue can store.
    size_t m_head;             // Head index for the ring buffer.
    size_t m_tail;             // Tail index for the ring buffer.
    input_event_t* m_buffer;   // Ring buffer for storing events.
    volatile uint32_t m_size;  // Current number of events in the queue (atomic updates required).
};
} // namespace input

#endif // INPUT_QUEUE_H

