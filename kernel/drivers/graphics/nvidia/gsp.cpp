// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// NVIDIA's NV_OK, the status the firmware reports for what succeeded
constexpr uint32_t FIRMWARE_STATUS_OK = 0;

gsp::~gsp() {
    stop();
}

int32_t gsp::start(
    registers& regs,
    uint8_t* queue_memory,
    uint64_t queue_dma_address,
    const cpu_sequencer_context& sequencer
) {
    uint8_t* payload = static_cast<uint8_t*>(heap::ualloc(MESSAGE_PAYLOAD_MAX_BYTES));
    if (payload == nullptr) {
        log::error(
            "nvidia: allocating %u bytes for the payloads of the firmware's messages failed",
            MESSAGE_PAYLOAD_MAX_BYTES
        );
        return ERR_NO_MEMORY;
    }

    m_payload = payload;
    m_queues = message_queues(queue_memory, queue_dma_address);
    m_regs = regs;
    m_sequencer = sequencer;

    return OK;
}

int32_t gsp::send(uint32_t function, const void* payload, uint32_t payload_bytes) {
    return m_queues.send(function, payload, payload_bytes);
}

int32_t gsp::link_status_queue(uint64_t timeout_ns) {
    return link_status_queue_within(m_queues, timeout_ns);
}

int32_t gsp::await(uint32_t function, received_message& out, uint64_t timeout_ns) {
    return await_since(function, out, clock::now_ns(), timeout_ns);
}

int32_t gsp::request(uint32_t function, const void* arguments, uint32_t argument_bytes, uint64_t timeout_ns) {
    uint64_t start_ns = clock::now_ns();

    int32_t rc = m_queues.send(function, arguments, argument_bytes);
    if (rc != OK) {
        log::error("nvidia: queueing the request of function 0x%X for the GSP's firmware failed: %d", function, rc);
        return rc;
    }

    notify_gsp_of_queued_messages(m_regs);

    received_message reply = {};
    rc = await_since(function, reply, start_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = to_error(reply.result);
    if (rc != OK) {
        log::error(
            "nvidia: the GSP's firmware reports that the request of function 0x%X failed, with status 0x%X",
            function,
            reply.result
        );
    }

    return rc;
}

int32_t gsp::to_error(uint32_t status) {
    if (status != FIRMWARE_STATUS_OK) {
        return ERR_FIRMWARE;
    }

    return OK;
}

void gsp::stop() {
    if (m_payload != nullptr) {
        heap::ufree(m_payload);
        m_payload = nullptr;
    }

    m_queues = message_queues();
    m_regs = registers(0);
    m_sequencer = {};
}

int32_t gsp::await_since(uint32_t function, received_message& out, uint64_t start_ns, uint64_t timeout_ns) {
    m_sequencer.deadline_ns = start_ns + DEFAULT_TIMEOUT_NS;

    return receive_awaited(
        m_queues,
        m_regs,
        m_sequencer,
        function,
        out,
        m_payload,
        MESSAGE_PAYLOAD_MAX_BYTES,
        start_ns + timeout_ns
    );
}

} // namespace drivers::nvidia
