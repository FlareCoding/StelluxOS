#ifndef STLXDM_OFFER_INTAKE_HPP
#define STLXDM_OFFER_INTAKE_HPP

#include "clipboard.hpp"

#include <sys/socket.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

/* Items of at most this many bytes are copied into objects stlxdm owns as they arrive, at a quarter of a millisecond
 * at most, so their sources can no longer change them. A larger item stays in its source's object, checked and
 * measured, since a copy costs about 3 ms per MB. */
constexpr uint64_t OWNED_COPY_MAX = 64 * 1024;

/* Makes a shared memory object holding `len` bytes of `data`, -1 with errno set */
using object_maker = std::function<int(const void* data, size_t len)>;

/* A client's received handles, oldest first, waiting for the messages that take them */
struct handle_queue {
    int*      handles;
    uint32_t  capacity;
    uint32_t* count;
};

/* Why an offer was refused, which ends the client that sent it */
enum class intake_error { none, missing_handles, not_memory, short_object };

/* Queues the handles of a read's one SCM_RIGHTS message, closing any past the queue's room. False when some were
 * dropped there or truncated by the kernel. */
bool queue_received_handles(handle_queue queue, const msghdr& msg);

/* Whether `have` unhandled bytes may begin a message that takes queued handles. A lone byte, short of a type, may. */
bool may_begin_handle_message(const uint8_t* bytes, uint32_t have);

/* Gives each representation with bytes the oldest queued handle, then checks each object: it must be a regular object,
 * as shared memory is and a socket, pipe or device is not, holding at least its representation's length. Each
 * representation records what its object holds. A refused offer closes every handle it took. */
intake_error take_offer_objects(std::vector<clipboard_rep>& reps, handle_queue queue);

/* Copies an item of OWNED_COPY_MAX bytes or fewer into objects `make` creates, closing the offered ones, and leaves a
 * larger one as it is. A copy that finds no memory leaves the item as offered. */
void own_small_item(std::vector<clipboard_rep>& reps, const object_maker& make);

/* Whether a representation's object still holds its length, which its source may have shrunk since it arrived */
bool holds_length(const clipboard_rep& rep);

/* What an intake error says in the log */
const char* intake_error_text(intake_error error);

#endif
