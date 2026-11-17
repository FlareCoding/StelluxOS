#ifndef SHEEN_EXTENSIONS_EMBEDDED_SERIAL_PORT_WATCH_H
#define SHEEN_EXTENSIONS_EMBEDDED_SERIAL_PORT_WATCH_H

/* Process-wide ref-counted `serial watch --events` service shared across open workspaces. */

#include "extensions/embedded/embedded_protocol.h"
#include "extensions/extension_host.h"
#include "workspace/process_task.h"

#include <QByteArrayView>

#include <cstdint>
#include <functional>
#include <vector>

class QTimer;

namespace sheen {

struct serial_watch_listener {
    void* owner = nullptr;
    std::function<void()> on_changed;
};

class serial_port_watch : public shared_service {
public:
    explicit serial_port_watch(bool auto_start = true);
    ~serial_port_watch() override;

    void add_listener(void* owner, std::function<void()> on_changed);
    void remove_listener(void* owner);

    const std::vector<serial_port_entry>& ports() const { return m_catalog.ports(); }
    resolved_serial_port resolve_port(std::string_view preferred_port, uint32_t baud_rate = 0) const;

    void feed_stdout_chunk(QByteArrayView chunk);

private:
    void start_watch_process();
    bool has_listener(const void* owner) const;
    void notify_listeners();

    process_task m_task;
    QTimer* m_retry_timer = nullptr;
    serial_port_catalog m_catalog;
    std::vector<serial_watch_listener> m_listeners;
    bool m_auto_start = true;
    bool m_is_shutting_down = false;
};

} // namespace sheen

#endif
