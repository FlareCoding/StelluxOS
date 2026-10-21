// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"

namespace drivers::nvidia {

// NVIDIA's NV_OK, the status the firmware reports for what succeeded
constexpr uint32_t FIRMWARE_STATUS_OK = 0;

// NVIDIA's number for a control request, which the reply carries too
constexpr uint32_t FUNCTION_GSP_RM_CONTROL = 0x4C;

// The first two of the GSP's four mailbox registers (NVIDIA's NV_PGSP_MAILBOX(0) and (1), apart from the GSP falcon's
// two), where the firmware can report a failure, and the tag that marks a report in bits 31:28 of the first
constexpr uint32_t REPORT_MAILBOX        = 0x110804;
constexpr uint32_t REPORT_DETAIL_MAILBOX = 0x110808;
constexpr uint32_t ERROR_TAG             = 0xE;

// A report's fields: the code in bits 7:0, the reason in 15:8, the task in 23:16, the count of errors the firmware
// skipped in 27:24 and the tag in 31:28
constexpr uint32_t REPORT_BYTE_MASK     = 0xFF;
constexpr uint32_t REPORT_REASON_SHIFT  = 8;
constexpr uint32_t REPORT_TASK_SHIFT    = 16;
constexpr uint32_t REPORT_SKIPPED_SHIFT = 24;
constexpr uint32_t REPORT_SKIPPED_MASK  = 0xF;
constexpr uint32_t REPORT_TAG_SHIFT     = 28;

namespace {

// NVIDIA's header of a control request, which the control's parameters follow, its status filled by the reply
struct control_header {
    rm::handle client;
    rm::handle object;
    uint32_t   command;
    uint32_t   status;
    uint32_t   params_bytes;
    uint32_t   flags;
};

} // namespace

static_assert(sizeof(control_header) == 24, "NVIDIA's layout of a control request's header");

// The most bytes of parameters one message carries after a control request's header
constexpr uint32_t CONTROL_PARAMS_MAX_BYTES = MESSAGE_PAYLOAD_MAX_BYTES - sizeof(control_header);

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

int32_t gsp::drain() {
    m_sequencer.deadline_ns = clock::now_ns() + DEFAULT_TIMEOUT_NS;

    return drain_status_queue(m_queues, m_regs, m_sequencer, m_payload, MESSAGE_PAYLOAD_MAX_BYTES);
}

void gsp::check_health() {
    uint32_t report = m_regs.read(REPORT_MAILBOX);
    if (report >> REPORT_TAG_SHIFT != ERROR_TAG) {
        return;
    }

    uint32_t detail = m_regs.read(REPORT_DETAIL_MAILBOX);
    uint32_t code = report & REPORT_BYTE_MASK;
    uint32_t reason = (report >> REPORT_REASON_SHIFT) & REPORT_BYTE_MASK;
    uint32_t task = (report >> REPORT_TASK_SHIFT) & REPORT_BYTE_MASK;
    uint32_t skipped = (report >> REPORT_SKIPPED_SHIFT) & REPORT_SKIPPED_MASK;

    m_regs.write(REPORT_MAILBOX, 0);

    log::error(
        "nvidia: the GSP's firmware reports error code 0x%X for reason 0x%X in its task %u, with 0x%X in the GSP's "
        "second mailbox register, so the GPU likely needs a reset",
        code,
        reason,
        task,
        detail
    );

    if (skipped != 0) {
        log::error("nvidia: the GSP's firmware skipped reporting more errors, %u of them", skipped);
    }
}

int32_t gsp::request(
    uint32_t function,
    void* arguments,
    uint32_t argument_bytes,
    uint32_t reply_capacity,
    received_message& reply,
    uint64_t timeout_ns
) {
    uint64_t start_ns = clock::now_ns();

    int32_t rc = m_queues.send(function, arguments, argument_bytes);
    if (rc != OK) {
        log::error("nvidia: queueing the request of function 0x%X for the GSP's firmware failed: %d", function, rc);
        return rc;
    }

    notify_gsp_of_queued_messages(m_regs);

    received_message received = {};
    rc = await_since(function, received, start_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    uint32_t copied_bytes = received.payload_bytes < reply_capacity ? received.payload_bytes : reply_capacity;
    if (copied_bytes > 0) {
        string::memcpy(arguments, m_payload, copied_bytes);
    }

    reply = received;

    rc = to_error(received.result);
    if (rc != OK) {
        log::error(
            "nvidia: the GSP's firmware reports that the request of function 0x%X failed, with status 0x%X",
            function,
            received.result
        );
    }

    return rc;
}

int32_t gsp::control(
    rm::handle client,
    rm::handle object,
    uint32_t command,
    void* params,
    uint32_t params_bytes,
    uint64_t timeout_ns
) {
    if (params_bytes > CONTROL_PARAMS_MAX_BYTES) {
        log::error(
            "nvidia: the parameters of control 0x%X take %u bytes, more than the %u one message carries after the "
            "control's header",
            command,
            params_bytes,
            CONTROL_PARAMS_MAX_BYTES
        );
        return ERR_TOO_LARGE;
    }

    uint32_t message_bytes = sizeof(control_header) + params_bytes;
    uint8_t* message = static_cast<uint8_t*>(heap::ualloc(message_bytes));
    if (message == nullptr) {
        log::error("nvidia: allocating %u bytes for the request of control 0x%X failed", message_bytes, command);
        return ERR_NO_MEMORY;
    }

    control_header header = {
        .client       = client,
        .object       = object,
        .command      = command,
        .status       = 0,
        .params_bytes = params_bytes,
        .flags        = 0,
    };

    string::memcpy(message, &header, sizeof(header));
    string::memcpy(message + sizeof(header), params, params_bytes);

    received_message reply = {};
    int32_t rc = request(FUNCTION_GSP_RM_CONTROL, message, message_bytes, message_bytes, reply, timeout_ns);
    if (rc == OK && reply.payload_bytes < message_bytes) {
        log::error(
            "nvidia: the GSP's firmware replies to control 0x%X with %u payload bytes, ending before the control's "
            "parameters end at byte %u",
            command,
            reply.payload_bytes,
            message_bytes
        );
        rc = ERR_BAD_MESSAGE;
    }

    if (rc == OK) {
        string::memcpy(&header, message, sizeof(header));
        rc = to_error(header.status);
        if (rc != OK) {
            log::error(
                "nvidia: the GSP's firmware reports that control 0x%X on object 0x%X failed, with status 0x%X",
                command,
                object,
                header.status
            );
        }
    }

    if (rc == OK) {
        string::memcpy(params, message + sizeof(header), params_bytes);
    }

    heap::ufree(message);

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
