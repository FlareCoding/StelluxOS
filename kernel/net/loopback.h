#ifndef STELLUX_NET_LOOPBACK_H
#define STELLUX_NET_LOOPBACK_H

#include "net/interface.h"
#include "net/packet.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace net {
namespace loopback {

constexpr size_t QUEUE_DEPTH = 64; // frames waiting for delivery, further ones are refused

// The largest packet a copied frame can hold
constexpr uint16_t MTU = static_cast<uint16_t>(PACKET_CAPACITY - eth::HEADER_LEN - eth::RX_ALIGN_PAD);

/**
 * The interface through which this host reaches its own addresses. No hardware sits
 * behind it: `transmit` copies each frame and a delivery task feeds the copies back
 * in through `receive`, the way a driver would, so the stack never runs re-entrantly.
 */
class loopback_interface : public interface {
public:
    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE loopback_interface();

    int32_t transmit(packet* pkt) override;

    /**
     * @brief Starts the delivery task and enables the interface.
     * @return OK, or ERR_NO_MEMORY when the task cannot be created.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t start();

    /**
     * @brief Sleeps until frames are queued, then moves all of them into `out`.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void take_pending(packet_list& out);

private:
    sync::spinlock   m_lock;
    packet_list      m_pending;
    sync::wait_queue m_wq;
};

/**
 * @brief Creates the loopback interface, registers it as lo0, and starts it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace loopback
} // namespace net

#endif // STELLUX_NET_LOOPBACK_H
