#ifndef STELLUX_DRIVERS_SERIAL_PORT_H
#define STELLUX_DRIVERS_SERIAL_PORT_H

#include "drivers/serial/port_name.h"
#include "common/list.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace fs { class node; }
namespace sync { struct poll_table; }

namespace serial {

// The received bytes a port holds for reads, about 0.7 s of them at 921,600 baud
constexpr size_t RECEIVE_QUEUE_BYTES = 64 * 1024;

/**
 * One serial port inside the subsystem. Its node in /dev/serial owns it, so it
 * lives until the registry, /dev/serial and every open file have released the
 * node.
 *
 * Received bytes are queued only while a file has the port open, so a first
 * open always finds the queue empty.
 */
class port {
public:
    /**
     * @brief Creates a port for `id` and the port's node, which starts with one
     * reference, the registry's. The port keeps a copy of the strings of `id`.
     * @return The port, or nullptr when memory runs out.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static port* create(const identity& id);

    port();
    ~port();

    const identity& id() const { return m_identity; }
    held_name& name_record() { return m_name; }
    const char* name() const { return m_name.text; }
    fs::node* node() const { return m_node; }

    // The listing's generation when the port was listed. Callers must write and read it under the registry's lock
    uint64_t arrival() const { return m_arrival; }
    void set_arrival(uint64_t generation) { m_arrival = generation; }

    uint64_t received_bytes() const { return m_received_bytes.load_relaxed(); }

    // Read with acquire ordering, so a later `received_bytes` counts every byte this count includes
    uint64_t dropped_bytes() const { return m_dropped_bytes.load_acquire(); }

    list::node registry_link; // in the registry's list of the ports the listing shows

    /**
     * @brief Counts a file opening the port.
     * @return fs::OK, or fs::ERR_IO once the device has left.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t open();

    /**
     * @brief Counts a file closing, and empties the queue at the last close.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void close();

    /**
     * @brief Reads queued bytes into `out`, a user buffer when `is_user_buffer`
     * is set, waiting for a byte unless `flags` holds O_NONBLOCK.
     * @return The bytes read, 0 once the device has left and the queue is
     *         empty, else fs::ERR_IO once receiving failed and the queue is
     *         empty, fs::ERR_AGAIN, fs::ERR_INTR when a signal ends the wait,
     *         or fs::ERR_FAULT.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ssize_t read(void* out, size_t count, uint32_t flags, bool is_user_buffer);

    /**
     * @brief Reports POLL_IN while bytes are queued, or once receiving has
     * failed while the device is attached, and POLL_HUP once the device has
     * left. Subscribes `pt` when given.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint32_t poll(sync::poll_table* pt);

    /**
     * @brief The bytes queued for reads, which FIONREAD reports, also once the
     * device has left.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE size_t queued_bytes();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void queue_received(const uint8_t* data, size_t len);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void mark_failed();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void mark_gone();

private:
    // True when a read has bytes or an end to report
    bool has_read_result_locked() const;

    // What a read that finds no queued bytes returns
    ssize_t empty_read_result_locked(bool may_wait) const;

    void push_locked(const uint8_t* data, size_t len);
    size_t pop_locked(uint8_t* out, size_t len);

    /**
     * Copies the strings of `id` into one block and points `m_identity` at the copies, false when memory runs out.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool copy_identity(const identity& id);

    fs::node* m_node = nullptr;

    identity  m_identity = {};
    char*     m_strings = nullptr; // the identity's strings, which `m_identity` and the name record's `device` point at
    held_name m_name = {};

    uint64_t               m_arrival = 0;
    sync::atomic<uint64_t> m_received_bytes{0}; // every byte the chip delivered, those dropped included
    sync::atomic<uint64_t> m_dropped_bytes{0};  // the received bytes dropped because the queue was full

    sync::spinlock   m_state_lock = sync::SPINLOCK_INIT;
    sync::wait_queue m_read_wq;         // readers and pollers, woken by every change they wait for
    uint8_t*         m_queue = nullptr; // RECEIVE_QUEUE_BYTES, oldest byte at m_queue_head
    size_t           m_queue_head = 0;
    size_t           m_queued = 0;
    uint32_t         m_open_files = 0;
    bool             m_is_gone = false;
    bool             m_has_failed = false;
};

using port_list = list::head<port, &port::registry_link>;

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_PORT_H
