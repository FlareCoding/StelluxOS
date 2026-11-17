#ifndef SHEEN_WORKSPACE_PROCESS_TASK_H
#define SHEEN_WORKSPACE_PROCESS_TASK_H

#include "extensions/extension.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QSocketNotifier;
class QTimer;

namespace sheen {

enum class process_io_mode {
    PIPES, // separate stdin, stdout, and stderr pipes (mcufw, serial, stlxpkg)
    PTY,   // single pseudo-terminal via pty_create, child stdout and stderr merge into on_stdout
};

struct process_spec {
    QString program;
    QStringList arguments;
    QString working_directory;
    process_io_mode io_mode = process_io_mode::PIPES;
    bool opens_stdin = false;
};

struct process_exit {
    int exit_code = 0;
    int term_signal = 0;
    bool was_stopped = false;
};

/* Non-blocking child process wrapper around proc_create, POSIX pipe/pty QSocketNotifiers, and SIGTERM shutdown. */
class process_task : public QObject {
public:
    explicit process_task(QObject* parent = nullptr);
    ~process_task() override;

    bool start(const process_spec& spec, QString* error);
    bool write_stdin(QByteArrayView bytes);

    /* Sends kill(m_pid, SIGTERM) and starts a 1500 ms timer that falls back to proc_kill(m_proc_handle). */
    void stop();
    bool is_running() const { return m_proc_handle >= 0; }
    int pid() const { return m_pid; }

    std::function<void(QByteArrayView bytes)> on_stdout;
    std::function<void(QByteArrayView bytes)> on_stderr;
    std::function<void(const process_exit& result)> on_finished;

private:
    void handle_pipe_readable(int& fd, QSocketNotifier*& notifier,
                              const std::function<void(QByteArrayView)>& callback);
    void read_available_bytes(int& fd, QSocketNotifier*& notifier,
                              const std::function<void(QByteArrayView)>& callback);
    void poll_child_exit();
    void release_pipe_notifiers();

    int m_proc_handle = -1; // Stellux kernel process handle, reaped exclusively by proc_wait once started
    int m_pid = -1;
    int m_stdin_fd = -1;
    int m_stdout_fd = -1;
    int m_stderr_fd = -1;
    bool m_was_stopped = false;
    bool m_is_reaping = false;
    QSocketNotifier* m_stdout_notifier = nullptr;
    QSocketNotifier* m_stderr_notifier = nullptr;
    QTimer* m_reap_timer = nullptr;
    QTimer* m_kill_timer = nullptr;
};

/* Splits streamed stdout or stderr lines for SPACE_STLXPKG, TSV_MCUFW, or LINES event protocols. */
class event_stream_runner {
public:
    explicit event_stream_runner(event_protocol protocol);

    void feed(QByteArrayView chunk, const std::function<void(const QStringList& fields)>& on_event);
    void flush(const std::function<void(const QStringList& fields)>& on_event);

private:
    event_line_stream m_stream;
};

} // namespace sheen

#endif
