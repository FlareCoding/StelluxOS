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

struct interrupt_in_stream;

void init_transfer_request(transfer_request& req,
                           uint8_t endpoint_addr,
                           void* buffer,
                           uint32_t requested_length,
                           uint32_t flags = 0);

int32_t submit_transfer_async(device* dev, transfer_request& req);
int32_t await_transfer(transfer_request& req);
void cancel_transfer(device* dev, transfer_request& req);

int32_t open_interrupt_in_stream(device* dev,
                                 uint8_t endpoint_addr,
                                 uint32_t payload_length,
                                 interrupt_in_stream** out_stream);

int32_t read_interrupt_in_stream(interrupt_in_stream* stream,
                                 void* buffer,
                                 uint32_t buffer_len,
                                 uint32_t* out_length);

void close_interrupt_in_stream(interrupt_in_stream* stream);

// Synchronous control transfer on EP0.
// For IN (device-to-host): data is filled by the device.
// For OUT (host-to-device): data is sent to the device.
// Direction is encoded in request_type bit 7 (0=OUT, 1=IN).
// Returns 0 on success, negative on failure.
int32_t control_transfer(device* dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void* data, uint16_t length);

// Synchronous interrupt transfer on the given endpoint.
// Direction (IN/OUT) is determined by the endpoint address bit 7.
// Blocks until the transfer completes.
// Returns 0 on success, negative on failure.
int32_t interrupt_transfer(device* dev, uint8_t endpoint_addr,
                           void* buffer, uint32_t length);

// Synchronous bulk transfer on the given endpoint.
// Direction (IN/OUT) is determined by the endpoint address bit 7.
// Blocks until the transfer completes.
// Returns 0 on success, negative on failure.
int32_t bulk_transfer(device* dev, uint8_t endpoint_addr,
                      void* buffer, uint32_t length);

} // namespace usb

#endif // STELLUX_DRIVERS_USB_CORE_USB_TRANSFER_H
