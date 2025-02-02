#include <modules/usb/xhci/xhci_usb_hid_driver.h>
#include <time/time.h>

void xhci_usb_hid_driver::on_startup(xhci_hcd* hcd, xhci_device* dev) {
    this->on_device_init();

    _request_hid_report(hcd, dev);
}

void xhci_usb_hid_driver::on_event(xhci_hcd* hcd, xhci_device* dev) {
    auto& endpoint = m_interface->endpoints[0];
    uint8_t* data = endpoint->get_data_buffer();

    // Delegate specific data handling logic to child class drivers
    this->on_device_event(data);

    _request_hid_report(hcd, dev);
}

void xhci_usb_hid_driver::_request_hid_report(xhci_hcd* hcd, xhci_device* dev) {
    auto endpoint = m_interface->endpoints[0];
    auto transfer_ring = endpoint->get_transfer_ring();

    xhci_normal_trb_t normal_trb;
    zeromem(&normal_trb, sizeof(xhci_normal_trb_t));
    normal_trb.trb_type = XHCI_TRB_TYPE_NORMAL;
    normal_trb.data_buffer_physical_base = endpoint->get_data_buffer_dma();
    normal_trb.trb_transfer_length = endpoint->max_packet_size;
    normal_trb.ioc = 1;

    transfer_ring->enqueue(reinterpret_cast<xhci_trb_t*>(&normal_trb));
    hcd->ring_doorbell(dev->get_slot_id(), endpoint->xhc_endpoint_num);
}
