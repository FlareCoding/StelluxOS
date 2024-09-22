#include "xhci_device_ctx.h"

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
    m_controlEndpointTransferRing = new XhciTransferRing(XHCI_TRANSFER_RING_TRB_COUNT, slotId);
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
