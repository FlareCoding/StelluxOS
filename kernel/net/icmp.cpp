#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/byteorder.h"
#include "net/checksum.h"
#include "common/logging.h"
#include "common/string.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"
#include "sched/sched.h"
#include "clock/clock.h" // clock::now_ns()
#include "dynpriv/dynpriv.h"

namespace net {

namespace {

struct icmp_waiter {
    uint16_t          id;
    uint16_t          seq;
    bool              active;
    bool              replied;
    uint64_t          send_time_ns;
    uint64_t          recv_time_ns;
    sync::wait_queue  wq;
};

__PRIVILEGED_DATA static icmp_waiter g_waiters[ICMP_MAX_WAITERS] = {};
__PRIVILEGED_DATA static sync::spinlock g_icmp_lock = sync::SPINLOCK_INIT;

} // anonymous namespace

void icmp_init() {
    for (uint32_t i = 0; i < ICMP_MAX_WAITERS; i++) {
        g_waiters[i].active = false;
        g_waiters[i].replied = false;
        g_waiters[i].wq.init();
    }
}

static void icmp_send_reply(netif* iface, uint32_t dst_ip,
                            const uint8_t* request_data, size_t request_len) {
    if (request_len < sizeof(icmp_header)) return;

    // Build echo reply — copy the request and change type to 0
    uint8_t reply[ETH_MTU];
    if (request_len > sizeof(reply)) return;

    string::memcpy(reply, request_data, request_len);
    auto* hdr = reinterpret_cast<icmp_header*>(reply);
    hdr->type = ICMP_TYPE_ECHO_REPLY;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->checksum = inet_checksum(reply, request_len);

    ipv4_send(iface, dst_ip, IPV4_PROTO_ICMP, reply, request_len);
}

void icmp_recv(netif* iface, uint32_t src_ip, const uint8_t* data, size_t len) {
    if (!iface || !data || len < sizeof(icmp_header)) {
        return;
    }

    const auto* hdr = reinterpret_cast<const icmp_header*>(data);

    // Verify ICMP checksum
    uint16_t computed = inet_checksum(data, len);
    if (computed != 0) {
        log::debug("icmp: bad checksum, dropping");
        return;
    }

    if (hdr->type == ICMP_TYPE_ECHO_REQUEST && hdr->code == 0) {
        // Reply to echo request
        log::debug("icmp: echo request from %u.%u.%u.%u",
                   (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                   (src_ip >> 8) & 0xFF, src_ip & 0xFF);
        icmp_send_reply(iface, src_ip, data, len);

    } else if (hdr->type == ICMP_TYPE_ECHO_REPLY && hdr->code == 0) {
        // Match against pending ping waiters
        uint16_t reply_id = ntohs(hdr->id);
        uint16_t reply_seq = ntohs(hdr->sequence);

        log::debug("icmp: echo reply from %u.%u.%u.%u id=%u seq=%u",
                   (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                   (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                   reply_id, reply_seq);

        uint64_t now = 0;
        now = clock::now_ns();

        sync::irq_lock_guard guard(g_icmp_lock);
        for (uint32_t i = 0; i < ICMP_MAX_WAITERS; i++) {
            if (g_waiters[i].active &&
                g_waiters[i].id == reply_id &&
                g_waiters[i].seq == reply_seq) {
                g_waiters[i].replied = true;
                g_waiters[i].recv_time_ns = now;
                RUN_ELEVATED(sync::wake_one(g_waiters[i].wq));
                break;
            }
        }
    }
}

int32_t icmp_send_echo_request(netif* iface, uint32_t dst_ip,
                               uint16_t id, uint16_t seq) {
    if (!iface) return ERR_INVAL;

    // Build ICMP echo request with 56 bytes of payload (total 64 bytes)
    constexpr size_t ICMP_PAYLOAD_LEN = 56;
    uint8_t packet[sizeof(icmp_header) + ICMP_PAYLOAD_LEN];
    string::memset(packet, 0, sizeof(packet));

    auto* hdr = reinterpret_cast<icmp_header*>(packet);
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(seq);

    // Fill payload with pattern
    for (size_t i = 0; i < ICMP_PAYLOAD_LEN; i++) {
        packet[sizeof(icmp_header) + i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Compute ICMP checksum
    hdr->checksum = inet_checksum(packet, sizeof(packet));

    return ipv4_send(iface, dst_ip, IPV4_PROTO_ICMP, packet, sizeof(packet));
}

int32_t icmp_ping(netif* iface, uint32_t dst_ip,
                  uint16_t id, uint16_t seq,
                  uint32_t timeout_ms, uint32_t* out_rtt_us) {
    if (!iface || !out_rtt_us) return ERR_INVAL;

    // Find a free waiter slot
    int32_t slot = -1;
    {
        sync::irq_lock_guard guard(g_icmp_lock);
        for (uint32_t i = 0; i < ICMP_MAX_WAITERS; i++) {
            if (!g_waiters[i].active) {
                slot = static_cast<int32_t>(i);
                g_waiters[i].active = true;
                g_waiters[i].replied = false;
                g_waiters[i].id = id;
                g_waiters[i].seq = seq;
                g_waiters[i].send_time_ns = 0;
                g_waiters[i].recv_time_ns = 0;
                break;
            }
        }
    }

    if (slot < 0) {
        return ERR_NOMEM; // no free waiter slots
    }

    // Record send time and send the request
    uint64_t send_time = 0;
    send_time = clock::now_ns();
    g_waiters[slot].send_time_ns = send_time;

    int32_t send_rc = icmp_send_echo_request(iface, dst_ip, id, seq);
    if (send_rc != OK) {
        sync::irq_lock_guard guard(g_icmp_lock);
        g_waiters[slot].active = false;
        return send_rc;
    }

    // Wait for reply with timeout
    uint64_t deadline_ns = send_time + static_cast<uint64_t>(timeout_ms) * 1000000ULL;

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_icmp_lock);
        while (!g_waiters[slot].replied) {
            uint64_t now = clock::now_ns();
            if (now >= deadline_ns) {
                break; // timeout
            }
            sync::spin_unlock_irqrestore(g_icmp_lock, irq);

            // Sleep briefly then re-check
            sched::sleep_ms(1);

            irq = sync::spin_lock_irqsave(g_icmp_lock);
        }
        sync::spin_unlock_irqrestore(g_icmp_lock, irq);
    });

    // Collect result
    int32_t result;
    {
        sync::irq_lock_guard guard(g_icmp_lock);
        if (g_waiters[slot].replied) {
            uint64_t rtt_ns = g_waiters[slot].recv_time_ns - g_waiters[slot].send_time_ns;
            *out_rtt_us = static_cast<uint32_t>(rtt_ns / 1000);
            result = OK;
        } else {
            result = ERR_TIMEOUT;
        }
        g_waiters[slot].active = false;
    }

    return result;
}

} // namespace net
