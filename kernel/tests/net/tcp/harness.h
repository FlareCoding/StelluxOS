#ifndef STELLUX_TESTS_NET_TCP_HARNESS_H
#define STELLUX_TESTS_NET_TCP_HARNESS_H

#include "../stub_interface.h"
#include "net/tcp/tcp.h"
#include "net/tcp/wire.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/checksum.h"
#include "net/byteorder.h"

// A remote host on a stub link. It builds segments the way the driver and the
// IPv4 layer would deliver them, with the link and network headers marked and
// the window at the TCP header, ready for tcp::input.
struct peer {
    stub_interface*      link;
    net::eth::mac_addr   mac;
    net::ipv4::ipv4_addr addr;
    net::ipv4::ipv4_addr host;
    uint16_t             port;
    uint16_t             host_port;

    net::packet* segment(uint8_t flags, uint32_t seq, uint32_t ack,
                         const net::tcp::tcp_options& opts = {},
                         const void* payload = nullptr, size_t payload_len = 0) const;

    net::packet* icmp_error(uint8_t type, uint8_t code, uint32_t offending_seq, uint16_t next_hop_mtu = 0) const;

    net::packet* syn(uint32_t seq) const { return segment(net::tcp::FLAG_SYN, seq, 0); }
    net::packet* ack(uint32_t seq, uint32_t ack_num) const { return segment(net::tcp::FLAG_ACK, seq, ack_num); }
    net::packet* rst(uint32_t seq) const { return segment(net::tcp::FLAG_RST, seq, 0); }

    // Teaches the link this host's MAC through an ARP request for `host`, so a reply
    // to it needs no resolution, then discards the ARP reply that answered it
    void announce() const;
};

// A configured stub link with one peer on it. The link's ARP entries are removed
// with it so the table never points at a dead interface.
struct linked_peer {
    stub_interface link;
    peer           remote;

    linked_peer();
    ~linked_peer() { net::arp::forget(&link); }
};

inline const net::ipv4::ipv4_header* sent_ip(const stub_interface& link, size_t index) {
    return reinterpret_cast<const net::ipv4::ipv4_header*>(link.frame(index) + net::eth::HEADER_LEN);
}

inline const net::tcp::tcp_header* sent_tcp(const stub_interface& link, size_t index) {
    const net::ipv4::ipv4_header* ip = sent_ip(link, index);
    return reinterpret_cast<const net::tcp::tcp_header*>(
        reinterpret_cast<const uint8_t*>(ip) + ip->header_len());
}

inline net::packet* peer::segment(uint8_t flags, uint32_t seq, uint32_t ack,
                                  const net::tcp::tcp_options& opts,
                                  const void* payload, size_t payload_len) const {
    using namespace net;

    packet* pkt = packet::alloc();
    if (!pkt) {
        return nullptr;
    }

    pkt->set_iface(link);

    eth::eth_header* link_hdr = reinterpret_cast<eth::eth_header*>(pkt->put(eth::HEADER_LEN));
    link_hdr->dest = link->mac();
    link_hdr->src = mac;
    link_hdr->type = htons(eth::TYPE_IPV4);
    pkt->mark_link_header();
    (void)pkt->pull(eth::HEADER_LEN);

    uint8_t options[tcp::MAX_OPTIONS_LEN];
    size_t options_len = tcp::build_options(options, opts);
    size_t segment_len = tcp::HEADER_LEN + options_len + payload_len;

    ipv4::ipv4_header* ip = reinterpret_cast<ipv4::ipv4_header*>(pkt->put(ipv4::HEADER_LEN));
    ip->set_version_ihl(ipv4::VERSION, ipv4::MIN_IHL);
    ip->tos = 0;
    ip->total_len = htons(static_cast<uint16_t>(ipv4::HEADER_LEN + segment_len));
    ip->id = 0;
    ip->fl_frag_off = htons(ipv4::FLAG_DF);
    ip->ttl = ipv4::DEFAULT_TTL;
    ip->proto = ipv4::PROTO_TCP;
    ip->src = addr;
    ip->dst = host;
    ip->checksum = 0;
    ip->checksum = htons(checksum(ip, ipv4::HEADER_LEN));
    pkt->mark_network_header();
    (void)pkt->pull(ipv4::HEADER_LEN);

    uint8_t* bytes = pkt->put(segment_len);
    tcp::tcp_header* hdr = reinterpret_cast<tcp::tcp_header*>(bytes);
    *hdr = {};
    hdr->src_port = htons(port);
    hdr->dst_port = htons(host_port);
    hdr->seq = htonl(seq);
    hdr->ack = htonl(ack);
    hdr->set_data_offset(static_cast<uint8_t>((tcp::HEADER_LEN + options_len) / sizeof(uint32_t)));
    hdr->flags = flags;
    hdr->window = htons(65535);
    string::memcpy(bytes + tcp::HEADER_LEN, options, options_len);
    if (payload_len > 0) {
        string::memcpy(bytes + tcp::HEADER_LEN + options_len, payload, payload_len);
    }

    hdr->checksum = htons(tcp::compute_checksum(addr, host, bytes, segment_len));

    return pkt;
}

inline net::packet* peer::icmp_error(uint8_t type, uint8_t code, uint32_t offending_seq, uint16_t next_hop_mtu) const {
    using namespace net;

    packet* pkt = packet::alloc();
    if (!pkt) {
        return nullptr;
    }

    pkt->set_iface(link);

    eth::eth_header* link_hdr = reinterpret_cast<eth::eth_header*>(pkt->put(eth::HEADER_LEN));
    link_hdr->dest = link->mac();
    link_hdr->src = mac;
    link_hdr->type = htons(eth::TYPE_IPV4);
    
    pkt->mark_link_header();
    (void)pkt->pull(eth::HEADER_LEN);

    size_t offending_len = ipv4::HEADER_LEN + icmp::ERROR_PAYLOAD_LEN;
    size_t message_len = icmp::HEADER_LEN + offending_len;

    ipv4::ipv4_header* ip = reinterpret_cast<ipv4::ipv4_header*>(pkt->put(ipv4::HEADER_LEN));
    ip->set_version_ihl(ipv4::VERSION, ipv4::MIN_IHL);
    ip->tos = 0;
    ip->total_len = htons(static_cast<uint16_t>(ipv4::HEADER_LEN + message_len));
    ip->id = 0;
    ip->fl_frag_off = htons(ipv4::FLAG_DF);
    ip->ttl = ipv4::DEFAULT_TTL;
    ip->proto = ipv4::PROTO_ICMP;
    ip->src = addr;
    ip->dst = host;
    ip->checksum = 0;
    ip->checksum = htons(checksum(ip, ipv4::HEADER_LEN));

    pkt->mark_network_header();
    (void)pkt->pull(ipv4::HEADER_LEN);

    uint8_t* bytes = pkt->put(message_len);
    icmp::icmp_header* hdr = reinterpret_cast<icmp::icmp_header*>(bytes);
    *hdr = {};
    hdr->type = type;
    hdr->code = code;
    hdr->frag.next_hop_mtu = htons(next_hop_mtu);

    ipv4::ipv4_header* offending = reinterpret_cast<ipv4::ipv4_header*>(bytes + icmp::HEADER_LEN);
    *offending = {};
    offending->set_version_ihl(ipv4::VERSION, ipv4::MIN_IHL);
    offending->total_len = htons(static_cast<uint16_t>(ipv4::HEADER_LEN + tcp::HEADER_LEN));
    offending->proto = ipv4::PROTO_TCP;
    offending->src = host;
    offending->dst = addr;

    tcp::tcp_header* offending_tcp = reinterpret_cast<tcp::tcp_header*>(bytes + icmp::HEADER_LEN + ipv4::HEADER_LEN);
    offending_tcp->src_port = htons(host_port);
    offending_tcp->dst_port = htons(port);
    offending_tcp->seq = htonl(offending_seq);

    hdr->checksum = htons(checksum(bytes, message_len));

    return pkt;
}

inline void peer::announce() const {
    using namespace net;

    packet* pkt = packet::alloc();
    if (!pkt) {
        return;
    }

    pkt->set_iface(link);

    eth::eth_header* link_hdr = reinterpret_cast<eth::eth_header*>(pkt->put(eth::HEADER_LEN));
    link_hdr->dest = eth::BROADCAST_ADDR;
    link_hdr->src = mac;
    link_hdr->type = htons(eth::TYPE_ARP);
    pkt->mark_link_header();
    (void)pkt->pull(eth::HEADER_LEN);

    arp::arp_header* request = reinterpret_cast<arp::arp_header*>(pkt->put(arp::HEADER_LEN));
    request->hw_type = htons(arp::HW_TYPE_ETHERNET);
    request->proto_type = htons(arp::PROTO_TYPE_IPV4);
    request->hw_len = eth::MAC_ADDR_LEN;
    request->proto_len = ipv4::ADDR_LEN;
    request->opcode = htons(arp::OP_REQUEST);
    request->sender_hw_addr = mac;
    request->sender_proto_addr = addr;
    request->target_hw_addr = {};
    request->target_proto_addr = host;

    (void)arp::input(pkt);
    link->clear_frames();
}

inline linked_peer::linked_peer() {
    link.configure_ipv4({{{10, 0, 2, 15}}, {{255, 255, 255, 0}}, {{10, 0, 2, 2}}});
    remote = {&link, {{0x52, 0x55, 0x0a, 0x00, 0x02, 0x02}}, {{10, 0, 2, 2}}, {{10, 0, 2, 15}}, 40000, 5000};
    remote.announce();
}

#endif // STELLUX_TESTS_NET_TCP_HARNESS_H
