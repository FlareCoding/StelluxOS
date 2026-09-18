#ifndef STELLUX_TESTS_NET_STUB_INTERFACE_H
#define STELLUX_TESTS_NET_STUB_INTERFACE_H

#include "net/interface.h"
#include "net/eth.h"
#include "net/net.h"
#include "common/string.h"

// A link that keeps the frames it was asked to send, never registered with the stack.
// The last frame is kept whole, earlier ones keep their headers.
class stub_interface : public net::interface {
public:
    static constexpr size_t MAX_FRAMES        = 8;
    static constexpr size_t FRAME_CAPTURE_LEN = 128;

    explicit stub_interface(bool loopback = false) {
        m_loopback = loopback;
        m_enabled = true;
        m_mtu = net::eth::MTU;
        m_mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
    }

    int32_t transmit(net::packet* pkt) override {
        m_last_frame_len = pkt->length() < sizeof(m_last_frame) ? pkt->length() : sizeof(m_last_frame);
        string::memcpy(m_last_frame, pkt->data(), m_last_frame_len);

        if (m_frames_sent < MAX_FRAMES) {
            size_t captured = pkt->length() < FRAME_CAPTURE_LEN ? pkt->length() : FRAME_CAPTURE_LEN;
            string::memcpy(m_frames[m_frames_sent], pkt->data(), captured);
            m_frame_lens[m_frames_sent] = pkt->length();
        }

        m_frames_sent++;
        return net::OK;
    }

    size_t frames_sent() const { return m_frames_sent; }
    const uint8_t* last_frame() const { return m_last_frame; }
    size_t last_frame_len() const { return m_last_frame_len; }

    const uint8_t* frame(size_t index) const { return m_frames[index]; }
    size_t frame_len(size_t index) const { return m_frame_lens[index]; }

    void clear_frames() {
        m_frames_sent = 0;
        m_last_frame_len = 0;
    }

private:
    uint8_t m_last_frame[net::eth::HEADER_LEN + net::eth::MTU] = {};
    size_t  m_last_frame_len = 0;
    uint8_t m_frames[MAX_FRAMES][FRAME_CAPTURE_LEN] = {};
    size_t  m_frame_lens[MAX_FRAMES] = {};
    size_t  m_frames_sent = 0;
};

#endif // STELLUX_TESTS_NET_STUB_INTERFACE_H
