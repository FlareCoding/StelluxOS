#include "drivers/usb/xhci/xhci_device.h"
#include "drivers/usb/xhci/xhci_mem.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "common/string.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "sync/spinlock.h"

namespace drivers::xhci {

int32_t xhci_device::init(uint8_t port_id, uint8_t slot_id, uint8_t speed, bool csz) {
    m_port_id = port_id;
    m_slot_id = slot_id;
    m_speed = speed;
    m_csz = csz;

    m_ctrl_completion_wq.init();
    m_ctrl_completion_lock = sync::SPINLOCK_INIT;

    // Allocate input context (DMA)
    size_t input_ctx_size = csz ? sizeof(xhci_input_context64) : sizeof(xhci_input_context32);
    m_input_ctx = alloc_xhci_memory(input_ctx_size);
    if (!m_input_ctx) {
        log::error("xhci: failed to allocate input context for slot %u", slot_id);
        return -1;
    }
    m_input_ctx_phys = xhci_get_physical_addr(m_input_ctx);

    // Allocate a persistent DMA page for control transfer payloads.
    m_ctrl_transfer_buffer = alloc_xhci_memory(paging::PAGE_SIZE_4KB);
    if (!m_ctrl_transfer_buffer) {
        log::error("xhci: failed to allocate control transfer buffer for slot %u", slot_id);
        free_xhci_memory(m_input_ctx);
        m_input_ctx = nullptr;
        return -1;
    }
    m_ctrl_transfer_buffer_phys = xhci_get_physical_addr(m_ctrl_transfer_buffer);

    // Allocate and init the control transfer ring
    m_ctrl_ring = heap::ualloc_new<xhci_transfer_ring>();
    if (!m_ctrl_ring) {
        log::error("xhci: failed to allocate control transfer ring for slot %u", slot_id);
        free_xhci_memory(m_ctrl_transfer_buffer);
        m_ctrl_transfer_buffer = nullptr;
        free_xhci_memory(m_input_ctx);
        m_input_ctx = nullptr;
        return -1;
    }

    if (m_ctrl_ring->init(XHCI_TRANSFER_RING_TRB_COUNT, slot_id) != 0) {
        log::error("xhci: failed to init control transfer ring for slot %u", slot_id);
        heap::ufree_delete(m_ctrl_ring);
        m_ctrl_ring = nullptr;
        free_xhci_memory(m_ctrl_transfer_buffer);
        m_ctrl_transfer_buffer = nullptr;
        free_xhci_memory(m_input_ctx);
        m_input_ctx = nullptr;
        return -1;
    }

    return 0;
}

void xhci_device::destroy() {
    // Destroy non-control endpoints
    for (uint8_t i = 0; i <= MAX_ENDPOINTS; i++) {
        if (m_endpoints[i]) {
            m_endpoints[i]->destroy();
            heap::ufree_delete(m_endpoints[i]);
            m_endpoints[i] = nullptr;
        }
    }

    if (m_ctrl_ring) {
        m_ctrl_ring->destroy();
        heap::ufree_delete(m_ctrl_ring);
        m_ctrl_ring = nullptr;
    }
    if (m_ctrl_transfer_buffer) {
        free_xhci_memory(m_ctrl_transfer_buffer);
        m_ctrl_transfer_buffer = nullptr;
        m_ctrl_transfer_buffer_phys = 0;
    }
    if (m_input_ctx) {
        free_xhci_memory(m_input_ctx);
        m_input_ctx = nullptr;
    }
    m_output_ctx = nullptr;
    m_core_device = nullptr;
}

xhci_input_control_context32* xhci_device::input_ctrl_ctx() {
    if (m_csz) {
        auto* ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_input_control_context32*>(&ctx->control_context);
    }
    auto* ctx = static_cast<xhci_input_context32*>(m_input_ctx);
    return &ctx->control_context;
}

xhci_slot_context32* xhci_device::input_slot_ctx() {
    if (m_csz) {
        auto* ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_slot_context32*>(&ctx->device_context.slot_context);
    }
    auto* ctx = static_cast<xhci_input_context32*>(m_input_ctx);
    return &ctx->device_context.slot_context;
}

xhci_endpoint_context32* xhci_device::input_ctrl_ep_ctx() {
    if (m_csz) {
        auto* ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_endpoint_context32*>(&ctx->device_context.control_ep_context);
    }
    auto* ctx = static_cast<xhci_input_context32*>(m_input_ctx);
    return &ctx->device_context.control_ep_context;
}

xhci_endpoint_context32* xhci_device::input_ep_ctx(uint8_t ep_num) {
    uint8_t ep_index = ep_num - 2;
    if (m_csz) {
        auto* ctx = static_cast<xhci_input_context64*>(m_input_ctx);
        return reinterpret_cast<xhci_endpoint_context32*>(&ctx->device_context.ep[ep_index]);
    }
    auto* ctx = static_cast<xhci_input_context32*>(m_input_ctx);
    return &ctx->device_context.ep[ep_index];
}

void xhci_device::sync_input_ctx() {
    if (!m_output_ctx) return;

    if (m_csz) {
        auto* input = static_cast<xhci_input_context64*>(m_input_ctx);
        string::memcpy(&input->device_context, m_output_ctx, sizeof(xhci_device_context64));
    } else {
        auto* input = static_cast<xhci_input_context32*>(m_input_ctx);
        string::memcpy(&input->device_context, m_output_ctx, sizeof(xhci_device_context32));
    }
}

} // namespace drivers::xhci
