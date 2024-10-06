#include "xhci_hid.h"
#include "hid_keyboard_driver.h"
#include "hid_mouse_driver.h"

#define HID_PROTOCOL_KEYBOARD  1
#define HID_PROTOCOL_MOUSE     2

XhciHidDriver::XhciHidDriver(XhciDoorbellManager* doorbellManager, XhciDevice* device) {
    m_doorbellManager = doorbellManager;
    m_device = device;

    switch (device->interfaceProtocol) {
    case HID_PROTOCOL_KEYBOARD: {
        m_hidDeviceDriver = new HidKeyboardDriver();
        break;
    }
    case HID_PROTOCOL_MOUSE: {
        m_hidDeviceDriver = new HidMouseDriver();
        break;
    }
    default: break;
    }
}

void XhciHidDriver::start() {
    _requestNextHidReport();
}

void XhciHidDriver::destroy() {}

void XhciHidDriver::handleEvent(void* evt) {
    __unused evt;

    if (m_hidDeviceDriver) {
        auto data = m_device->endpoints[0]->dataBuffer;
        m_hidDeviceDriver->handleEvent(data);
    }

    _requestNextHidReport();
}

void XhciHidDriver::_requestNextHidReport() {
    auto endpoint = m_device->endpoints[0];
    auto& transferRing = endpoint->transferRing;

    // Prepare a Normal TRB
    XhciNormalTrb_t normalTrb;
    zeromem(&normalTrb, sizeof(XhciNormalTrb_t));
    normalTrb.trbType = XHCI_TRB_TYPE_NORMAL;
    normalTrb.dataBufferPhysicalBase = physbase(endpoint->dataBuffer);
    normalTrb.trbTransferLength = endpoint->maxPacketSize;
    normalTrb.ioc = 1;

    transferRing->enqueue((XhciTrb_t*)&normalTrb);

    m_doorbellManager->ringDoorbell(m_device->slotId, endpoint->endpointNum);
}
