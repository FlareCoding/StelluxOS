#include <drivers/usb/xhci/xhci_device.h>

xhci_device::xhci_device(uint8_t port, uint8_t slot, uint8_t speed, bool use_64byte_ctx)
    : m_port_id(port), m_slot_id(slot), m_speed(speed), m_use64byte_ctx(use_64byte_ctx) {
    _alloc_input_ctx();
    _alloc_ctrl_ep_ring();
}

void xhci_device::_alloc_input_ctx() {
    // Calculate the input context size based
    // on the capability register parameters.
    uint64_t input_context_size = m_use64byte_ctx ? sizeof(xhci_input_context64) : sizeof(xhci_input_context32);

    // Allocate the input context memory
    m_input_ctx = alloc_xhci_memory(
        input_context_size,
        XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
        XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
    );

    m_input_ctx_dma_addr = xhci_get_physical_addr(m_input_ctx);
}

void xhci_device::_alloc_ctrl_ep_ring() {
    m_ctrl_ep_ring = xhci_transfer_ring::allocate(m_slot_id);
}

xhci_input_control_context32* xhci_device::get_input_ctrl_ctx() {
    if (m_use64byte_ctx) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_input_control_context32*>(&input_ctx->control_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_ctx);
        return &input_ctx->control_context;
    }
}

xhci_slot_context32* xhci_device::get_input_slot_ctx() {
    if (m_use64byte_ctx) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_slot_context32*>(&input_ctx->device_context.slot_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_ctx);
        return &input_ctx->device_context.slot_context;
    }
}

xhci_endpoint_context32* xhci_device::get_input_ctrl_ep_ctx() {
    if (m_use64byte_ctx) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_endpoint_context32*>(&input_ctx->device_context.control_ep_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_ctx);
        return &input_ctx->device_context.control_ep_context;
    }
}

xhci_endpoint_context32* xhci_device::get_input_ep_ctx(uint8_t endpoint_num) {
    uint8_t endpoint_index = endpoint_num - 2;

    if (m_use64byte_ctx) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_endpoint_context32*>(&input_ctx->device_context.ep[endpoint_index]);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_ctx);
        return &input_ctx->device_context.ep[endpoint_index];
    }
}

void xhci_device::sync_input_ctx(void* out_ctx) {
    if (m_use64byte_ctx) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        xhci_device_context64* input_device_ctx = &input_ctx->device_context;
        memcpy(input_device_ctx, out_ctx, sizeof(xhci_device_context64));
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_ctx);
        xhci_device_context32* input_device_ctx = &input_ctx->device_context;
        memcpy(input_device_ctx, out_ctx, sizeof(xhci_device_context32));
    }
}

void xhci_device::setup_add_interface(const usb_interface_descriptor* desc) {
    auto iface = kstl::make_shared<xhci_usb_interface>(m_slot_id, desc);
    interfaces.push_back(iface);
}
