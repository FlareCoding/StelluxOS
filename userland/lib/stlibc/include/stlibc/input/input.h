#ifndef STLIBC_INPUT_H
#define STLIBC_INPUT_H
#include "input_event.h"

/**
 * @brief Reads input events from the specified input queue.
 * 
 * @param queue_id The input queue identifier (e.g., INPUT_QUEUE_ID_SYSTEM)
 * @param blocking 0 for non-blocking read, 1 for blocking read
 * @param events Buffer to store the input events
 * @param max_events Maximum number of events to read
 * @return Number of events read on success, negative error code on failure
 */
long stlx_read_input_events(uint32_t queue_id, int blocking, input_event_t* events, size_t max_events);

#endif // STLIBC_INPUT_H
