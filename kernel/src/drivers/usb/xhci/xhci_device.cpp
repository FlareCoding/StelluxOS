#include "xhci_device.h"

uint16_t getInitialPacketSizeFromPortSpeed(uint8_t speed) {
    // Calculate initial max packet size for the set device command
    uint16_t initialMaxPacketSize = 0;
    switch (speed) {
    case XHCI_USB_SPEED_LOW_SPEED: initialMaxPacketSize = 8; break;

    case XHCI_USB_SPEED_FULL_SPEED:
    case XHCI_USB_SPEED_HIGH_SPEED: initialMaxPacketSize = 64; break;

    case XHCI_USB_SPEED_SUPER_SPEED:
    case XHCI_USB_SPEED_SUPER_SPEED_PLUS:
    default: initialMaxPacketSize = 512; break;
    }

    return initialMaxPacketSize;
}

XhciDevice::XhciDevice(XhciHcContext* xhc) {
    _allocInputContext(xhc);
}

XhciDevice::XhciDevice(XhciHcContext* xhc, uint8_t port) : port(port) {
    _allocInputContext(xhc);
}

void XhciDevice::setupTransferRing() {
    controlEpTransferRing = XhciTransferRing::allocate(slotId);
}

void XhciDevice::setupAddressDeviceCtx(uint8_t portSpeed) {
    auto& inputControlCtx = inputContext32.virtualBase->controlContext;
    auto& inputDeviceCtx = inputContext32.virtualBase->deviceContext;

    // Setup the input control context
    inputControlCtx.addFlags = (1 << 0) | (1 << 1);
    inputControlCtx.dropFlags = 0;

    // Setup the slot context
    auto& slotContext = inputDeviceCtx.slotContext;
    slotContext.contextEntries = 1;
    slotContext.speed = portSpeed;
    slotContext.rootHubPortNum = port;
    slotContext.routeString = 0;
    slotContext.interrupterTarget = 0;

    // Setup the control endpoint context
    auto& ctrlEpContext = inputDeviceCtx.controlEndpointContext;
    ctrlEpContext.endpointState = XHCI_ENDPOINT_STATE_DISABLED;
    ctrlEpContext.endpointType = XHCI_ENDPOINT_TYPE_CONTROL;
    ctrlEpContext.interval = 0;
    ctrlEpContext.errorCount = 3;
    ctrlEpContext.maxPacketSize = getInitialPacketSizeFromPortSpeed(portSpeed);
    ctrlEpContext.maxEsitPayloadLo = 0;
    ctrlEpContext.maxEsitPayloadHi = 0;
    ctrlEpContext.averageTrbLength = 8;
    ctrlEpContext.transferRingDequeuePtr = controlEpTransferRing->getPhysicalBase();
    ctrlEpContext.dcs = 1;
}

uint64_t XhciDevice::getInputContextPhysicalBase() {
    // return inputContext64.physicalBase ? inputContext64.physicalBase : inputContext32.physicalBase;
    return inputContext32.physicalBase;
}

void XhciDevice::_allocInputContext(XhciHcContext* xhc) {
    if (xhc->has64ByteContextSize()) {
        inputContext64 = xhciAllocDma<XhciInputContext64>(
            sizeof(XhciInputContext64),
            XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
            XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
        );
    } else {
        inputContext32 = xhciAllocDma<XhciInputContext32>(
            sizeof(XhciInputContext32),
            XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
            XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
        );
    }
}
