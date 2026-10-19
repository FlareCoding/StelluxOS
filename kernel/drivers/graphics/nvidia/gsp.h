// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_H

#include "common/types.h"
#include "drivers/graphics/nvidia/cpu_sequencer.h"
#include "drivers/graphics/nvidia/message_queues.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

/**
 * The driver's side of its conversation with the GSP's firmware (the GSP is
 * the processor inside the GPU that runs NVIDIA's firmware). Every message the
 * driver sends or receives goes through this object. It keeps the message
 * queues through which the driver and the firmware exchange messages, the
 * buffer each received message's payload is copied into, the context taken by
 * the steps of the CPU sequencer (the register, delay and falcon steps the
 * firmware sends for the driver to run), and a view of BAR0 for those steps
 * and for telling the firmware that a request waits.
 *
 * A message that needs no reply, such as either of the two messages queued
 * before the GSP starts, is queued with `send`. A request goes through
 * `request`, the one path every request takes: the request is queued, the
 * firmware is told that the request waits, the reply is awaited as
 * `receive_awaited` describes (a CPU sequencer the firmware sends first runs
 * as soon as its message is read) and handed to the caller, with as much of
 * its payload as the caller has room for copied over the request's arguments,
 * and its result is translated with `to_error`, the one translation of the
 * firmware's status codes into this driver's errors. A message the firmware
 * sends unasked, such as its ready message, is awaited with `await`.
 *
 *   driver --- send, request ---> command queue ---> GSP's firmware
 *   driver <-- await, request --- status queue  <--- GSP's firmware
 *
 * The object owns the payload buffer, which `stop` and the destructor free,
 * but not the queues' memory, which must stay allocated for as long as
 * messages are sent or received through the object, nor BAR0's mapping, which
 * must stay mapped until `stop`. Its waits spin on the calling CPU without
 * sleeping, and only one thread may use it at a time.
 */
class gsp {
public:
    /**
     * @brief An object holding no queues, for a driver to keep until it lays
     * the queues out. No call but `start` and `stop` may be made on it until
     * `start` returns `OK`.
     */
    gsp() = default;

    ~gsp();

    gsp(const gsp&) = delete;
    gsp& operator=(const gsp&) = delete;

    /**
     * @brief Allocate the payload buffer, room for `MESSAGE_PAYLOAD_MAX_BYTES`
     * on the unprivileged heap, lay the message queues out in `queue_memory`
     * as `message_queues` does before the firmware starts, and keep `regs` and
     * `sequencer` for the requests and the CPU sequencer's runs. The object
     * must hold no queues, as the default constructor and `stop` leave it.
     * @param regs A view of BAR0, kept until `stop`, for telling the firmware
     * that a request waits and for the sequencer's steps. Booter load must
     * have run on SEC2's falcon before the first wait, with nothing resetting
     * SEC2 since, as `receive_awaited` requires.
     * @param queue_memory The CPU's mapping of the queues' memory, as the
     * `message_queues` constructor requires it.
     * @param queue_dma_address The address the card reaches that memory at, as
     * the `message_queues` constructor requires it.
     * @param sequencer What the CPU sequencer's steps take. Its
     * `register_bytes` must be the size of the mapping `regs` views, and its
     * `deadline_ns` is ignored, since each wait sets its own as `await`
     * describes.
     * @return `OK`, or `ERR_NO_MEMORY`, which is logged, when the payload
     * buffer cannot be allocated. That error writes nothing and leaves the
     * object holding no queues.
     */
    int32_t start(
        registers& regs,
        uint8_t* queue_memory,
        uint64_t queue_dma_address,
        const cpu_sequencer_context& sequencer
    );

    /**
     * @brief Queue a message for the firmware with no reply awaited, as
     * `message_queues::send` does. Like `message_queues::send`, it touches
     * none of the card's registers, so after `OK` the caller must tell the
     * firmware that a message waits.
     * @param function NVIDIA's number for what the message asks of the
     * firmware.
     * @param payload The bytes the message carries after its headers, which
     * may be null when `payload_bytes` is 0.
     * @param payload_bytes How many bytes `payload` holds, refused above
     * `MESSAGE_PAYLOAD_MAX_BYTES`.
     * @return What `message_queues::send` returns, `ERR_TOO_LARGE` logged and
     * `ERR_QUEUE_FULL` not.
     */
    int32_t send(uint32_t function, const void* payload, uint32_t payload_bytes);

    /**
     * @brief Link the status queue once the firmware has written its header,
     * as `link_status_queue_within` does.
     * @param timeout_ns How long to retry, `STATUS_QUEUE_LINK_TIMEOUT_NS` at
     * bring-up.
     * @return `OK`, or the `ERR_TIMEOUT` `link_status_queue_within` logs and
     * returns.
     */
    int32_t link_status_queue(uint64_t timeout_ns);

    /**
     * @brief Wait for the next message carrying `function`, as
     * `receive_awaited` waits, with a deadline `timeout_ns` after the call.
     * The CPU sequencer's waits other than its delays and the ready waits of
     * its falcon steps count against a deadline `DEFAULT_TIMEOUT_NS` after the
     * call, as NVIDIA's driver starts its thread-state budget again just
     * before its wait for the ready message. The status queue must be linked.
     * @param function NVIDIA's number for the message awaited.
     * @param out Receives the message's function, results and payload size,
     * which never exceeds `MESSAGE_PAYLOAD_MAX_BYTES`, on `OK`, and is left
     * unchanged on an error.
     * @param timeout_ns How long to wait, `RECEIVE_TIMEOUT_NS` at bring-up.
     * @return What `receive_awaited` returns, logged as its doc describes.
     */
    int32_t await(uint32_t function, received_message& out, uint64_t timeout_ns);

    /**
     * @brief Send the firmware a request and wait for its reply: queue the
     * request, tell the firmware that the request waits, wait for the reply
     * (the next message carrying `function`) as `await` waits but with both
     * deadlines counted from before the request is queued, hand the caller the
     * reply's fields and as much of its payload as `reply_capacity` allows,
     * copied over `arguments`, and translate the reply's result with
     * `to_error`. Nothing is logged between the queueing and the wait, and the
     * wait runs a CPU sequencer the firmware sends ahead of the reply as soon
     * as its message is read, as `receive_awaited` describes. The status queue
     * must be linked. NVIDIA's driver does not start its thread-state budget
     * again before a request, so the sequencer's waits other than its delays
     * and the ready waits of its falcon steps share what is left of a budget
     * begun earlier, while here they get one of their own. NVIDIA's driver
     * also skips a request when its GSP has crashed or the GPU is in reset,
     * lost, shut down, below full power or without access to system memory,
     * and waits up to 1 s for room for each element the request fills in the
     * command queue, whereas `request` fails at once on a full queue.
     * @param function NVIDIA's number for what the request asks, which the
     * reply carries too.
     * @param arguments The buffer whose first `argument_bytes` bytes the
     * request carries after its headers and over which the reply's payload is
     * copied once the reply arrives, up to `reply_capacity` bytes. It may be
     * null when `argument_bytes` and `reply_capacity` are 0.
     * @param argument_bytes How many bytes of `arguments` the request carries,
     * refused above `MESSAGE_PAYLOAD_MAX_BYTES`.
     * @param reply_capacity How many bytes of the reply's payload `arguments`
     * has room for, 0 for a caller that reads none of it. A longer payload is
     * cut to its first `reply_capacity` bytes and a shorter one leaves the
     * bytes past it as they were, so a caller must check that
     * `reply.payload_bytes` covers what it reads.
     * @param reply Receives the reply's function, results and payload size,
     * which never exceeds `MESSAGE_PAYLOAD_MAX_BYTES`, once the reply arrives,
     * whatever its result, and is left unchanged otherwise. Only the result is
     * checked here, as NVIDIA's driver checks it for every request, so a
     * caller whose request also reports through the private result checks that
     * itself.
     * @param timeout_ns How long to wait for the reply, `RECEIVE_TIMEOUT_NS`
     * in the driver.
     * @return `OK` once the reply's result is NVIDIA's success value, or what
     * `message_queues::send` returns when the request cannot be queued, which
     * is logged, what `receive_awaited` returns when its wait ends without the
     * reply, logged as its doc describes, or what `to_error` returns for the
     * reply's result, logged with the status.
     */
    int32_t request(
        uint32_t function,
        void* arguments,
        uint32_t argument_bytes,
        uint32_t reply_capacity,
        received_message& reply,
        uint64_t timeout_ns
    );

    /**
     * @brief The driver's error for a status code the firmware reports in a
     * reply or in its ready message. NVIDIA's driver hands the status of a
     * failed request up to the request's caller when the status lies below
     * 0xFF000000, and NV_ERR_GENERIC otherwise. Whatever their value, it
     * hands up the status of a failed ready message and, unless it sends the
     * request again, the private result of a reply refusing the driver's VGX
     * version (the version of NVIDIA's messages the driver builds). Here
     * every failure is the one error, and the callers log the status.
     * @param status The result field of a reply or of the ready message, or
     * the private result of a reply whose request reports through it.
     * @return `OK` for NVIDIA's success value 0, and `ERR_FIRMWARE` for every
     * other status, which in a reply's result reports that the request
     * failed, in a private result that the firmware refuses what the request
     * asked, and in the ready message that the firmware's initialization
     * failed.
     */
    static int32_t to_error(uint32_t status);

    /**
     * @brief Free the payload buffer and drop the queues, the view of BAR0 and
     * the sequencer's context, leaving the object as the default constructor
     * makes it. The queues' memory is left as it is.
     */
    void stop();

private:
    int32_t await_since(uint32_t function, received_message& out, uint64_t start_ns, uint64_t timeout_ns);

    message_queues        m_queues;
    registers             m_regs{0};
    cpu_sequencer_context m_sequencer = {};
    uint8_t*              m_payload = nullptr;
};

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_GSP_H
