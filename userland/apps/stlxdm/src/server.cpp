#include "server.hpp"

#include <stlx/proc.h>
#include <stlxgfx/image.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Spawns a detached app. args is an optional space-separated argument
 * string tokenized into a scratch copy, no quoting support. */
static void spawn_app(const char* path, const char* args) {
    char scratch[256];
    const char* argv[16];
    int argc = 0;

    if (args && args[0]) {
        strncpy(scratch, args, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';

        for (char* p = scratch; *p && argc < 15;) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            argv[argc++] = p;
            while (*p && *p != ' ') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
    }
    argv[argc] = nullptr;

    int handle = proc_exec(path, argc > 0 ? argv : nullptr);
    if (handle < 0) {
        printf("stlxdm: failed to spawn %s\r\n", path);
        return;
    }

    proc_detach(handle);
    printf("stlxdm: spawned %s\r\n", path);
}

/* Loads the configured wallpaper and pre-scales it to cover the
 * screen, so compose only ever pays for a plain blit */
static stlxgfx_surface_t* load_wallpaper(const dm_config& conf,
                                         uint32_t screen_w,
                                         uint32_t screen_h) {
    if (!conf.wallpaper[0]) {
        return nullptr;
    }

    stlxgfx_surface_t* image = stlxgfx_load_image(conf.wallpaper);
    if (!image) {
        printf("stlxdm: failed to load wallpaper %s\r\n", conf.wallpaper);
        return nullptr;
    }

    stlxgfx_surface_t* scaled =
        stlxgfx_create_surface(screen_w, screen_h, 32, 16, 8, 0);
    if (!scaled) {
        stlxgfx_destroy_surface(image);
        return nullptr;
    }

    /* Center-crop the source to the screen's aspect ratio, then scale */
    uint64_t img_w = image->width;
    uint64_t img_h = image->height;
    uint32_t crop_w;
    uint32_t crop_h;
    if (img_w * screen_h > img_h * screen_w) {
        crop_h = static_cast<uint32_t>(img_h);
        crop_w = static_cast<uint32_t>(img_h * screen_w / screen_h);
    } else {
        crop_w = static_cast<uint32_t>(img_w);
        crop_h = static_cast<uint32_t>(img_w * screen_h / screen_w);
    }
    if (crop_w == 0) crop_w = 1;
    if (crop_h == 0) crop_h = 1;

    int32_t crop_x = static_cast<int32_t>((img_w - crop_w) / 2);
    int32_t crop_y = static_cast<int32_t>((img_h - crop_h) / 2);
    stlxgfx_blit_scaled(scaled, 0, 0, screen_w, screen_h,
                        image, crop_x, crop_y, crop_w, crop_h);
    stlxgfx_destroy_surface(image);

    return scaled;
}

/* Parses a plus-separated chord such as ctrl+alt+t into modifier
 * bits and a usage. Returns false for keys outside the letter and
 * digit rows. */
static bool parse_hotkey(const char* chord, uint8_t* out_mods,
                         uint16_t* out_usage) {
    uint8_t mods = 0;
    uint16_t usage = 0;

    const char* p = chord;
    while (*p) {
        const char* end = strchr(p, '+');
        size_t len = end ? static_cast<size_t>(end - p) : strlen(p);

        if (len == 4 && strncmp(p, "ctrl", 4) == 0) {
            mods |= 1u << 1;
        } else if (len == 3 && strncmp(p, "alt", 3) == 0) {
            mods |= 1u << 2;
        } else if (len == 5 && strncmp(p, "shift", 5) == 0) {
            mods |= 1u << 0;
        } else if ((len == 3 && strncmp(p, "gui", 3) == 0) ||
                   (len == 5 && strncmp(p, "super", 5) == 0)) {
            mods |= 1u << 3;
        } else if (len == 1 && *p >= 'a' && *p <= 'z') {
            usage = static_cast<uint16_t>(0x04 + (*p - 'a'));
        } else if (len == 1 && *p >= '1' && *p <= '9') {
            usage = static_cast<uint16_t>(0x1E + (*p - '1'));
        } else if (len == 1 && *p == '0') {
            usage = 0x27;
        } else {
            return false;
        }

        p = end ? end + 1 : p + len;
    }

    if (usage == 0) {
        return false;
    }

    *out_mods = mods;
    *out_usage = usage;
    return true;
}

int server::init(presenter* pres, const dm_config* conf) {
    m_presenter = pres;
    m_conf = conf;

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

    if (m_cursor.init() != 0) {
        return -1;
    }

    if (m_panels.init(pres->width(), pres->height(), *conf) != 0) {
        return -1;
    }

    if (m_power.init(pres->width(), pres->height(),
                     conf->taskbar_height) != 0) {
        return -1;
    }

    m_panels.on_launch = [](const char* path) {
        spawn_app(path, nullptr);
    };

    m_wallpaper = load_wallpaper(*conf, pres->width(), pres->height());

    /* Exec shortcuts with parseable chords become live hotkeys */
    for (uint32_t i = 0; i < conf->shortcut_count; i++) {
        const dm_conf_shortcut& sc = conf->shortcuts[i];
        if (strcmp(sc.action, "exec") != 0 || !sc.path[0]) {
            continue;
        }

        dm_hotkey hk;
        if (parse_hotkey(sc.key, &hk.mods, &hk.usage)) {
            hk.path = sc.path;
            m_hotkeys.push_back(hk);
        }
    }

    /* The config's autostart entries, or a lone terminal without any */
    if (conf->autostart_count > 0) {
        for (uint32_t i = 0; i < conf->autostart_count; i++) {
            spawn_app(conf->autostart[i].path, conf->autostart[i].args);
        }
    } else {
        spawn_app("/bin/stlxterm", nullptr);
    }

    m_cursor_x = static_cast<int32_t>(pres->width()) / 2;
    m_cursor_y = static_cast<int32_t>(pres->height()) / 2;

    /* The first tick paints the whole desktop over the boot contents */
    m_damage.add_full();

    return 0;
}

void server::shutdown() {
    for (auto& c : m_clients) {
        close(c->fd);
    }
    m_clients.clear();

    stlxgfx_destroy_surface(m_wallpaper);
    m_wallpaper = nullptr;
    m_power.shutdown();
    m_panels.shutdown();

    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        unlink(SWP_SOCKET_PATH);
        m_listen_fd = -1;
    }
}

void server::collect_fds(std::vector<pollfd>& fds) const {
    fds.push_back({ m_listen_fd, POLLIN, 0 });
    for (const auto& c : m_clients) {
        short events = POLLIN;
        if (!c->out_q.empty()) {
            events |= POLLOUT;
        }
        fds.push_back({ c->fd, events, 0 });
    }
}

void server::pump(const std::vector<pollfd>& fds) {
    /* Slot 0 is the listen socket, clients follow in collect_fds order */
    if (fds[0].revents & POLLIN) {
        accept_one();
    }

    for (size_t i = 0; i < m_clients.size() && i + 1 < fds.size(); i++) {
        if (fds[i + 1].revents & POLLOUT) {
            flush_client(*m_clients[i]);
        }
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
                                  m_presenter->width(),
                                  m_presenter->height() };
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
        swp_clipboard_data prefix = { static_cast<uint32_t>(m_clipboard.size()) };
        memcpy(reply, &prefix, sizeof(prefix));
        if (!m_clipboard.empty()) {
            memcpy(reply + sizeof(prefix), m_clipboard.data(),
                   m_clipboard.size());
        }

        send_to(c, SWP_MSG_CLIPBOARD_DATA, reply,
                static_cast<uint32_t>(sizeof(prefix) + m_clipboard.size()));
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

void server::spawn_shortcut(const char* path) {
    spawn_app(path, nullptr);
}

constexpr size_t OUT_Q_LIMIT = 64 * 1024;

/* Nonblocking send that queues whatever the socket refuses. Overflow
 * means the client stopped reading long ago and it is disconnected. */
bool server::send_to(dm_client& c, uint16_t type,
                     const void* payload, uint32_t length) {
    if (c.dead) {
        return false;
    }

    swp_header hdr = { type, 0, length };
    uint8_t msg[SWP_MAX_MSG_SIZE];
    memcpy(msg, &hdr, sizeof(hdr));
    if (length > 0) {
        memcpy(msg + sizeof(hdr), payload, length);
    }

    size_t total = sizeof(hdr) + length;
    size_t sent = 0;

    /* Queued bytes must go first to preserve message order */
    if (c.out_q.empty()) {
        ssize_t n = write(c.fd, msg, total);
        if (n == static_cast<ssize_t>(total)) {
            return true;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            c.dead = true;
            return false;
        }

        sent = n > 0 ? static_cast<size_t>(n) : 0;
    }

    if (c.out_q.size() + (total - sent) > OUT_Q_LIMIT) {
        c.dead = true;
        return false;
    }

    /* Any append that is not a lone motion invalidates the coalesce
     * marker so ordering is never disturbed */
    c.q_motion_off = SIZE_MAX;
    c.out_q.insert(c.out_q.end(), msg + sent, msg + total);
    return true;
}

/* Flush one window's staged records as a single EVENT message. Under
 * queue pressure the batch keeps only its newest motion, and a lone
 * motion overwrites the queued one in place, so a stalled client sees
 * the latest position without the queue growing. */
void server::flush_window_events(dm_client& c, dm_window& w) {
    if (w.ev_batch_count == 0 || c.dead) {
        w.ev_batch_count = 0;
        return;
    }

    if (!c.out_q.empty()) {
        int32_t last_motion = -1;
        for (uint32_t i = 0; i < w.ev_batch_count; i++) {
            if (w.ev_batch[i].kind == SWP_EV_MOTION) {
                last_motion = static_cast<int32_t>(i);
            }
        }

        uint32_t keep = 0;
        for (uint32_t i = 0; i < w.ev_batch_count; i++) {
            if (w.ev_batch[i].kind == SWP_EV_MOTION &&
                static_cast<int32_t>(i) != last_motion) {
                continue;
            }
            w.ev_batch[keep++] = w.ev_batch[i];
        }
        w.ev_batch_count = keep;

        if (w.ev_batch_count == 1 &&
            w.ev_batch[0].kind == SWP_EV_MOTION &&
            c.q_motion_off != SIZE_MAX && c.q_motion_win == w.win_id) {
            memcpy(c.out_q.data() + c.q_motion_off +
                       sizeof(swp_header) + sizeof(swp_event_prefix),
                   &w.ev_batch[0], sizeof(swp_event_rec));
            w.ev_batch_count = 0;
            return;
        }
    }

    struct {
        swp_event_prefix prefix;
        swp_event_rec recs[DM_EV_BATCH_MAX];
    } msg;
    msg.prefix = { w.win_id, w.ev_batch_count };
    memcpy(msg.recs, w.ev_batch,
           w.ev_batch_count * sizeof(swp_event_rec));
    uint32_t length = static_cast<uint32_t>(
        sizeof(swp_event_prefix) +
        w.ev_batch_count * sizeof(swp_event_rec));

    size_t off = c.out_q.size();
    bool lone_motion = w.ev_batch_count == 1 &&
                       w.ev_batch[0].kind == SWP_EV_MOTION;
    w.ev_batch_count = 0;
    if (!send_to(c, SWP_MSG_EVENT, &msg, length)) {
        return;
    }

    /* The coalesce marker is valid only when a whole lone-motion
     * message sits in the queue */
    if (lone_motion && c.out_q.size() > off &&
        c.out_q.size() - off == sizeof(swp_header) + length) {
        c.q_motion_off = off;
        c.q_motion_win = w.win_id;
    }
}

/* Cross-window delivery order within one wakeup is unspecified, only
 * per-window order is promised. */
void server::flush_events() {
    for (auto& c : m_clients) {
        for (auto& w : c->windows) {
            flush_window_events(*c, *w);
        }
    }
}

void server::flush_client(dm_client& c) {
    if (c.dead || c.out_q.empty()) {
        return;
    }

    ssize_t n = write(c.fd, c.out_q.data(), c.out_q.size());
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            c.dead = true;
        }
        return;
    }

    c.out_q.erase(c.out_q.begin(), c.out_q.begin() + n);

    if (c.q_motion_off != SIZE_MAX) {
        if (static_cast<size_t>(n) > c.q_motion_off) {
            c.q_motion_off = SIZE_MAX;
        } else {
            c.q_motion_off -= static_cast<size_t>(n);
        }
    }
}

