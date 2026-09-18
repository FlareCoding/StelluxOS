#ifndef STELLUX_NET_TCP_RECORD_H
#define STELLUX_NET_TCP_RECORD_H

#include "net/ipv4.h"
#include "common/hashmap.h"
#include "sync/spinlock.h"
#include "rc/ref_counted.h"

namespace net {

class interface;

namespace tcp {

/**
 * The four values that name one connection: this host's address and port and
 * the peer's. Ports are in host order. Every table lookup is by tuple.
 */
struct tuple {
    ipv4::ipv4_addr local_addr;
    ipv4::ipv4_addr remote_addr;
    uint16_t        local_port;
    uint16_t        remote_port;

    inline bool operator==(const tuple& other) const {
        return local_port == other.local_port && remote_port == other.remote_port &&
               local_addr == other.local_addr && remote_addr == other.remote_addr;
    }

    inline bool operator!=(const tuple& other) const { return !(*this == other); }
};

enum class record_kind : uint8_t {
    request    = 0,
    connection = 1,
    timewait   = 2,
};

/**
 * The header every table record begins with, so one lookup finds a request, a
 * connection, or a TIME-WAIT record and reads the kind afterwards. A lookup
 * takes a reference under the table lock, the caller then takes the record's
 * lock, and the table lock is never taken while a record lock is held. The
 * reference count reaches zero only once the record has left the table and
 * every timer holding it has run or been cancelled.
 */
struct record : rc::ref_counted<record> {
    record_kind    kind;
    hashmap::node  table_link;
    tuple          key;
    interface*     iface;
    sync::spinlock lock;

    /**
     * @brief Frees the record as the kind it is, once the last reference is gone.
     */
    static void ref_destroy(record* self);
};

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_RECORD_H
