#include "workspace/process_task.h"
#include "app/sheen_app.h"

#include <QFile>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>

#include <stlx/proc.h>

extern "C" {
#include <stlx/pty.h>
}

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace sheen {

static constexpr size_t READ_CHUNK_BYTES = 4096;
static constexpr int REAP_POLL_MS = 20;
static constexpr int KILL_FALLBACK_MS = 1500;
static constexpr int WRITE_POLL_MS = 50;
static constexpr unsigned short DEFAULT_PTY_ROWS = 40;
static constexpr unsigned short DEFAULT_PTY_COLS = 120;

static void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

static QByteArray resolve_program_path(const QString& program) {
    QByteArray encoded = QFile::encodeName(program);
    if (encoded.isEmpty()) {
        return {};
    }

    if (encoded.contains('/')) {
        return ::access(encoded.constData(), X_OK) == 0 ? encoded : QByteArray();
    }

    const char* path_env = std::getenv("PATH");
    QByteArray search_dirs = path_env && path_env[0] ? QByteArray(path_env)
                                                     : QByteArrayLiteral("/usr/local/bin:/bin:/usr/bin");
    for (const QByteArray& dir : search_dirs.split(':')) {
        QByteArray candidate = dir.isEmpty() ? encoded : dir + '/' + encoded;
        if (::access(candidate.constData(), X_OK) == 0) {
            return candidate;
        }
    }

    return {};
}

static int create_in_directory(const QByteArray& program, std::vector<const char*>& argv,
                               const QString& working_directory, QString* error) {
    char saved_dir[PATH_MAX];
    QByteArray target_dir = QFile::encodeName(working_directory);
    if (!target_dir.isEmpty()) {
        if (!::getcwd(saved_dir, sizeof(saved_dir)) || ::chdir(target_dir.constData()) != 0) {
            if (error) {
                *error = QStringLiteral("Cannot enter %1: %2")
                             .arg(working_directory, QString::fromLocal8Bit(std::strerror(errno)));
            }

            return -1;
        }
    }

    int handle = proc_create(program.constData(), argv.data());
    int create_errno = errno;

    if (!target_dir.isEmpty() && ::chdir(saved_dir) != 0) {
        if (handle >= 0) {
            ::close(handle);
        }

        if (error) {
            *error = QStringLiteral("Cannot restore working directory: %1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }

        return -1;
    }

    if (handle < 0 && error) {
        *error = QStringLiteral("Cannot start %1: %2")
                     .arg(QFile::decodeName(program), QString::fromLocal8Bit(std::strerror(create_errno)));
    }

    return handle;
}

process_task::process_task(QObject* parent)
    : QObject(parent), m_reap_timer(new QTimer(this)), m_kill_timer(new QTimer(this)) {
    m_reap_timer->setInterval(REAP_POLL_MS);
    connect(m_reap_timer, &QTimer::timeout, this, [this] { poll_child_exit(); });

    m_kill_timer->setSingleShot(true);
    m_kill_timer->setInterval(KILL_FALLBACK_MS);
    connect(m_kill_timer, &QTimer::timeout, this, [this] {
        if (m_proc_handle >= 0) {
            proc_kill(m_proc_handle);
            poll_child_exit();
        }
    });
}

process_task::~process_task() {
    on_stdout = nullptr;
    on_stderr = nullptr;
    on_finished = nullptr;

    int remaining_ms = m_kill_timer->isActive() ? qMax(0, m_kill_timer->remainingTime()) : KILL_FALLBACK_MS;
    m_reap_timer->stop();
    m_kill_timer->stop();
    release_pipe_notifiers();
    close_fd(m_stdin_fd);

    if (m_proc_handle < 0) {
        return;
    }

    int handle = m_proc_handle;
    int pid = m_pid;
    bool already_stopped = m_was_stopped;
    m_proc_handle = -1;
    m_pid = -1;
    m_was_stopped = false;

    if (!already_stopped && pid > 0) {
        ::kill(pid, SIGTERM);
    }

    sheen_app::reap_orphan_process(handle, pid, remaining_ms);
}

bool process_task::start(const process_spec& spec, QString* error) {
    if (m_proc_handle >= 0 || m_is_reaping) {
        if (error) {
            *error = QStringLiteral("Task is already running");
        }

        return false;
    }

    QByteArray program = resolve_program_path(spec.program);
    if (program.isEmpty()) {
        if (error) {
            *error = QStringLiteral("%1 is not installed").arg(spec.program);
        }

        return false;
    }

    QList<QByteArray> encoded_args;
    std::vector<const char*> argv;
    for (const QString& argument : spec.arguments) {
        encoded_args.append(QFile::encodeName(argument));
    }

    for (const QByteArray& argument : encoded_args) {
        argv.push_back(argument.constData());
    }

    argv.push_back(nullptr);

    int child_stdin = -1;
    int child_stdout = -1;
    int child_stderr = -1;
    int parent_stdin = -1;
    int parent_stdout = -1;
    int parent_stderr = -1;

    if (spec.io_mode == process_io_mode::PTY) {
        int master_fd = -1;
        int slave_fd = -1;
        if (pty_create(&master_fd, &slave_fd) < 0) {
            if (error) {
                *error = QStringLiteral("Cannot open pseudo-terminal: %1")
                             .arg(QString::fromLocal8Bit(std::strerror(errno)));
            }

            return false;
        }

        ::fcntl(master_fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(master_fd, F_SETFL, O_NONBLOCK);
        ::fcntl(slave_fd, F_SETFD, FD_CLOEXEC);

        struct winsize window_size = { DEFAULT_PTY_ROWS, DEFAULT_PTY_COLS, 0, 0 };
        ::ioctl(master_fd, TIOCSWINSZ, &window_size);

        parent_stdout = master_fd;
        if (spec.opens_stdin) {
            parent_stdin = ::fcntl(master_fd, F_DUPFD_CLOEXEC, 0);
            if (parent_stdin < 0) {
                close_fd(master_fd);
                close_fd(slave_fd);
                if (error) {
                    *error = QStringLiteral("Cannot duplicate pseudo-terminal descriptor");
                }

                return false;
            }
        }

        child_stdin = slave_fd;
        child_stdout = slave_fd;
        child_stderr = slave_fd;
    } else {
        int stdin_pipe[2] = { -1, -1 };
        int stdout_pipe[2] = { -1, -1 };
        int stderr_pipe[2] = { -1, -1 };
        bool stdin_ok = spec.opens_stdin ? ::pipe2(stdin_pipe, O_CLOEXEC) == 0
                                         : (stdin_pipe[0] = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0;
        if (!stdin_ok || ::pipe2(stdout_pipe, O_CLOEXEC) != 0 || ::pipe2(stderr_pipe, O_CLOEXEC) != 0) {
            close_fd(stdin_pipe[0]);
            close_fd(stdin_pipe[1]);
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stderr_pipe[0]);
            close_fd(stderr_pipe[1]);

            if (error) {
                *error = QStringLiteral("Cannot create process pipes: %1")
                             .arg(QString::fromLocal8Bit(std::strerror(errno)));
            }

            return false;
        }

        ::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
        if (stdin_pipe[1] >= 0) {
            ::fcntl(stdin_pipe[1], F_SETFL, O_NONBLOCK);
        }

        child_stdin = stdin_pipe[0];
        parent_stdin = stdin_pipe[1];
        parent_stdout = stdout_pipe[0];
        child_stdout = stdout_pipe[1];
        parent_stderr = stderr_pipe[0];
        child_stderr = stderr_pipe[1];
    }

    auto close_child_ends = [&] {
        if (spec.io_mode == process_io_mode::PTY) {
            close_fd(child_stdin);
            child_stdout = -1;
            child_stderr = -1;
            return;
        }

        close_fd(child_stdin);
        close_fd(child_stdout);
        close_fd(child_stderr);
    };

    int handle = create_in_directory(program, argv, spec.working_directory, error);
    if (handle < 0) {
        close_child_ends();
        close_fd(parent_stdin);
        close_fd(parent_stdout);
        close_fd(parent_stderr);
        return false;
    }

    ::fcntl(handle, F_SETFD, FD_CLOEXEC);

    process_info proc_details = {};
    bool is_started = proc_set_handle(handle, STDIN_FILENO, child_stdin) == 0
                   && proc_set_handle(handle, STDOUT_FILENO, child_stdout) == 0
                   && proc_set_handle(handle, STDERR_FILENO, child_stderr) == 0
                   && proc_info(handle, &proc_details) == 0
                   && proc_start(handle) == 0;
    int start_errno = errno;
    close_child_ends();

    if (!is_started) {
        ::close(handle);
        close_fd(parent_stdin);
        close_fd(parent_stdout);
        close_fd(parent_stderr);
        if (error) {
            *error = QStringLiteral("Cannot start %1: %2")
                         .arg(spec.program, QString::fromLocal8Bit(std::strerror(start_errno)));
        }

        return false;
    }

    m_proc_handle = handle;
    m_pid = proc_details.pid;
    m_stdin_fd = parent_stdin;
    m_stdout_fd = parent_stdout;
    m_stderr_fd = parent_stderr;
    m_was_stopped = false;

    m_stdout_notifier = new QSocketNotifier(m_stdout_fd, QSocketNotifier::Read, this);
    connect(m_stdout_notifier, &QSocketNotifier::activated, this, [this] {
        handle_pipe_readable(m_stdout_fd, m_stdout_notifier, on_stdout);
    });

    if (m_stderr_fd >= 0) {
        m_stderr_notifier = new QSocketNotifier(m_stderr_fd, QSocketNotifier::Read, this);
        connect(m_stderr_notifier, &QSocketNotifier::activated, this, [this] {
            handle_pipe_readable(m_stderr_fd, m_stderr_notifier, on_stderr);
        });
    }

    m_reap_timer->start();

    return true;
}

bool process_task::write_stdin(QByteArrayView bytes) {
    if (m_proc_handle < 0 || m_stdin_fd < 0) {
        return false;
    }

    qsizetype offset = 0;
    while (offset < bytes.size()) {
        ssize_t written = ::write(m_stdin_fd, bytes.data() + offset, static_cast<size_t>(bytes.size() - offset));
        if (written > 0) {
            offset += written;
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd writable = { m_stdin_fd, POLLOUT, 0 };
            if (::poll(&writable, 1, WRITE_POLL_MS) > 0) {
                continue;
            }
        }

        return false;
    }

    return true;
}

void process_task::stop() {
    if (m_proc_handle < 0) {
        return;
    }

    m_was_stopped = true;
    close_fd(m_stdin_fd);
    if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
    }

    if (!m_kill_timer->isActive()) {
        m_kill_timer->start();
    }

    poll_child_exit();
}

void process_task::handle_pipe_readable(int& fd, QSocketNotifier*& notifier,
                                        const std::function<void(QByteArrayView)>& callback) {
    QPointer<process_task> self(this);
    read_available_bytes(fd, notifier, callback);
    if (self) {
        poll_child_exit();
    }
}

void process_task::read_available_bytes(int& fd, QSocketNotifier*& notifier,
                                        const std::function<void(QByteArrayView)>& callback) {
    if (fd < 0) {
        return;
    }

    QPointer<process_task> self(this);
    char chunk[READ_CHUNK_BYTES];
    while (fd >= 0) {
        ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            if (callback) {
                std::function<void(QByteArrayView)> local_cb = callback;
                local_cb(QByteArrayView(chunk, count));
                if (!self) {
                    return;
                }
            }

            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
            notifier = nullptr;
        }

        close_fd(fd);
        return;
    }
}

void process_task::poll_child_exit() {
    if (m_proc_handle < 0 || m_is_reaping) {
        return;
    }

    pollfd status_poll = { m_proc_handle, POLLIN, 0 };
    if (::poll(&status_poll, 1, 0) <= 0 || !(status_poll.revents & (POLLIN | POLLHUP))) {
        return;
    }

    int handle = m_proc_handle;
    bool was_stopped = m_was_stopped;
    m_proc_handle = -1;
    m_pid = -1;
    m_was_stopped = false;
    m_is_reaping = true;

    m_reap_timer->stop();
    m_kill_timer->stop();

    QPointer<process_task> self(this);
    if (m_stdout_fd >= 0) {
        read_available_bytes(m_stdout_fd, m_stdout_notifier, on_stdout);
    }

    if (self && m_stderr_fd >= 0) {
        read_available_bytes(m_stderr_fd, m_stderr_notifier, on_stderr);
    }

    int wait_status = 0;
    while (proc_wait(handle, &wait_status) < 0 && errno == EINTR) {
    }

    if (!self) {
        return;
    }

    release_pipe_notifiers();
    close_fd(m_stdin_fd);
    m_is_reaping = false;

    process_exit exit_result;
    exit_result.was_stopped = was_stopped;
    if (STLX_WIFSIGNALED(wait_status)) {
        exit_result.term_signal = STLX_WTERMSIG(wait_status);
        exit_result.exit_code = 128 + exit_result.term_signal;
    } else {
        exit_result.exit_code = STLX_WEXITSTATUS(wait_status);
    }

    if (on_finished) {
        std::function<void(const process_exit&)> callback = on_finished;
        callback(exit_result);
    }
}

void process_task::release_pipe_notifiers() {
    if (m_stdout_notifier) {
        m_stdout_notifier->setEnabled(false);
        m_stdout_notifier->deleteLater();
        m_stdout_notifier = nullptr;
    }

    if (m_stderr_notifier) {
        m_stderr_notifier->setEnabled(false);
        m_stderr_notifier->deleteLater();
        m_stderr_notifier = nullptr;
    }

    close_fd(m_stdout_fd);
    close_fd(m_stderr_fd);
}

event_stream_runner::event_stream_runner(event_protocol protocol) : m_stream(protocol) {
}

void event_stream_runner::feed(QByteArrayView chunk, const std::function<void(const QStringList& fields)>& on_event) {
    m_stream.feed(std::string_view(chunk.data(), static_cast<size_t>(chunk.size())),
                  [&on_event](const std::vector<std::string>& raw_fields) {
                      if (!on_event) {
                          return;
                      }

                      QStringList converted;
                      converted.reserve(static_cast<qsizetype>(raw_fields.size()));
                      for (const std::string& field : raw_fields) {
                          converted.append(QString::fromUtf8(field.data(), static_cast<qsizetype>(field.size())));
                      }

                      on_event(converted);
                  });
}

void event_stream_runner::flush(const std::function<void(const QStringList& fields)>& on_event) {
    m_stream.flush([&on_event](const std::vector<std::string>& raw_fields) {
        if (!on_event) {
            return;
        }

        QStringList converted;
        converted.reserve(static_cast<qsizetype>(raw_fields.size()));
        for (const std::string& field : raw_fields) {
            converted.append(QString::fromUtf8(field.data(), static_cast<qsizetype>(field.size())));
        }

        on_event(converted);
    });
}

} // namespace sheen
