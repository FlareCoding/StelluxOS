#ifndef STELLUX_SMP_IPI_H
#define STELLUX_SMP_IPI_H

#include "common/types.h"

namespace smp {
namespace ipi {

constexpr int32_t OK          = 0;
constexpr int32_t ERR_INVALID = -1; // null handler, or a message that was never registered
constexpr int32_t ERR_FULL    = -2; // every message slot is taken
constexpr int32_t ERR_OFFLINE = -3; // the target CPU is not online
constexpr int32_t ERR_UNREACHABLE = -4; // the interrupt controller cannot address the target CPU

// One bit per message in a CPU's pending word
constexpr uint32_t MAX_MESSAGES = 32;

/**
 * An inter-processor interrupt asks another CPU to run a handler. The reason
 * for the interrupt is a `message`, and each message belongs to exactly one
 * subsystem, which registers it once during boot and keeps the returned value
 * to send it later.
 * Messages carry no payload and coalesce: a message sent to a CPU several
 * times before that CPU services its interrupt runs its handler once. A
 * subsystem that needs more than the fact of the interrupt must publish that
 * state itself before sending.
 * Handlers run on the receiving CPU in interrupt context with interrupts
 * masked. A handler must be short, must not take locks, and must never wait
 * for another CPU, since that CPU may be waiting for this one.
 */
struct message {
    uint32_t bit;
};

using handler = void (*)();

/**
 * @brief Set up delivery on the bootstrap CPU. Call after `irq::init`.
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Enable delivery on the calling AP. Call after `irq::init_ap`.
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_ap();

/**
 * @brief Bind `fn` to a new message. A subsystem calls this once, before any
 * CPU may send the message.
 * @param fn Handler run on the receiving CPU.
 * @param out Receives the message to pass to `send`.
 * @return OK on success, ERR_INVALID for a null argument, ERR_FULL when no
 *         slot is left.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t register_message(handler fn, message* out);

/**
 * @brief Interrupt one CPU with `m`. The calling CPU may target itself, the
 * handler then runs once it enables interrupts.
 * @param cpu Logical CPU id.
 * @return OK on success, ERR_INVALID for an unregistered message,
 *         ERR_OFFLINE when `cpu` is unknown or not online, ERR_UNREACHABLE
 *         when the interrupt controller cannot address it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t send(uint32_t cpu, message m);

/**
 * @brief Interrupt every online CPU except the caller with `m`.
 * @return The number of CPUs interrupted, which is the number of handler
 *         runs the caller may wait for.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t send_all_but_self(message m);

/**
 * @brief Run the handler of every message pending on the calling CPU. Called
 * by the architecture's interrupt entry when the IPI vector fires.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dispatch();

} // namespace ipi
} // namespace smp

#endif // STELLUX_SMP_IPI_H
