#include "instance.h"

#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static constexpr int LISTEN_BACKLOG = 8;
static constexpr size_t READ_CHUNK = 4096;

// A handover is a list of paths, so a client sending more than this is not Sheen
static constexpr qsizetype MESSAGE_LIMIT = 1024 * 1024;

static sockaddr_un socket_address() {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, INSTANCE_SOCKET_PATH, sizeof(address.sun_path) - 1);
    return address;
}

static bool write_all(int fd, const QByteArray& bytes) {
    qsizetype written = 0;
    while (written < bytes.size()) {
        ssize_t count = ::write(fd, bytes.constData() + written, static_cast<size_t>(bytes.size() - written));
        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count <= 0) {
            return false;
        }

        written += count;
    }

    return true;
}

// Each path ends in a NUL, the one byte no path holds
bool hand_to_running_instance(const QStringList& paths) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_un address = socket_address();
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }

    QByteArray message;
    for (const QString& path : paths) {
        message += QFile::encodeName(QFileInfo(path).absoluteFilePath());
        message += '\0';
    }

    bool is_sent = write_all(fd, message);
    ::close(fd);
    return is_sent;
}

instance_server::instance_server(QObject* parent) : QObject(parent) {
}

instance_server::~instance_server() {
    for (auto client = m_clients.cbegin(); client != m_clients.cend(); ++client) {
        ::close(client.key());
    }

    if (m_listen_fd >= 0) {
        ::close(m_listen_fd);
        ::unlink(INSTANCE_SOCKET_PATH);
    }
}

bool instance_server::listen() {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_un address = socket_address();
    ::unlink(INSTANCE_SOCKET_PATH);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, LISTEN_BACKLOG) != 0) {
        ::close(fd);
        return false;
    }

    m_listen_fd = fd;
    auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] { accept_clients(); });
    return true;
}

void instance_server::accept_clients() {
    while (true) {
        int fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (fd < 0) {
            return;
        }

        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        auto* notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        m_clients.insert(fd, { notifier, QByteArray() });
        connect(notifier, &QSocketNotifier::activated, this, [this, fd] { read_client(fd); });
    }
}

// A client's paths arrive whole once it closes its end
void instance_server::read_client(int fd) {
    instance_client& client = m_clients[fd];
    char chunk[READ_CHUNK];
    while (true) {
        ssize_t count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            client.received.append(chunk, count);
            if (client.received.size() > MESSAGE_LIMIT) {
                drop_client(fd);
                return;
            }

            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        break;
    }

    QByteArray received = client.received;
    drop_client(fd);

    QStringList paths;
    for (const QByteArray& path : received.split('\0')) {
        if (!path.isEmpty()) {
            paths.append(QFile::decodeName(path));
        }
    }

    if (on_paths) {
        on_paths(paths);
    }
}

// The notifier may be the one whose signal is running, so it goes once control returns to the event loop
void instance_server::drop_client(int fd) {
    instance_client client = m_clients.take(fd);
    client.notifier->setEnabled(false);
    client.notifier->deleteLater();
    ::close(fd);
}
