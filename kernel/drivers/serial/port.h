#ifndef STELLUX_DRIVERS_SERIAL_PORT_H
#define STELLUX_DRIVERS_SERIAL_PORT_H

#include "drivers/serial/port_name.h"
#include "drivers/serial/serial.h"
#include "common/list.h"
#include "sync/atomic.h"
#include "sync/mutex.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "terminal/tty.h"

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
     * reference, the registry's. The port calls `driver` and keeps a copy of
     * the strings of `id`.
     * @return The port, or nullptr when memory runs out.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static port* create(port_driver* driver, const identity& id);

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
     * @brief Counts a file opening the port. The first open sets the chip to
     * the port's settings, which are unknown after setup, and raises DTR and
     * RTS unless the speed is B0.
     * @return fs::OK, fs::ERR_BUSY while the port is held exclusively, or
     *         fs::ERR_IO once the device has left or a chip request fails.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t open();

    /**
     * @brief Counts a file closing. The last close waits for the send in
     * flight up to its deadline, unless a signal is pending, cancels it if it
     * has not ended, lowers DTR and RTS when HUPCL is set, ends exclusive mode
     * and empties the queue.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void close();

    /**
     * @brief Carries out a terminal request a program makes with ioctl, with
     * `arg` as the program passed it, an address in its memory or a number.
     * @return fs::OK, or the error the program gets.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t request(uint32_t cmd, uint64_t arg);

    /**
     * @brief Reads queued bytes into `out`, a user buffer when `is_user_buffer`
     * is set, waiting as the settings' VMIN and VTIME ask unless `flags` holds
     * O_NONBLOCK.
     * @return The bytes read, 0 once the device has left and the queue is
     *         empty or with VMIN 0 when no byte arrives in time, else
     *         fs::ERR_IO once receiving failed and the queue is empty,
     *         fs::ERR_AGAIN, fs::ERR_INTR when a signal ends the wait, or
     *         fs::ERR_FAULT.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ssize_t read(void* out, size_t count, uint32_t flags, bool is_user_buffer);

    /**
     * @brief Copies `data`, a user buffer when `is_user_buffer` is set, into
     * sends of up to SEND_BYTES, one in flight at a time, waiting for each
     * send in flight to end unless `flags` holds O_NONBLOCK.
     * @return The bytes copied into sends, or with none fs::ERR_AGAIN,
     *         fs::ERR_INTR when a signal ends the wait, fs::ERR_IO once the
     *         device has left or a send failed, or fs::ERR_FAULT.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ssize_t write(const void* data, size_t count, uint32_t flags, bool is_user_buffer);

    /**
     * @brief Reports POLL_IN once a byte is queued (VMIN bytes when VTIME is 0
     * and VMIN is above 1) or receiving has failed while the device is
     * attached, POLL_OUT while a write starts a send at once or fails at once,
     * and POLL_HUP once the device has left. Subscribes `pt` when given.
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

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void end_send(ssize_t taken_or_error);

    /**
     * @brief Stops calling the driver, once a call in progress returns.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void release_driver();

private:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE ssize_t start_send(const uint8_t* data, size_t length, bool may_wait, bool is_user_buffer);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t wait_for_send_end(uint64_t deadline_ns);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE uint64_t send_deadline();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t copy_modem_lines_out(uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t take_modem_outputs(uint32_t cmd, uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t change_modem_outputs(uint32_t lines);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void release_write_lock();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void cancel_started_send();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t copy_send_length_out(uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE bool is_gone();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void drop_queued();

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t copy_settings_out(uint64_t arg, size_t length);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t apply_settings(uint64_t arg, size_t length, bool waits_for_send, bool drops_queued);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t flush(uint64_t which);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t copy_queued_count_out(uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t copy_window_size_out(uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t take_window_size(uint64_t arg);

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t set_exclusive(bool is_exclusive);

    /**
     * @brief Copies what `settings` asks of reads and received bytes into the
     * fields the state lock guards.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void take_read_settings(const terminal::termios2& settings);

    // True when a read of `count` bytes may return without waiting further
    bool is_read_ready_locked(size_t count) const;

    // The queued bytes that make the port readable: VMIN while VTIME is 0 and VMIN is above 1, else one
    size_t min_readable_bytes_locked() const;

    // When a read that started at `started_ns` stops waiting under VTIME, or 0 when it waits without a limit
    uint64_t read_deadline_locked(uint64_t started_ns) const;

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

    // Guards the settings, the modem outputs, the window size and exclusive mode across each chip request. The driver
    // pointer changes only under this lock and the write lock, so a call holding either may use it
    sync::mutex        m_control_lock;
    port_driver*       m_driver = nullptr;
    terminal::termios2 m_settings = {};
    line_settings      m_line = {};          // what each first open sends the chip, never with a speed of 0
    uint32_t           m_modem_outputs = 0;  // DTR and RTS as last set
    terminal::winsize  m_window_size = {};
    bool               m_is_exclusive = false; // TIOCEXCL, which refuses other opens until the last close

    identity  m_identity = {};
    char*     m_strings = nullptr; // the identity's strings, which `m_identity` and the name record's `device` point at
    held_name m_name = {};

    uint64_t               m_arrival = 0;
    sync::atomic<uint64_t> m_received_bytes{0}; // every byte the chip delivered, those dropped included
    sync::atomic<uint64_t> m_dropped_bytes{0};  // the received bytes dropped because the queue was full

    // Held from each copy into the staging buffer to its send's start, and while TCSETSW, TCSETSF or the last close
    // waits for the send to end, so no new send starts meanwhile
    sync::mutex m_write_lock;
    uint8_t*    m_staging = nullptr; // SEND_BYTES, which the send in flight reads until it ends

    sync::spinlock   m_state_lock = sync::SPINLOCK_INIT;
    sync::wait_queue m_read_wq;         // readers and pollers, woken by every change they wait for
    sync::wait_queue m_send_wq;         // writers and pollers, woken by every change they wait for
    uint8_t*         m_queue = nullptr; // RECEIVE_QUEUE_BYTES, oldest byte at m_queue_head
    size_t           m_queue_head = 0;
    size_t           m_queued = 0;
    size_t           m_read_min = 1;        // VMIN
    uint64_t         m_read_timeout_ns = 0; // VTIME
    bool             m_is_receiving = true; // CREAD
    uint64_t         m_last_receive_ns = 0;
    size_t           m_send_length = 0; // the bytes in the send in flight, 0 with none
    uint32_t         m_open_files = 0;
    bool             m_is_gone = false;
    bool             m_has_failed = false;
    bool             m_is_send_started = false; // the driver's send call has returned, so the send may be cancelled
    bool             m_has_send_failed = false;
    bool             m_is_send_failure_logged = false;
    bool             m_is_cancelling = false; // a cancel is running, so no send starts
};

using port_list = list::head<port, &port::registry_link>;

} // namespace serial

#endif // STELLUX_DRIVERS_SERIAL_PORT_H
