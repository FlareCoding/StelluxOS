#include "xhci_device.h"

uint8_t getEndpointTypeFromEpDescriptor(UsbEndpointDescriptor* desc) {
    uint8_t endpointDirectionIn = (desc->bEndpointAddress & 0x80) ? 1 : 0;
    uint8_t transferType = desc->bmAttributes & 0x3;

    switch (transferType) {
    case 0: {
        return XHCI_ENDPOINT_TYPE_CONTROL;
    }
    case 1: {
        return endpointDirectionIn ? XHCI_ENDPOINT_TYPE_ISOCHRONOUS_IN : XHCI_ENDPOINT_TYPE_ISOCHRONOUS_OUT;
    }
    case 2: {
        return endpointDirectionIn ? XHCI_ENDPOINT_TYPE_BULK_IN : XHCI_ENDPOINT_TYPE_BULK_OUT;
    }
    case 3: {
        return endpointDirectionIn ? XHCI_ENDPOINT_TYPE_INTERRUPT_IN : XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;
    }
    default: break;
    }

    return 0;
}

XhciDeviceEndpointDescriptor::XhciDeviceEndpointDescriptor(uint8_t slotId, UsbEndpointDescriptor* desc) : slotId(slotId) {
    uint8_t endpointNumberBase = desc->bEndpointAddress & 0x0F;
    uint8_t endpointDirectionIn = (desc->bEndpointAddress & 0x80) ? 1 : 0;

    endpointNum = (endpointNumberBase * 2) + endpointDirectionIn;
    endpointType = getEndpointTypeFromEpDescriptor(desc);
    maxPacketSize = desc->wMaxPacketSize;
    interval = desc->bInterval;
    
    dataBuffer = (uint8_t*)allocXhciMemory(desc->wMaxPacketSize, 64, 64);
    transferRing = XhciTransferRing::allocate(slotId);
}

XhciDeviceEndpointDescriptor::~XhciDeviceEndpointDescriptor() {
    kfreeAligned(dataBuffer);
}

void XhciDevice::allocateInputContext(bool use64ByteContexts) {
    // Calculate the input context size based
    // on the capability register parameters.
    uint64_t inputContextSize = use64ByteContexts ? sizeof(XhciInputContext64) : sizeof(XhciInputContext32);

    // Allocate and zero out the input context
    m_inputContext = allocXhciMemory(
        inputContextSize,
        XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
        XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
    );
}

uint64_t XhciDevice::getInputContextPhysicalBase() {
    return physbase(m_inputContext);
}

void XhciDevice::allocateControlEndpointTransferRing() {
    m_controlEndpointTransferRing = XhciTransferRing::allocate(slotId);
}

XhciInputControlContext32* XhciDevice::getInputControlContext(bool use64ByteContexts) {
    if (use64ByteContexts) {
        XhciInputContext64* inputCtx = static_cast<XhciInputContext64*>(m_inputContext);
        return reinterpret_cast<XhciInputControlContext32*>(&inputCtx->controlContext);
    } else {
        XhciInputContext32* inputCtx = static_cast<XhciInputContext32*>(m_inputContext);
        return &inputCtx->controlContext;
    }
}

XhciSlotContext32* XhciDevice::getInputSlotContext(bool use64ByteContexts) {
    if (use64ByteContexts) {
        XhciInputContext64* inputCtx = static_cast<XhciInputContext64*>(m_inputContext);
        return reinterpret_cast<XhciSlotContext32*>(&inputCtx->deviceContext.slotContext);
    } else {
        XhciInputContext32* inputCtx = static_cast<XhciInputContext32*>(m_inputContext);
        return &inputCtx->deviceContext.slotContext;
    }
}

XhciEndpointContext32* XhciDevice::getInputControlEndpointContext(bool use64ByteContexts) {
    if (use64ByteContexts) {
        XhciInputContext64* inputCtx = static_cast<XhciInputContext64*>(m_inputContext);
        return reinterpret_cast<XhciEndpointContext32*>(&inputCtx->deviceContext.controlEndpointContext);
    } else {
        XhciInputContext32* inputCtx = static_cast<XhciInputContext32*>(m_inputContext);
        return &inputCtx->deviceContext.controlEndpointContext;
    }
}

XhciEndpointContext32* XhciDevice::getInputEndpointContext(bool use64ByteContexts, uint8_t endpointID) {
    uint8_t endpointIndex = endpointID - 2;

    if (use64ByteContexts) {
        XhciInputContext64* inputCtx = static_cast<XhciInputContext64*>(m_inputContext);
        return reinterpret_cast<XhciEndpointContext32*>(&inputCtx->deviceContext.ep[endpointIndex]);
    } else {
        XhciInputContext32* inputCtx = static_cast<XhciInputContext32*>(m_inputContext);
        return &inputCtx->deviceContext.ep[endpointIndex];
    }
}

void XhciDevice::copyOutputDeviceContextToInputDeviceContext(bool use64ByteContexts, void* outputDeviceContext) {
    if (use64ByteContexts) {
        XhciInputContext64* inputCtx = static_cast<XhciInputContext64*>(m_inputContext);
        XhciDeviceContext64* inputDeviceCtx = &inputCtx->deviceContext;
        memcpy(inputDeviceCtx, outputDeviceContext, sizeof(XhciDeviceContext64));
    } else {
        XhciInputContext32* inputCtx = static_cast<XhciInputContext32*>(m_inputContext);
        XhciDeviceContext32* inputDeviceCtx = &inputCtx->deviceContext;
        memcpy(inputDeviceCtx, outputDeviceContext, sizeof(XhciDeviceContext32));
    }
}
