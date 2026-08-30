#include "server.hpp"
#include "screen.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int server::init(const screen* scr) {
    m_screen = scr;

    m_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listen_fd < 0) {
        return -1;
    }

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SWP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SWP_SOCKET_PATH);
    if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) != 0 ||
        listen(m_listen_fd, 8) != 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
        return -1;
    }

    fcntl(m_listen_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

void server::shutdown() {
    for (auto& c : m_clients) {
        close(c->fd);
    }
    m_clients.clear();

    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        unlink(SWP_SOCKET_PATH);
        m_listen_fd = -1;
    }
}

void server::collect_fds(std::vector<pollfd>& fds) const {
    fds.push_back({ m_listen_fd, POLLIN, 0 });
    for (const auto& c : m_clients) {
        fds.push_back({ c->fd, POLLIN, 0 });
    }
}

void server::pump(const std::vector<pollfd>& fds) {
    /* Slot 0 is the listen socket, clients follow in collect_fds order */
    if (fds[0].revents & POLLIN) {
        accept_one();
    }

    for (size_t i = 0; i < m_clients.size() && i + 1 < fds.size(); i++) {
        if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) {
            pump_client(*m_clients[i]);
        }
    }

    for (size_t i = m_clients.size(); i-- > 0;) {
        if (m_clients[i]->dead) {
            drop_client(*m_clients[i]);
            m_clients.erase(m_clients.begin() + static_cast<long>(i));
        }
    }
}

void server::accept_one() {
    int fd = accept(m_listen_fd, nullptr, nullptr);
    if (fd < 0) {
        return;
    }

    fcntl(fd, F_SETFL, O_NONBLOCK);
    m_clients.push_back(std::make_unique<dm_client>(fd));
}

void server::pump_client(dm_client& c) {
    while (true) {
        ssize_t n = read(c.fd, c.rd_buf + c.rd_have,
                         sizeof(c.rd_buf) - c.rd_have);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            c.dead = true;
            return;
        }
        if (n == 0) {
            c.dead = true;
            return;
        }

        c.rd_have += static_cast<uint32_t>(n);

        uint32_t off = 0;
        while (c.rd_have - off >= sizeof(swp_header)) {
            swp_header hdr;
            memcpy(&hdr, c.rd_buf + off, sizeof(hdr));
            if (hdr.length > SWP_MAX_MSG_SIZE - sizeof(hdr)) {
                c.dead = true;
                return;
            }

            if (c.rd_have - off < sizeof(hdr) + hdr.length) {
                break;
            }

            handle_message(c, hdr, c.rd_buf + off + sizeof(hdr));
            if (c.dead) {
                return;
            }
            off += sizeof(hdr) + hdr.length;
        }

        if (off > 0) {
            memmove(c.rd_buf, c.rd_buf + off, c.rd_have - off);
            c.rd_have -= off;
        }
    }
}

void server::handle_message(dm_client& c, const swp_header& hdr,
                            const uint8_t* payload) {
    /* The hello handshake gates everything else */
    if (!c.hello_done) {
        if (hdr.type != SWP_MSG_HELLO || hdr.length != sizeof(swp_hello)) {
            c.dead = true;
            return;
        }

        const swp_hello* m = reinterpret_cast<const swp_hello*>(payload);
        if (m->proto_version != SWP_VERSION) {
            c.dead = true;
            return;
        }

        memcpy(c.app_id, m->app_id, sizeof(c.app_id));
        c.app_id[sizeof(c.app_id) - 1] = '\0';
        c.hello_done = true;

        swp_hello_reply reply = { SWP_VERSION, SWP_FMT_XRGB8888,
                                  m_screen->width, m_screen->height };
        if (!send_to(c, SWP_MSG_HELLO_REPLY, &reply, sizeof(reply))) {
            c.dead = true;
        }
        return;
    }

    switch (hdr.type) {
    case SWP_MSG_CREATE_WINDOW:
        if (hdr.length == sizeof(swp_create_window)) {
            handle_create_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_DESTROY_WINDOW:
        if (hdr.length == sizeof(swp_destroy_window)) {
            handle_destroy_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_SET_WINDOW:
        if (hdr.length == sizeof(swp_set_window)) {
            handle_set_window(c, payload);
            return;
        }
        break;
    case SWP_MSG_ATTACH_BUFFER:
        if (hdr.length == sizeof(swp_attach_buffer)) {
            handle_attach_buffer(c, payload);
            return;
        }
        break;
    case SWP_MSG_DETACH_BUFFER:
        if (hdr.length == sizeof(swp_detach_buffer)) {
            handle_detach_buffer(c, payload);
            return;
        }
        break;
    case SWP_MSG_COMMIT:
        if (hdr.length == sizeof(swp_commit)) {
            handle_commit(c, payload);
            return;
        }
        break;
    case SWP_MSG_CLIPBOARD_SET: {
        if (hdr.length < sizeof(swp_clipboard_set)) {
            break;
        }

        const auto* m = reinterpret_cast<const swp_clipboard_set*>(payload);
        if (m->len > SWP_CLIPBOARD_MAX ||
            hdr.length != sizeof(*m) + m->len) {
            break;
        }

        const char* text = reinterpret_cast<const char*>(payload + sizeof(*m));
        m_clipboard.assign(text, text + m->len);
        return;
    }
    case SWP_MSG_CLIPBOARD_GET: {
        uint8_t reply[sizeof(swp_clipboard_data) + SWP_CLIPBOARD_MAX];
        swp_clipboard_data prefix = { (uint32_t)m_clipboard.size() };
        memcpy(reply, &prefix, sizeof(prefix));
        if (!m_clipboard.empty()) {
            memcpy(reply + sizeof(prefix), m_clipboard.data(),
                   m_clipboard.size());
        }

        send_to(c, SWP_MSG_CLIPBOARD_DATA, reply,
                (uint32_t)(sizeof(prefix) + m_clipboard.size()));
        return;
    }
    case SWP_MSG_CAPTURE:
        /* Deferred until the screenshot consumer exists, the wire needs
         * a completion message first */
        return;
    default:
        break;
    }

    c.dead = true;
}

void server::drop_client(dm_client& c) {
    while (!c.windows.empty()) {
        destroy_window_tree(c, c.windows.back()->win_id);
    }

    close(c.fd);
    c.fd = -1;
}

bool server::send_to(dm_client& c, uint16_t type,
                     const void* payload, uint32_t length) {
    swp_header hdr = { type, 0, length };
    uint8_t msg[SWP_MAX_MSG_SIZE];
    memcpy(msg, &hdr, sizeof(hdr));
    if (length > 0) {
        memcpy(msg + sizeof(hdr), payload, length);
    }

    /* Nonblocking single write. A client that cannot take a small
     * reply has already stalled beyond saving. Outbound queueing for
     * event bursts lands with the event unit. */
    size_t total = sizeof(hdr) + length;
    ssize_t n = write(c.fd, msg, total);
    return n == static_cast<ssize_t>(total);
}

