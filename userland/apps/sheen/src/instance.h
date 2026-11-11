#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>

class QSocketNotifier;

/* One Sheen per session: the first launch listens here and later launches hand it their files */
#define INSTANCE_SOCKET_PATH "/tmp/sheen.sock"

struct instance_client {
    QSocketNotifier* notifier;
    QByteArray received;
};

/* The running Sheen's end of the socket, removed again as it exits */
class instance_server : public QObject {
public:
    explicit instance_server(QObject* parent = nullptr);
    ~instance_server() override;

    /* Takes the socket over, replacing one a dead instance left. False leaves this launch without one */
    bool listen();

    std::function<void(const QStringList& paths)> on_paths;

private:
    void accept_clients();
    void read_client(int fd);
    void drop_client(int fd);

    int m_listen_fd = -1;
    QHash<int, instance_client> m_clients;
};

/* Hands the paths to a running Sheen as absolute paths. False when none answers, so this launch becomes the instance */
bool hand_to_running_instance(const QStringList& paths);
