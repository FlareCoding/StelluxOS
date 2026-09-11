#include "net/loopback.h"
#include "net/eth.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "dynpriv/dynpriv.h"
#include "common/logging.h"
#include "common/string.h"

namespace net {
namespace loopback {

static void delivery_task(void* arg) {
    auto* lo = static_cast<loopback_interface*>(arg);

    packet_list batch;
    batch.init();

    while (true) {
        RUN_ELEVATED(lo->take_pending(batch));

        while (packet* pkt = batch.pop_front()) {
            lo->receive(pkt);
        }
    }

    sched::exit(0);
}

__PRIVILEGED_CODE loopback_interface::loopback_interface()
    : m_lock(sync::SPINLOCK_INIT) {
    m_pending.init();
    m_wq.init();

    m_loopback = true;
    m_link_up.store_relaxed(true);
    m_mtu = MTU;

    set_ipv4_conf({{{127, 0, 0, 1}}, {{255, 0, 0, 0}}, {{0, 0, 0, 0}}});
}

int32_t loopback_interface::transmit(packet* pkt) {
    if (!pkt || pkt->length() == 0) {
        return ERR_INVALID;
    }

    if (pkt->length() > eth::HEADER_LEN + MTU) {
        return ERR_TOO_LARGE;
    }

    // The caller keeps its packet, so a copy carries the frame from here
    packet* copy = packet::alloc();
    if (!copy) {
        record_packet_dropped();
        return ERR_BUSY;
    }

    (void)copy->reserve(eth::RX_ALIGN_PAD);

    uint8_t* frame = copy->put(pkt->length());
    string::memcpy(frame, pkt->data(), pkt->length());

    copy->set_iface(this);

    bool queued = false;
    RUN_ELEVATED({
        {
            sync::irq_lock_guard guard(m_lock);
            if (m_pending.size() < QUEUE_DEPTH) {
                m_pending.push_back(copy);

                m_counters.frames_out++;
                m_counters.bytes_out += pkt->length();
                queued = true;
            }
        }

        if (queued) {
            sync::wake_one(m_wq);
        }
    });

    if (!queued) {
        packet::free(copy);
        record_packet_dropped();
        return ERR_BUSY;
    }

    return OK;
}

__PRIVILEGED_CODE int32_t loopback_interface::start() {
    sched::task* task = sched::create_kernel_task(delivery_task, this, name());
    if (!task) {
        return ERR_NO_MEMORY;
    }

    m_enabled = true;
    sched::enqueue(task);

    return OK;
}

__PRIVILEGED_CODE void loopback_interface::take_pending(packet_list& out) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);
    while (m_pending.empty()) {
        irq = sync::wait(m_wq, m_lock, irq);
    }

    while (packet* pkt = m_pending.pop_front()) {
        out.push_back(pkt);
    }

    sync::spin_unlock_irqrestore(m_lock, irq);
}

__PRIVILEGED_CODE int32_t init() {
    loopback_interface* lo = heap::ualloc_new<loopback_interface>();
    if (!lo) {
        return ERR_NO_MEMORY;
    }

    int32_t rc = register_interface(lo, "lo");
    if (rc != OK) {
        heap::ufree_delete(lo);
        return rc;
    }

    // A registered interface cannot be withdrawn, so on failure it stays down
    rc = lo->start();
    if (rc != OK) {
        log::error("loopback: failed to start the delivery task: %d", rc);
    }

    return rc;
}

} // namespace loopback
} // namespace net
