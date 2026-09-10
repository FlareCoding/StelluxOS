#ifndef STELLUX_TESTS_NET_STUB_INTERFACE_H
#define STELLUX_TESTS_NET_STUB_INTERFACE_H

#include "net/interface.h"
#include "net/eth.h"
#include "net/net.h"
#include "common/string.h"

// A link that keeps the last frame it was asked to send, never registered with the stack
class stub_interface : public net::interface {
public:
    explicit stub_interface(bool loopback = false) {
        m_loopback = loopback;
        m_enabled = true;
        m_mtu = net::eth::MTU;
        m_mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
    }

    int32_t transmit(net::packet* pkt) override {
        m_last_frame_len = pkt->length() < sizeof(m_last_frame) ? pkt->length() : sizeof(m_last_frame);
        string::memcpy(m_last_frame, pkt->data(), m_last_frame_len);
        m_frames_sent++;
        return net::OK;
    }

    size_t frames_sent() const { return m_frames_sent; }
    const uint8_t* last_frame() const { return m_last_frame; }
    size_t last_frame_len() const { return m_last_frame_len; }

private:
    uint8_t m_last_frame[net::eth::HEADER_LEN + net::eth::MTU] = {};
    size_t  m_last_frame_len = 0;
    size_t  m_frames_sent = 0;
};

#endif // STELLUX_TESTS_NET_STUB_INTERFACE_H
