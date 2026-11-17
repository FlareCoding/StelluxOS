#ifndef SHEEN_WORKSPACE_DEPENDENCY_INSTALLER_H
#define SHEEN_WORKSPACE_DEPENDENCY_INSTALLER_H

#include "extensions/extension.h"
#include "workspace/process_task.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace sheen {

struct installer_step {
    QString group_id;
    QString group_title;
    process_spec spec;
    event_protocol protocol = event_protocol::NONE;
};

struct installer_listener {
    QPointer<QObject> context;
    std::function<void(QByteArrayView bytes)> on_log_output;
    std::function<void()> on_state_changed;
    std::function<void(const QString& extension_id, bool succeeded, const QString& failure_message)> on_finished;
};

/* Process-wide installer driving sequential stlxpkg and post-install commands without lock collisions. */
class dependency_installer : public QObject {
public:
    explicit dependency_installer(QObject* parent = nullptr);
    ~dependency_installer() override;

    bool start_extension_install(const extension_descriptor& descriptor, QString* error);
    void cancel();

    bool is_running() const { return m_is_running; }
    const QString& target_extension_id() const { return m_extension_id; }
    const QString& progress_label() const { return m_progress_label; }
    int progress_percent() const { return m_progress_percent; }
    const QString& last_failed_extension_id() const { return m_last_failed_extension_id; }
    const QString& last_failure_message() const { return m_last_failure_message; }
    const QByteArray& log_history() const { return m_log_history; }

    void add_listener(QObject* context,
                      std::function<void(QByteArrayView)> on_log,
                      std::function<void()> on_state,
                      std::function<void(const QString&, bool, const QString&)> on_done);
    void remove_listener(QObject* context);

    std::function<void()> on_dependencies_installed;

private:
    void append_log(QByteArrayView bytes);
    void notify_state_changed();
    void notify_finished(const QString& extension_id, bool succeeded, const QString& failure_message);
    void run_next_step();
    void handle_event(event_protocol protocol, const QStringList& fields);
    void finish_step(const process_exit& exit_info);

    QString m_extension_id;
    QList<installer_step> m_steps;
    int m_step_index = 0;
    bool m_is_running = false;
    bool m_was_cancelled = false;
    QString m_progress_label;
    int m_progress_percent = -1;
    QString m_failure_message;
    QString m_last_failed_extension_id;
    QString m_last_failure_message;
    QByteArray m_log_history;
    QList<installer_listener> m_listeners;
    process_task m_task;
    std::unique_ptr<event_stream_runner> m_event_runner;
};

} // namespace sheen

#endif
