#ifndef STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H
#define STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H

#include "drivers/usb/core/usb_device.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace usb {

enum class transfer_status : int32_t {
    ok = 0,
    short_packet = 1,
    cancelled = -1,
    device_gone = -2,
    stalled = -3,
    io_error = -4,
    invalid = -5,
};

enum transfer_flags : uint32_t {
    allow_short = 1u << 0,
};

struct transfer_request {
    uint8_t endpoint_addr = 0;
    void* buffer = nullptr;
    uint32_t requested_length = 0;
    uint32_t actual_length = 0;
    uint32_t flags = 0;
    transfer_status status = transfer_status::invalid;
    bool pending = false;
    sync::spinlock lock;
    sync::wait_queue complete_wq;

    // Internal linkage for per-endpoint software queues.
    transfer_request* next = nullptr;
    void* hcd_private = nullptr;
};

struct in_stream;

// The most reads a stream with a callback keeps posted
constexpr uint32_t MAX_IN_STREAM_READS = 64;

/**
 * A stream's callback, called on the host controller driver's task with the
 * stream's `context` once per completed read, in completion order. It must
 * not block or call into the USB core, and `data`, null only when `length`
 * is 0, holds the read's bytes until the call returns. A status other than
 * `ok` or `short_packet` comes once, in the stream's last call: `cancelled`
 * from a close, with the stopped read's bytes, `io_error` from a close whose
 * stop fails, `device_gone` from an unplug, or `stalled` or `io_error` from
 * a failed read.
 */
using in_stream_callback = void (*)(void* context, const uint8_t* data, uint32_t length, transfer_status status);

void init_transfer_request(transfer_request& req,
                           uint8_t endpoint_addr,
                           void* buffer,
                           uint32_t requested_length,
                           uint32_t flags = 0);

int32_t submit_transfer_async(device* dev, transfer_request& req);
int32_t await_transfer(transfer_request& req);
void cancel_transfer(device* dev, transfer_request& req);

/**
 * Streams reads of `payload_length` bytes from an interrupt or bulk IN
 * endpoint until the stream is closed, its device is unplugged or a read
 * fails. With `on_payload` it keeps `read_count` reads posted, 2 to
 * `MAX_IN_STREAM_READS`. Without it, `read_count` must be 1 and payloads
 * queue for `read_in_stream`, so a bulk endpoint must have `on_payload`.
 * @param payload_length At most a page, whole packets on a bulk endpoint.
 * @return 0, or -1 with no call made when an argument breaks these rules,
 * memory runs out, or the endpoint has a stream, transfers, an unplug under
 * way, or a failed stop or read behind it.
 */
int32_t open_in_stream(device* dev,
                       uint8_t endpoint_addr,
                       uint32_t payload_length,
                       uint32_t read_count,
                       in_stream_callback on_payload,
                       void* context,
                       in_stream** out_stream);

/**
 * Copies the oldest payload queued by a stream opened without a callback
 * into `buffer`, as much as fits, waiting while none is queued. The queue
 * keeps the newest two payloads.
 * @return 0, or -1 for a null stream or empty buffer, once the stream has
 * ended, or when it has a callback.
 */
int32_t read_in_stream(in_stream* stream,
                       void* buffer,
                       uint32_t buffer_len,
                       uint32_t* out_length);

/**
 * Ends and frees the stream, stopping its posted reads. Its last call has
 * returned by the time this returns. The wait can last until any device
 * setup in progress ends, and during an unplug until the device's drivers'
 * `disconnect()` calls have returned, so a `disconnect()` must not wait for
 * a task that may be closing a stream. It must not be called from any
 * stream's callback, since it can wait for the controller.
 */
void close_in_stream(in_stream* stream);

/**
 * Synchronous control transfer on EP0. Fails at once after the device's
 * unplug, and on the HCD task while another task's request holds EP0.
 * For IN (device-to-host) data is filled by the device, for OUT
 * (host-to-device) data is sent to the device. Direction is encoded
 * in request_type bit 7 (0=OUT, 1=IN). An IN reply shorter than
 * `length` leaves the rest of `data` zeroed.
 * @return 0 on success, negative on failure.
 */
int32_t control_transfer(device* dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void* data, uint16_t length);

/**
 * Synchronous interrupt transfer on the given endpoint. Blocks until the
 * transfer completes. Direction is determined by endpoint address bit 7.
 * @return 0 on success, negative on failure.
 */
int32_t interrupt_transfer(device* dev, uint8_t endpoint_addr,
                           void* buffer, uint32_t length);

/**
 * Synchronous bulk transfer on the given endpoint. Blocks until the
 * transfer completes. Direction is determined by endpoint address bit 7.
 * @return 0 on success, negative on failure.
 */
int32_t bulk_transfer(device* dev, uint8_t endpoint_addr,
                      void* buffer, uint32_t length);

} // namespace usb

#endif // STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H
