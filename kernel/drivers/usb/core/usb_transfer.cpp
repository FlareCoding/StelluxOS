#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/xhci/xhci.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"

namespace usb {

struct interrupt_in_stream {
    device* dev = nullptr;
    uint8_t endpoint_addr = 0;
};

void init_transfer_request(transfer_request& req,
                           uint8_t endpoint_addr,
                           void* buffer,
                           uint32_t requested_length,
                           uint32_t flags) {
    req.endpoint_addr = endpoint_addr;
    req.buffer = buffer;
    req.requested_length = requested_length;
    req.actual_length = 0;
    req.flags = flags;
    req.status = transfer_status::invalid;
    req.pending = false;
    req.lock = sync::SPINLOCK_INIT;
    req.complete_wq.init();
    req.next = nullptr;
    req.hcd_private = nullptr;
}

int32_t submit_transfer_async(device* dev, transfer_request& req) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_submit_transfer_async(xdev, req);
}

int32_t await_transfer(transfer_request& req) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(req.lock);
        while (req.pending) {
            irq = sync::wait(req.complete_wq, req.lock, irq);
        }
        sync::spin_unlock_irqrestore(req.lock, irq);
    });
    return static_cast<int32_t>(req.status);
}

void cancel_transfer(device* dev, transfer_request& req) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    hcd->usb_cancel_transfer(xdev, req);
}

int32_t open_interrupt_in_stream(device* dev,
                                 uint8_t endpoint_addr,
                                 uint32_t payload_length,
                                 interrupt_in_stream** out_stream) {
    if (!out_stream) {
        return -1;
    }
    *out_stream = nullptr;

    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    int32_t rc = hcd->usb_open_interrupt_in_stream(
        xdev, endpoint_addr, payload_length);
    if (rc != 0) {
        return rc;
    }

    auto* stream = heap::ualloc_new<interrupt_in_stream>();
    if (!stream) {
        hcd->usb_close_interrupt_in_stream(xdev, endpoint_addr);
        return -1;
    }
    stream->dev = dev;
    stream->endpoint_addr = endpoint_addr;
    *out_stream = stream;
    return 0;
}

int32_t read_interrupt_in_stream(interrupt_in_stream* stream,
                                 void* buffer,
                                 uint32_t buffer_len,
                                 uint32_t* out_length) {
    if (!stream || !stream->dev || !stream->dev->hcd || !stream->dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(stream->dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(stream->dev->hcd_device);
    return hcd->usb_read_interrupt_in_stream(
        xdev, stream->endpoint_addr, buffer, buffer_len, out_length);
}

void close_interrupt_in_stream(interrupt_in_stream* stream) {
    if (!stream) {
        return;
    }

    if (stream->dev && stream->dev->hcd && stream->dev->hcd_device) {
        auto* hcd = static_cast<drivers::xhci_hcd*>(stream->dev->hcd);
        auto* xdev = static_cast<drivers::xhci::xhci_device*>(stream->dev->hcd_device);
        hcd->usb_close_interrupt_in_stream(xdev, stream->endpoint_addr);
    }

    heap::ufree_delete(stream);
}

int32_t control_transfer(device* dev,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index,
                         void* data, uint16_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_control_transfer(xdev, request_type, request,
                                     value, index, data, length);
}

int32_t interrupt_transfer(device* dev, uint8_t endpoint_addr,
                           void* buffer, uint32_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device || (!buffer && length > 0)) {
        return -1;
    }

    if ((endpoint_addr & 0x80u) != 0) {
        string::memset(buffer, 0, length);
    }

    transfer_request req = {};
    init_transfer_request(req, endpoint_addr, buffer, length,
                          (endpoint_addr & 0x80u) ? allow_short : 0);
    int32_t rc = submit_transfer_async(dev, req);
    if (rc != 0) {
        return rc;
    }

    rc = await_transfer(req);
    if (rc == static_cast<int32_t>(transfer_status::ok) ||
        rc == static_cast<int32_t>(transfer_status::short_packet)) {
        return 0;
    }
    return rc;
}

int32_t bulk_transfer(device* dev, uint8_t endpoint_addr,
                      void* buffer, uint32_t length) {
    if (!dev || !dev->hcd || !dev->hcd_device || (!buffer && length > 0)) {
        return -1;
    }

    auto* hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    auto* xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    return hcd->usb_submit_transfer(xdev, endpoint_addr, buffer, length);
}

} // namespace usb
