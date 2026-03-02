#include "resource/providers/socket_provider.h"
#include "dynpriv/dynpriv.h"
#include "mm/heap.h"

namespace resource::socket_provider {

namespace {

struct socket_resource_impl {
    net::unix_stream::stream_socket* socket;
};

int32_t map_net_error_to_resource(int32_t net_err) {
    switch (net_err) {
        case net::unix_stream::OK:
            return OK;
        case net::unix_stream::ERR_INVAL:
            return ERR_INVAL;
        case net::unix_stream::ERR_NOMEM:
            return ERR_NOMEM;
        case net::unix_stream::ERR_ADDRINUSE:
            return ERR_ADDRINUSE;
        case net::unix_stream::ERR_AGAIN:
            return ERR_AGAIN;
        case net::unix_stream::ERR_NOTCONN:
            return ERR_NOTCONN;
        case net::unix_stream::ERR_CONNREFUSED:
            return ERR_CONNREFUSED;
        case net::unix_stream::ERR_PIPE:
            return ERR_PIPE;
        case net::unix_stream::ERR_OPNOTSUPP:
            return ERR_OPNOTSUPP;
        case net::unix_stream::ERR_AFNOSUPPORT:
            return ERR_AFNOSUPPORT;
        case net::unix_stream::ERR_PROTONOSUPPORT:
            return ERR_PROTONOSUPPORT;
        case net::unix_stream::ERR_BADF:
            return ERR_BADF;
        default:
            return ERR_IO;
    }
}

ssize_t map_net_io_to_resource(ssize_t net_rc) {
    if (net_rc >= 0) {
        return net_rc;
    }
    return map_net_error_to_resource(static_cast<int32_t>(net_rc));
}

__PRIVILEGED_CODE int32_t create_resource_from_socket(
    net::unix_stream::stream_socket* socket,
    resource_object** out_obj
) {
    if (!socket || !out_obj) {
        return ERR_INVAL;
    }

    auto* impl = heap::kalloc_new<socket_resource_impl>();
    if (!impl) {
        return ERR_NOMEM;
    }
    impl->socket = socket;

    auto* obj = heap::kalloc_new<resource_object>();
    if (!obj) {
        heap::kfree_delete(impl);
        return ERR_NOMEM;
    }

    obj->type = resource_type::SOCKET;
    obj->impl = impl;
    *out_obj = obj;
    return OK;
}

__PRIVILEGED_CODE ssize_t socket_read(resource_object* obj, void* kdst, size_t count) {
    if (!obj || obj->type != resource_type::SOCKET || !obj->impl || !kdst) {
        return ERR_INVAL;
    }

    auto* impl = static_cast<socket_resource_impl*>(obj->impl);
    return map_net_io_to_resource(net::unix_stream::recv(impl->socket, kdst, count));
}

__PRIVILEGED_CODE ssize_t socket_write(resource_object* obj, const void* ksrc, size_t count) {
    if (!obj || obj->type != resource_type::SOCKET || !obj->impl || !ksrc) {
        return ERR_INVAL;
    }

    auto* impl = static_cast<socket_resource_impl*>(obj->impl);
    return map_net_io_to_resource(net::unix_stream::send(impl->socket, ksrc, count));
}

__PRIVILEGED_CODE void socket_close(resource_object* obj) {
    if (!obj || obj->type != resource_type::SOCKET || !obj->impl) {
        return;
    }

    auto* impl = static_cast<socket_resource_impl*>(obj->impl);
    if (impl->socket) {
        net::unix_stream::close(impl->socket);
        net::unix_stream::release(impl->socket);
        impl->socket = nullptr;
    }

    heap::kfree_delete(impl);
    obj->impl = nullptr;
}

const resource_ops g_socket_ops = {
    socket_read,
    socket_write,
    socket_close,
};

__PRIVILEGED_CODE int32_t with_socket(resource_object* obj, net::unix_stream::stream_socket** out_socket) {
    if (!obj || !out_socket) {
        return ERR_INVAL;
    }
    if (obj->type != resource_type::SOCKET || !obj->impl || !obj->ops) {
        return ERR_NOTSOCK;
    }

    auto* impl = static_cast<socket_resource_impl*>(obj->impl);
    if (!impl->socket) {
        return ERR_BADF;
    }
    *out_socket = impl->socket;
    return OK;
}

} // namespace

__PRIVILEGED_CODE static int32_t create_stream_socket_resource_elevated(
    bool nonblocking,
    resource_object** out_obj
) {
    if (!out_obj) {
        return ERR_INVAL;
    }

    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = map_net_error_to_resource(
        net::unix_stream::create_socket(nonblocking, &socket)
    );
    if (rc != OK) {
        return rc;
    }

    resource_object* obj = nullptr;
    rc = create_resource_from_socket(socket, &obj);
    if (rc != OK) {
        net::unix_stream::close(socket);
        net::unix_stream::release(socket);
        return rc;
    }

    obj->ops = &g_socket_ops;
    *out_obj = obj;
    return OK;
}

__PRIVILEGED_CODE static int32_t bind_elevated(
    resource_object* obj,
    const net::unix_stream::socket_path& path
) {
    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = with_socket(obj, &socket);
    if (rc != OK) {
        return rc;
    }

    return map_net_error_to_resource(net::unix_stream::bind(socket, path));
}

__PRIVILEGED_CODE static int32_t listen_elevated(resource_object* obj, uint32_t backlog) {
    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = with_socket(obj, &socket);
    if (rc != OK) {
        return rc;
    }

    return map_net_error_to_resource(net::unix_stream::listen(socket, backlog));
}

__PRIVILEGED_CODE static int32_t connect_elevated(
    resource_object* obj,
    const net::unix_stream::socket_path& path
) {
    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = with_socket(obj, &socket);
    if (rc != OK) {
        return rc;
    }

    return map_net_error_to_resource(net::unix_stream::connect(socket, path));
}

__PRIVILEGED_CODE static int32_t accept_elevated(
    resource_object* listener_obj,
    resource_object** out_obj
) {
    if (!out_obj) {
        return ERR_INVAL;
    }

    net::unix_stream::stream_socket* listener = nullptr;
    int32_t rc = with_socket(listener_obj, &listener);
    if (rc != OK) {
        return rc;
    }

    net::unix_stream::stream_socket* accepted = nullptr;
    rc = map_net_error_to_resource(net::unix_stream::accept(listener, &accepted));
    if (rc != OK) {
        return rc;
    }

    resource_object* out = nullptr;
    rc = create_resource_from_socket(accepted, &out);
    if (rc != OK) {
        net::unix_stream::close(accepted);
        net::unix_stream::release(accepted);
        return rc;
    }

    out->ops = &g_socket_ops;
    *out_obj = out;
    return OK;
}

__PRIVILEGED_CODE static int32_t get_nonblocking_elevated(
    resource_object* obj,
    bool* out_nonblocking
) {
    if (!out_nonblocking) {
        return ERR_INVAL;
    }

    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = with_socket(obj, &socket);
    if (rc != OK) {
        return rc;
    }

    return map_net_error_to_resource(net::unix_stream::get_nonblocking(socket, out_nonblocking));
}

__PRIVILEGED_CODE static int32_t set_nonblocking_elevated(
    resource_object* obj,
    bool nonblocking
) {
    net::unix_stream::stream_socket* socket = nullptr;
    int32_t rc = with_socket(obj, &socket);
    if (rc != OK) {
        return rc;
    }

    return map_net_error_to_resource(net::unix_stream::set_nonblocking(socket, nonblocking));
}

int32_t create_stream_socket_resource(
    bool nonblocking,
    resource_object** out_obj
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = create_stream_socket_resource_elevated(nonblocking, out_obj);
    });
    return rc;
}

int32_t bind(
    resource_object* obj,
    const net::unix_stream::socket_path& path
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = bind_elevated(obj, path);
    });
    return rc;
}

int32_t listen(resource_object* obj, uint32_t backlog) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = listen_elevated(obj, backlog);
    });
    return rc;
}

int32_t connect(
    resource_object* obj,
    const net::unix_stream::socket_path& path
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = connect_elevated(obj, path);
    });
    return rc;
}

int32_t accept(
    resource_object* listener_obj,
    resource_object** out_obj
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = accept_elevated(listener_obj, out_obj);
    });
    return rc;
}

int32_t get_nonblocking(
    resource_object* obj,
    bool* out_nonblocking
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = get_nonblocking_elevated(obj, out_nonblocking);
    });
    return rc;
}

int32_t set_nonblocking(
    resource_object* obj,
    bool nonblocking
) {
    int32_t rc = ERR_IO;
    RUN_ELEVATED({
        rc = set_nonblocking_elevated(obj, nonblocking);
    });
    return rc;
}

} // namespace resource::socket_provider
