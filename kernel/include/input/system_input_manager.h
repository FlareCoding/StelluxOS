#ifndef SYSTEM_INPUT_MANAGER_H
#define SYSTEM_INPUT_MANAGER_H

#include "input_queue.h"
#include <kstl/hashmap.h>

namespace input {
/**
 * @brief Manages system-wide input queues and event dispatching.
 */
class system_input_manager {
public:
    /**
     * @brief Default constructor.
     */
    system_input_manager() = default;

    /**
     * @brief Retrieves the singleton instance of the system_input_manager.
     * @return Reference to the singleton instance.
     */
    static system_input_manager& get();

    /**
     * @brief Initializes the system input manager, creating default queues.
     */
    void init();

    /**
     * @brief Creates a new input queue.
     * @param queue_id Unique identifier for the queue.
     * @param capacity Maximum number of events the queue can hold.
     * @return True if successfully created, false if the queue already exists.
     */
    bool create_queue(uint32_t queue_id, size_t capacity);

    /**
     * @brief Retrieves an input queue by its ID.
     * @param queue_id The queue identifier.
     * @return Pointer to the input queue, or nullptr if not found.
     */
    input_queue* get_queue(uint32_t queue_id);

    /**
     * @brief Pushes an event to the appropriate input queue.
     * @param queue_id The ID of the queue.
     * @param event The event to push.
     * @return True if the event was successfully pushed, false if the queue was full or not found.
     */
    bool push_event(uint32_t queue_id, const input_event_t& event);

private:
    kstl::hashmap<uint32_t, input_queue*> m_input_queues; // Stores input queues mapped by ID.
};
} // namespace input

#endif // SYSTEM_INPUT_MANAGER_H
