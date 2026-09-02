#include "fs/socket_node.h"
#include "socket/listener.h"

namespace fs {

socket_node::socket_node(instance* fs, const char* name)
    : node(node_type::socket, fs, name) {}

void socket_node::set_listener(rc::strong_ref<socket::listener_state> ls) {
    m_listener = static_cast<rc::strong_ref<socket::listener_state>&&>(ls);
}

} // namespace fs
