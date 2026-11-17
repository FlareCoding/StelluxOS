#include "workspace/dependency_installer.h"

#include <unistd.h>

namespace sheen {

static constexpr double BYTES_PER_MB = 1000000.0;
static constexpr qsizetype MAX_LOG_HISTORY_BYTES = 256 * 1024;

dependency_installer::dependency_installer(QObject* parent) : QObject(parent) {
    m_task.on_stdout = [this](QByteArrayView bytes) {
        append_log(bytes);
        if (m_event_runner) {
            event_protocol proto = m_steps.value(m_step_index).protocol;
            m_event_runner->feed(bytes, [this, proto](const QStringList& fields) {
                handle_event(proto, fields);
            });
        }
    };

    m_task.on_stderr = [this](QByteArrayView bytes) {
        append_log(bytes);
    };

    m_task.on_finished = [this](const process_exit& exit_info) {
        finish_step(exit_info);
    };
}

dependency_installer::~dependency_installer() {
    cancel();
}

void dependency_installer::add_listener(QObject* context,
                                        std::function<void(QByteArrayView)> on_log,
                                        std::function<void()> on_state,
                                        std::function<void(const QString&, bool, const QString&)> on_done) {
    remove_listener(context);
    m_listeners.append({ context, std::move(on_log), std::move(on_state), std::move(on_done) });
}

void dependency_installer::remove_listener(QObject* context) {
    for (qsizetype i = m_listeners.size() - 1; i >= 0; i--) {
        if (!m_listeners.at(i).context || m_listeners.at(i).context == context) {
            m_listeners.removeAt(i);
        }
    }
}

void dependency_installer::append_log(QByteArrayView bytes) {
    if (bytes.isEmpty()) {
        return;
    }

    m_log_history.append(bytes.data(), bytes.size());
    if (m_log_history.size() > MAX_LOG_HISTORY_BYTES) {
        m_log_history.remove(0, m_log_history.size() - MAX_LOG_HISTORY_BYTES);
    }

    QList<installer_listener> snapshot = m_listeners;
    for (const installer_listener& listener : snapshot) {
        if (listener.context && listener.on_log_output) {
            listener.on_log_output(bytes);
        }
    }
}

void dependency_installer::notify_state_changed() {
    QList<installer_listener> snapshot = m_listeners;
    for (const installer_listener& listener : snapshot) {
        if (listener.context && listener.on_state_changed) {
            listener.on_state_changed();
        }
    }
}

void dependency_installer::notify_finished(const QString& extension_id, bool succeeded,
                                           const QString& failure_message) {
    QList<installer_listener> snapshot = m_listeners;
    for (const installer_listener& listener : snapshot) {
        if (listener.context && listener.on_finished) {
            listener.on_finished(extension_id, succeeded, failure_message);
        }
    }
}

bool dependency_installer::start_extension_install(const extension_descriptor& descriptor, QString* error) {
    if (m_is_running) {
        if (error) {
            *error = QStringLiteral("Another package installation is already running.");
        }

        return false;
    }

    m_steps.clear();
    m_step_index = 0;
    m_was_cancelled = false;
    m_failure_message.clear();

    m_last_failed_extension_id.clear();
    m_last_failure_message.clear();
    m_log_history.clear();
    m_extension_id = QString::fromLatin1(descriptor.id);

    for (const dependency_group& group : descriptor.dependencies) {
        QStringList missing_packages;
        for (const package_requirement& req : group.packages) {
            bool present = req.probe_path && req.probe_path[0] != '\0' && ::access(req.probe_path, F_OK) == 0;
            if (!present && req.package_name && req.package_name[0] != '\0') {
                missing_packages.append(QString::fromLatin1(req.package_name));
            }
        }

        if (!missing_packages.isEmpty()) {
            QStringList args = { QStringLiteral("--events"), QStringLiteral("install") };
            args.append(missing_packages);
            process_spec spec;
            spec.program = QStringLiteral("/bin/stlxpkg");
            spec.arguments = args;
            m_steps.append({ QString::fromLatin1(group.id), QString::fromLatin1(group.title),
                             spec, event_protocol::SPACE_STLXPKG });
        }

        bool post_needed = !group.post_install_argv.empty()
                        && (!group.post_install_probe_path || group.post_install_probe_path[0] == '\0'
                            || ::access(group.post_install_probe_path, F_OK) != 0);
        if (post_needed) {
            process_spec spec;
            spec.program = QString::fromLatin1(group.post_install_argv[0]);
            for (size_t i = 1; i < group.post_install_argv.size(); i++) {
                spec.arguments.append(QString::fromLatin1(group.post_install_argv[i]));
            }

            m_steps.append({ QString::fromLatin1(group.id), QString::fromLatin1(group.title),
                             spec, group.post_install_protocol });
        }
    }

    if (m_steps.isEmpty()) {
        if (on_dependencies_installed) {
            on_dependencies_installed();
        }

        notify_finished(m_extension_id, true, QString());
        return true;
    }

    m_is_running = true;
    run_next_step();

    return true;
}

void dependency_installer::cancel() {
    if (!m_is_running) {
        return;
    }

    m_was_cancelled = true;
    m_task.stop();
}

void dependency_installer::run_next_step() {
    if (m_step_index >= m_steps.size()) {
        m_is_running = false;
        m_progress_label.clear();
        m_progress_percent = -1;
        if (on_dependencies_installed) {
            on_dependencies_installed();
        }

        notify_finished(m_extension_id, true, QString());
        notify_state_changed();
        return;
    }

    const installer_step& step = m_steps.at(m_step_index);
    m_event_runner = std::make_unique<event_stream_runner>(step.protocol);
    m_progress_label = QStringLiteral("Installing %1...").arg(step.group_title);
    m_progress_percent = 0;
    notify_state_changed();

    QString start_error;
    if (!m_task.start(step.spec, &start_error)) {
        m_is_running = false;
        m_progress_label.clear();
        m_progress_percent = -1;

        m_last_failed_extension_id = m_extension_id;
        m_last_failure_message = start_error;
        notify_finished(m_extension_id, false, start_error);
        notify_state_changed();
    }
}

void dependency_installer::handle_event(event_protocol protocol, const QStringList& fields) {
    if (fields.isEmpty()) {
        return;
    }

    const QString kind = fields.at(0);
    if (kind == QLatin1String("start") && fields.size() >= 3) {
        m_progress_label = QStringLiteral("Downloading %1 %2...").arg(fields.at(1), fields.at(2));
        m_progress_percent = 0;
        notify_state_changed();
        return;
    }

    if (kind == QLatin1String("progress") && fields.size() >= 4) {
        qint64 done = fields.at(2).toLongLong();
        qint64 total = fields.at(3).toLongLong();
        m_progress_percent = total > 0 ? static_cast<int>(done * 100 / total) : -1;

        QString done_mb = QString::number(static_cast<double>(done) / BYTES_PER_MB, 'f', 1);
        QString total_mb = QString::number(static_cast<double>(total) / BYTES_PER_MB, 'f', 1);
        m_progress_label = total > 0 ? QStringLiteral("%1 (%2 of %3 MB)").arg(fields.at(1), done_mb, total_mb)
                                     : QStringLiteral("%1 (%2 MB)").arg(fields.at(1), done_mb);
        notify_state_changed();
        return;
    }

    if (kind == QLatin1String("commit") && fields.size() >= 2) {
        m_progress_label = QStringLiteral("Unpacking %1...").arg(fields.at(1));
        m_progress_percent = 100;
        notify_state_changed();
        return;
    }

    if (kind == QLatin1String("failed")) {
        if (protocol == event_protocol::SPACE_STLXPKG && fields.size() >= 4) {
            m_failure_message = QStringLiteral("%1 (%2): %3").arg(fields.at(1), fields.at(2), fields.at(3));
        } else if (fields.size() >= 3) {
            m_failure_message = QStringLiteral("%1: %2").arg(fields.at(1), fields.at(2));
        } else {
            m_failure_message = fields.join(QStringLiteral(" "));
        }
    }
}

void dependency_installer::finish_step(const process_exit& exit_info) {
    if (m_event_runner && m_step_index < m_steps.size()) {
        event_protocol proto = m_steps.at(m_step_index).protocol;
        m_event_runner->flush([this, proto](const QStringList& fields) {
            handle_event(proto, fields);
        });
    }

    m_event_runner.reset();

    if (m_was_cancelled || exit_info.was_stopped) {
        m_is_running = false;
        m_progress_label.clear();
        m_progress_percent = -1;

        m_last_failed_extension_id = m_extension_id;
        m_last_failure_message = QStringLiteral("Installation cancelled.");
        notify_finished(m_extension_id, false, m_last_failure_message);
        notify_state_changed();
        return;
    }

    if (exit_info.exit_code != 0 || !m_failure_message.isEmpty()) {
        m_is_running = false;
        m_progress_label.clear();
        m_progress_percent = -1;

        m_last_failed_extension_id = m_extension_id;
        m_last_failure_message = m_failure_message.isEmpty()
                               ? QStringLiteral("Installer exited with status %1.").arg(exit_info.exit_code)
                               : m_failure_message;
        notify_finished(m_extension_id, false, m_last_failure_message);
        notify_state_changed();
        return;
    }

    m_step_index++;
    run_next_step();
}

} // namespace sheen
