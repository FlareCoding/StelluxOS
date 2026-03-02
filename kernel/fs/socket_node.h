#ifndef STELLUX_FS_SOCKET_NODE_H
#define STELLUX_FS_SOCKET_NODE_H

#include "fs/node.h"
#include "rc/strong_ref.h"

namespace socket { struct listener_state; }

namespace fs {

class socket_node : public node {
public:
    socket_node(instance* fs, const char* name);

    int32_t getattr(vattr* attr) override;

    socket::listener_state* get_listener() const { return m_listener.ptr(); }
    void set_listener(rc::strong_ref<socket::listener_state> ls);

private:
    rc::strong_ref<socket::listener_state> m_listener;
};

} // namespace fs

#endif // STELLUX_FS_SOCKET_NODE_H
