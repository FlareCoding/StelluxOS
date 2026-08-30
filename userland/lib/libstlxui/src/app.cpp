/* The application object: one connection, one poll loop serving
 * window events, watched descriptors, and one shot timers. Idle
 * means blocked forever, timers exist only while pending.
 */
#include <stlxui/stlxui.h>

#include <stlxwin/stlxwin.h>

#include <poll.h>
#include <time.h>

namespace ui {

static uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}

app::app(const char* app_id) {
    m_conn = stlxwin_connect(app_id);
}

app::~app() {
    m_hosts.clear();

    if (m_conn) {
        stlxwin_disconnect(m_conn);
    }
}

window_host* app::create_window(uint32_t w, uint32_t h, const char* title,
                                uint32_t win_flags) {
    if (!m_conn) {
        return nullptr;
    }

    auto host = std::unique_ptr<window_host>(new window_host(*this));
    host->m_win = stlxwin_window_create(m_conn, w, h, title, win_flags);
    if (!host->m_win) {
        return nullptr;
    }

    window_host* raw = host.get();
    m_hosts.push_back(std::move(host));

    return raw;
}

void app::watch_fd(int fd, std::function<void()> on_readable) {
    m_watches.push_back({ fd, std::move(on_readable) });
}

void app::unwatch_fd(int fd) {
    for (size_t i = 0; i < m_watches.size(); i++) {
        if (m_watches[i].fd == fd) {
            m_watches.erase(m_watches.begin() + static_cast<long>(i));
            return;
        }
    }
}

uint64_t app::set_timer(int64_t after_ns, std::function<void()> fn) {
    uint64_t id = m_next_timer_id++;
    uint64_t deadline = now_ns() + static_cast<uint64_t>(after_ns > 0 ? after_ns : 0);
    m_timers.push_back({ id, deadline, std::move(fn) });

    return id;
}

void app::cancel_timer(uint64_t id) {
    for (size_t i = 0; i < m_timers.size(); i++) {
        if (m_timers[i].id == id) {
            m_timers.erase(m_timers.begin() + static_cast<long>(i));
            return;
        }
    }
}

void app::quit(int code) {
    m_exit_code = code;
    m_running = false;
}

int app::run() {
    if (!m_conn) {
        return 1;
    }

    m_running = true;
    while (m_running && !m_hosts.empty()) {
        for (auto& h : m_hosts) {
            h->flush();
        }

        /* The nearest timer bounds the sleep, idle blocks forever */
        int timeout = -1;
        if (!m_timers.empty()) {
            uint64_t now = now_ns();
            uint64_t nearest = m_timers[0].deadline_ns;
            for (const timer& t : m_timers) {
                if (t.deadline_ns < nearest) {
                    nearest = t.deadline_ns;
                }
            }

            timeout = nearest > now
                    ? static_cast<int>((nearest - now) / 1000000ull) : 0;
        }

        std::vector<pollfd> fds;
        fds.push_back({ stlxwin_conn_fd(m_conn), POLLIN, 0 });
        for (const fd_watch& w : m_watches) {
            fds.push_back({ w.fd, POLLIN, 0 });
        }

        int rc = poll(fds.data(), fds.size(), timeout);
        if (rc < 0) {
            continue;
        }

        if (fds[0].revents & POLLIN) {
            stlxwin_dispatch(m_conn);
        }

        stlxwin_event ev;
        while (stlxwin_next_event(m_conn, &ev)) {
            window_host* h = nullptr;
            for (auto& cand : m_hosts) {
                if (cand->m_win == ev.window) {
                    h = cand.get();
                    break;
                }
            }
            if (!h) {
                if (ev.type == STLXWIN_EVT_DISCONNECTED) {
                    quit(1);
                    break;
                }
                continue;
            }

            switch (ev.type) {
            case STLXWIN_EVT_CONFIGURE:
                /* A configure obliges a commit, and the relayout at
                 * flush happens against the newly sized buffer */
                h->root()->invalidate_layout();
                break;
            case STLXWIN_EVT_POINTER_MOTION:
                h->dispatch_pointer_move({ ev.motion.x, ev.motion.y });
                break;
            case STLXWIN_EVT_POINTER_LEAVE:
                h->dispatch_pointer_move({ -1, -1 });
                break;
            case STLXWIN_EVT_BUTTON_DOWN:
                h->dispatch_pointer_button({ ev.button.x, ev.button.y },
                                           ev.button.button, true);
                break;
            case STLXWIN_EVT_BUTTON_UP:
                h->dispatch_pointer_button({ ev.button.x, ev.button.y },
                                           ev.button.button, false);
                break;
            case STLXWIN_EVT_SCROLL:
                h->dispatch_scroll({ ev.scroll.x, ev.scroll.y },
                                   ev.scroll.dy);
                break;
            case STLXWIN_EVT_KEY_DOWN:
            case STLXWIN_EVT_KEY_REPEAT:
                h->dispatch_key({ ev.key.usage, ev.key.modifiers,
                                  ev.key.ch }, true);
                break;
            case STLXWIN_EVT_KEY_UP:
                h->dispatch_key({ ev.key.usage, ev.key.modifiers,
                                  ev.key.ch }, false);
                break;
            case STLXWIN_EVT_CLOSE: {
                bool close = !h->on_close || h->on_close();
                if (close) {
                    for (size_t i = 0; i < m_hosts.size(); i++) {
                        if (m_hosts[i].get() == h) {
                            m_hosts.erase(m_hosts.begin()
                                          + static_cast<long>(i));
                            break;
                        }
                    }
                }
                break;
            }
            case STLXWIN_EVT_POPUP_DISMISSED:
                /* The compositor already destroyed the popup on an
                 * outside press, only the host remains to retire */
                if (m_menu_host) {
                    m_menu_retire = true;
                }
                break;
            case STLXWIN_EVT_DISCONNECTED:
                quit(1);
                break;
            default:
                break;
            }

            if (!m_running) {
                break;
            }
        }

        /* Menu retirement happens between dispatch and the callback,
         * so selection may safely open the next menu */
        if (m_menu_retire) {
            window_host* dead = m_menu_host;
            m_menu_host = nullptr;
            m_menu_retire = false;

            for (size_t i = 0; i < m_hosts.size(); i++) {
                if (m_hosts[i].get() == dead) {
                    m_hosts.erase(m_hosts.begin() + static_cast<long>(i));
                    break;
                }
            }

            std::function<void()> after = std::move(m_menu_after_close);
            m_menu_after_close = nullptr;
            if (after) {
                after();
            }
        }

        /* Due timers fire once and disappear */
        uint64_t now = now_ns();
        for (size_t i = 0; i < m_timers.size();) {
            if (m_timers[i].deadline_ns > now) {
                i++;
                continue;
            }

            std::function<void()> fn = std::move(m_timers[i].fn);
            m_timers.erase(m_timers.begin() + static_cast<long>(i));
            fn();
        }

        for (size_t i = 1; i < fds.size() && i - 1 < m_watches.size(); i++) {
            if (fds[i].revents & POLLIN) {
                m_watches[i - 1].on_readable();
            }
        }
    }

    return m_exit_code;
}

} // namespace ui
