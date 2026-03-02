#include "fs/socket_node.h"
#include "fs/fs.h"
#include "socket/listener.h"

namespace fs {

socket_node::socket_node(instance* fs, const char* name)
    : node(node_type::socket, fs, name) {}

int32_t socket_node::getattr(vattr* attr) {
    if (!attr) {
        return fs::ERR_INVAL;
    }
    attr->type = node_type::socket;
    attr->size = 0;
    return fs::OK;
}

void socket_node::set_listener(rc::strong_ref<socket::listener_state> ls) {
    m_listener = static_cast<rc::strong_ref<socket::listener_state>&&>(ls);
}

} // namespace fs
