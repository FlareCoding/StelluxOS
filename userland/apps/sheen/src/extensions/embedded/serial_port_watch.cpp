#include "extensions/embedded/serial_port_watch.h"

#include <QTimer>

#include <algorithm>
#include <string_view>
#include <utility>

namespace sheen {

static constexpr int WATCH_RETRY_DELAY_MS = 2000;

serial_port_watch::serial_port_watch(bool auto_start)
    : m_retry_timer(new QTimer(&m_task)),
      m_auto_start(auto_start) {
    m_retry_timer->setSingleShot(true);
    QObject::connect(m_retry_timer, &QTimer::timeout, &m_task, [this] {
        if (!m_is_shutting_down && !m_listeners.empty()) {
            start_watch_process();
        }
    });

    m_task.on_stdout = [this](QByteArrayView chunk) {
        if (!m_is_shutting_down) {
            feed_stdout_chunk(chunk);
        }
    };

    m_task.on_finished = [this](const process_exit& result) {
        if (m_is_shutting_down) {
            return;
        }

        if (m_catalog.finish_process()) {
            notify_listeners();
        }

        if (!result.was_stopped && !m_listeners.empty()) {
            m_retry_timer->start(WATCH_RETRY_DELAY_MS);
        }
    };

    if (m_auto_start) {
        start_watch_process();
    }
}

serial_port_watch::~serial_port_watch() {
    m_is_shutting_down = true;
    m_retry_timer->stop();
    m_listeners.clear();

    m_task.on_stdout = nullptr;
    m_task.on_stderr = nullptr;
    m_task.on_finished = nullptr;
    m_task.stop();
}

void serial_port_watch::add_listener(void* owner, std::function<void()> on_changed) {
    remove_listener(owner);
    m_listeners.push_back({ owner, std::move(on_changed) });

    if (m_auto_start && !m_task.is_running() && !m_retry_timer->isActive()) {
        start_watch_process();
    }
}

void serial_port_watch::remove_listener(void* owner) {
    auto it = std::remove_if(m_listeners.begin(), m_listeners.end(), [owner](const serial_watch_listener& entry) {
        return entry.owner == owner;
    });
    m_listeners.erase(it, m_listeners.end());
}

resolved_serial_port serial_port_watch::resolve_port(std::string_view preferred_port, uint32_t baud_rate) const {
    return m_catalog.resolve(preferred_port, baud_rate);
}

void serial_port_watch::feed_stdout_chunk(QByteArrayView chunk) {
    std::string_view raw(chunk.data(), static_cast<size_t>(chunk.size()));
    if (m_catalog.feed_chunk(raw)) {
        notify_listeners();
    }
}

void serial_port_watch::start_watch_process() {
    if (m_task.is_running() || m_is_shutting_down) {
        return;
    }

    process_spec spec;
    spec.program = QStringLiteral("/bin/serial");
    spec.arguments = { QStringLiteral("watch"), QStringLiteral("--events") };
    spec.io_mode = process_io_mode::PIPES;
    spec.opens_stdin = false;

    QString ignored_error;
    m_task.start(spec, &ignored_error);
}

bool serial_port_watch::has_listener(const void* owner) const {
    for (const serial_watch_listener& entry : m_listeners) {
        if (entry.owner == owner) {
            return true;
        }
    }

    return false;
}

void serial_port_watch::notify_listeners() {
    std::vector<serial_watch_listener> snapshot = m_listeners;
    for (const serial_watch_listener& entry : snapshot) {
        if (has_listener(entry.owner) && entry.on_changed) {
            entry.on_changed();
        }
    }
}

} // namespace sheen
