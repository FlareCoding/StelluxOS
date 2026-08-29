#ifndef STLXDM_SERVER_HPP
#define STLXDM_SERVER_HPP

#include <stlxwin/proto.h>

#include <cstdint>
#include <memory>
#include <vector>

struct screen;

/* One connected application. Owns the socket and the partial-message
 * assembly buffer. Window and buffer state attach here in later units. */
struct dm_client {
    int      fd = -1;
    bool     hello_done = false;
    bool     dead = false;
    char     app_id[SWP_APP_ID_MAX] = {};

    uint8_t  rd_buf[SWP_MAX_MSG_SIZE];
    uint32_t rd_have = 0;

    explicit dm_client(int sock) : fd(sock) {}
};

/* The protocol server: listens, accepts, pumps client sockets, and
 * routes complete messages. Compose and input attach in later units. */
class server {
public:
    /* Binds and listens on the protocol socket. Returns 0 or -1. */
    int init(const screen* scr);
    void shutdown();

    /* Fills fds for one poll cycle: listen socket plus every client. */
    void collect_fds(std::vector<struct pollfd>& fds) const;

    /* Accepts new connections and pumps readable clients, then reaps
     * dead ones. Call after every poll wakeup. */
    void pump(const std::vector<struct pollfd>& fds);

private:
    void accept_one();
    void pump_client(dm_client& c);

    void handle_message(dm_client& c, const swp_header& hdr,
                        const uint8_t* payload);

    void drop_client(dm_client& c);
    bool send_to(dm_client& c, uint16_t type,
                 const void* payload, uint32_t length);

    int m_listen_fd = -1;
    const screen* m_screen = nullptr;
    std::vector<std::unique_ptr<dm_client>> m_clients;
};

#endif
