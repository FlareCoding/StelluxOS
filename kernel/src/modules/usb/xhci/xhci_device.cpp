#include <modules/usb/xhci/xhci_device.h>
#include <memory/paging.h>

uint8_t get_endpoint_type_from_ep_descriptor(usb_endpoint_descriptor* desc) {
    uint8_t endpoint_direction_in = (desc->bEndpointAddress & 0x80) ? 1 : 0;
    uint8_t transfer_type = desc->bmAttributes & 0x3;

    switch (transfer_type) {
    case 0: {
        return XHCI_ENDPOINT_TYPE_CONTROL;
    }
    case 1: {
        return endpoint_direction_in ? XHCI_ENDPOINT_TYPE_ISOCHRONOUS_IN : XHCI_ENDPOINT_TYPE_ISOCHRONOUS_OUT;
    }
    case 2: {
        return endpoint_direction_in ? XHCI_ENDPOINT_TYPE_BULK_IN : XHCI_ENDPOINT_TYPE_BULK_OUT;
    }
    case 3: {
        return endpoint_direction_in ? XHCI_ENDPOINT_TYPE_INTERRUPT_IN : XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;
    }
    default: break;
    }

    return 0;
}

xhci_device_endpoint_descriptor::xhci_device_endpoint_descriptor(uint8_t slot_id, usb_endpoint_descriptor* desc) : slot_id(slot_id) {
    uint8_t endpoint_number_base = desc->bEndpointAddress & 0x0F;
    uint8_t endpoint_direction_in = (desc->bEndpointAddress & 0x80) ? 1 : 0;

    endpoint_num = (endpoint_number_base * 2) + endpoint_direction_in;
    endpoint_type = get_endpoint_type_from_ep_descriptor(desc);
    max_packet_size = desc->wMaxPacketSize;
    interval = desc->bInterval;
    
    data_buffer = (uint8_t*)alloc_xhci_memory(desc->wMaxPacketSize, 64, 64);
    transfer_ring = xhci_transfer_ring::allocate(slot_id);
}

xhci_device_endpoint_descriptor::~xhci_device_endpoint_descriptor() {
    free_xhci_memory(data_buffer);
}

void xhci_device::allocate_input_context(bool use_64byte_contexts) {
    // Calculate the input context size based
    // on the capability register parameters.
    uint64_t input_context_size = use_64byte_contexts ? sizeof(xhci_input_context64) : sizeof(xhci_input_context32);

    // Allocate and zero out the input context
    m_input_context = alloc_xhci_memory(
        input_context_size,
        XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT,
        XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY
    );
}

uint64_t xhci_device::get_input_context_physical_base() {
    return xhci_get_physical_addr(m_input_context);
}

void xhci_device::allocate_control_ep_transfer_ring() {
    m_control_endpoint_transfer_ring = xhci_transfer_ring::allocate(slot_id);
}

xhci_input_control_context32* xhci_device::get_input_control_context(bool use_64byte_contexts) {
    if (use_64byte_contexts) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_context);
        return reinterpret_cast<xhci_input_control_context32*>(&input_ctx->control_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_context);
        return &input_ctx->control_context;
    }
}

xhci_slot_context32* xhci_device::get_input_slot_context(bool use_64byte_contexts) {
    if (use_64byte_contexts) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_context);
        return reinterpret_cast<xhci_slot_context32*>(&input_ctx->device_context.slot_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_context);
        return &input_ctx->device_context.slot_context;
    }
}

xhci_endpoint_context32* xhci_device::get_input_control_ep_context(bool use_64byte_contexts) {
    if (use_64byte_contexts) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_context);
        return reinterpret_cast<xhci_endpoint_context32*>(&input_ctx->device_context.control_ep_context);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_context);
        return &input_ctx->device_context.control_ep_context;
    }
}

xhci_endpoint_context32* xhci_device::get_input_ep_context(bool use_64byte_contexts, uint8_t endpoint_id) {
    uint8_t endpoint_index = endpoint_id - 2;

    if (use_64byte_contexts) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_context);
        return reinterpret_cast<xhci_endpoint_context32*>(&input_ctx->device_context.ep[endpoint_index]);
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_context);
        return &input_ctx->device_context.ep[endpoint_index];
    }
}

void xhci_device::copy_output_device_context_to_input_device_context(bool use_64byte_contexts, void* output_device_context) {
    if (use_64byte_contexts) {
        xhci_input_context64* input_ctx = static_cast<xhci_input_context64*>(m_input_context);
        xhci_device_context64* input_device_ctx = &input_ctx->device_context;
        memcpy(input_device_ctx, output_device_context, sizeof(xhci_device_context64));
    } else {
        xhci_input_context32* input_ctx = static_cast<xhci_input_context32*>(m_input_context);
        xhci_device_context32* input_device_ctx = &input_ctx->device_context;
        memcpy(input_device_ctx, output_device_context, sizeof(xhci_device_context32));
    }
}
